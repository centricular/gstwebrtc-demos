/*
 * Mountpoint object for webrtc clients (browsers) to connect to.
 * One of these per pipeline.
 */
#include "webrtc-mountpoint.h"
#include "webrtc-server.h"

#define UNUSED(x) (void)(x)

#define STUN_SERVER "stun://stun.l.google.com:19302"

gchar* get_string_from_json_object(JsonObject* object)
{
    JsonNode *root;
    JsonGenerator *generator;
    gchar *text;

    /* Make it the root node */
    root      = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();

    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
}


static void send_ice_candidate_message(
    GstElement* webrtc,
    guint       mlineindex,
    gchar*      candidate,
    void*       userdata)
{
    webrtc_session_t* session = (webrtc_session_t*)userdata;
    UNUSED(webrtc);
    gchar *text;
    JsonObject *ice, *msg;

    if (session->state < STREAM_NEGOTIATING) {
        g_printerr("Can't send ICE, not in call %u\n", session->client_uid);
        return;
    }

    ice = json_object_new        ();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member   (ice, "sdpMLineIndex", mlineindex);

    msg = json_object_new             ();
    json_object_set_object_member     (msg, "ice", ice);
    json_object_set_int_member        (msg, "client_uid", session->client_uid);
    text = get_string_from_json_object(msg);
    json_object_unref                 (msg);

    if (soup_websocket_connection_get_state(session->ws_conn_ref) !=
            SOUP_WEBSOCKET_STATE_OPEN) {
        g_printerr("No websocket connection for client %u\n",
                   session->client_uid);
        return;
    }

    soup_websocket_connection_send_text(session->ws_conn_ref, text);
    g_free                             (text);
}


static void send_sdp_offer(
    GstWebRTCSessionDescription* offer,
    void*                        userdata)
{
    webrtc_session_t* session = (webrtc_session_t*)userdata;
    gchar *text, *response_text;
    JsonObject *msg, *sdp;

    if (session->state < STREAM_NEGOTIATING) {
        g_printerr("Can't send offer, not in call %u\n", session->client_uid);
        return;
    }

    text = gst_sdp_message_as_text(offer->sdp);
    g_print("Sending offer:\n%s\n", text);

    sdp = json_object_new        ();
    json_object_set_string_member(sdp, "type", "offer");
    json_object_set_string_member(sdp, "sdp", text);
    g_free                       (text);

    msg = json_object_new        ();
    json_object_set_object_member(msg, "sdp", sdp);
    json_object_set_int_member   (msg, "client_uid", session->client_uid);

    response_text = get_string_from_json_object(msg);
    json_object_unref                          (msg);

    soup_websocket_connection_send_text(session->ws_conn_ref, response_text);
    g_free                             (response_text);
}


/* Offer created by our pipeline, to be sent to the peer */
static void on_offer_created(
    GstPromise* promise,
    void*       userdata)
{
    webrtc_session_t* session = (webrtc_session_t*)userdata;
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply;

    if (session->state != STREAM_NEGOTIATING) {
        g_printerr("Creating offer for session not in negotiation\n");
        return;
    }

    if (!session->webrtcbin_ref) {
        g_printerr("No webrtcbin associated with session\n");
        return;
    }

    if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
        g_printerr("No reply to promise\n");
        return;
    }
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer",
            GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    promise = gst_promise_new();

    g_signal_emit_by_name(session->webrtcbin_ref, "set-local-description",
                          offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref    (promise);

    /* Send offer to peer */
    send_sdp_offer(offer, session);
    gst_webrtc_session_description_free(offer);
}


static void on_negotiation_needed(
    GstElement* webrtcbin,
    void*       userdata)
{
    webrtc_session_t* session = (webrtc_session_t*)userdata;
    GstPromise *promise;

    session->state = STREAM_NEGOTIATING;
    promise = gst_promise_new_with_change_func(
        on_offer_created, userdata, NULL);
    g_signal_emit_by_name(webrtcbin, "create-offer", NULL, promise);
}


static void data_channel_on_error(GObject* dc, void* userdata)
{
    UNUSED(dc);
    UNUSED(userdata);
    g_printerr("Data channel error\n");
}


static void data_channel_on_open(GObject* dc, void* userdata)
{
    UNUSED(dc);
    UNUSED(userdata);
    GBytes *bytes = g_bytes_new ("data", strlen("data"));
    g_print ("data channel opened\n");
    g_signal_emit_by_name (dc, "send-string", "Hi! from GStreamer");
    g_signal_emit_by_name (dc, "send-data", bytes);
    g_bytes_unref (bytes);
}


static void data_channel_on_close(GObject* dc, void* userdata)
{
    UNUSED(dc);
    UNUSED(userdata);
    g_printerr("Data channel closed\n");
}


static void data_channel_on_message_string(
    GObject* dc,
    gchar*   str,
    void*    userdata)
{
    UNUSED(dc);
    UNUSED(userdata);
    g_print ("Received data channel message: %s\n", str);
}


static void connect_data_channel_signals(
    GObject* data_channel,
    void*    userdata)
{
    g_signal_connect(data_channel, "on-error",
                     G_CALLBACK (data_channel_on_error), userdata);
    g_signal_connect(data_channel, "on-open",
                     G_CALLBACK (data_channel_on_open), userdata);
    g_signal_connect(data_channel, "on-close",
                     G_CALLBACK (data_channel_on_close), userdata);
    g_signal_connect(data_channel, "on-message-string",
                     G_CALLBACK (data_channel_on_message_string), userdata);
}


static void on_data_channel(
    GstElement* webrtc,
    GObject*    data_channel,
    void*       userdata)
{
    UNUSED(webrtc);
    webrtc_session_t* session = (webrtc_session_t*)userdata;
    connect_data_channel_signals(data_channel, userdata);
    session->receive_channel = data_channel;
}


webrtc_mp_t* webrtc_mp_create(camera_pipe_t* pipeline)
{
    /* Initialise stuff */
    webrtc_mp_t *mountpoint = (webrtc_mp_t*)malloc(
        sizeof(webrtc_mp_t));

    if (!mountpoint) {
        return NULL;
    }

    mountpoint->webrtcbins   = NULL;
    mountpoint->tee_pads     = NULL;
    mountpoint->bin_pads     = NULL;
    mountpoint->session_refs = NULL;

    mountpoint->pipeline_ref = pipeline;
    mountpoint->bin_count    = 0;

    return mountpoint;
}


void webrtc_mp_delete(webrtc_mp_t* mountpoint)
{
    if (!mountpoint) {
        return;
    }
    for (size_t i = 0; i < mountpoint->bin_count; i++) {
        gst_object_unref(mountpoint->tee_pads[i]);
        gst_object_unref(mountpoint->bin_pads[i]);
    }
    free(mountpoint);
}


static gboolean _webrtc_mp_reallocate(webrtc_mp_t* mountpoint)
{
    GstElement** webrtcbins = (GstElement**)realloc(
        mountpoint->webrtcbins,
        sizeof(GstElement*) * mountpoint->bin_count);

    webrtc_session_t** session_refs = (webrtc_session_t**)realloc(
        mountpoint->session_refs,
        sizeof(webrtc_session_t*) * mountpoint->bin_count);

    GstPad** tee_pads = (GstPad**)realloc(
        mountpoint->tee_pads,
        sizeof(GstPad*) * mountpoint->bin_count);

    GstPad** bin_pads = (GstPad**)realloc(
        mountpoint->bin_pads,
        sizeof(GstPad*) * mountpoint->bin_count);

    if ((mountpoint->bin_count != 0) &&
            (!webrtcbins || !session_refs || !tee_pads || !bin_pads)) {
        g_printerr("ERROR: Failure allocating memory in mountpoint "
                   "(%p, %p, %p, %p)\n",
                   webrtcbins, session_refs, tee_pads, bin_pads);
        free(webrtcbins);
        free(session_refs);
        free(tee_pads);
        free(bin_pads);
        return FALSE;
    }
    else {
        mountpoint->webrtcbins   = webrtcbins;
        mountpoint->session_refs = session_refs;
        mountpoint->tee_pads     = tee_pads;
        mountpoint->bin_pads     = bin_pads;
        return TRUE;
    }
}


gboolean webrtc_mp_add_element(
    webrtc_mp_t*      mountpoint,
    webrtc_session_t* session)
{
    if (!mountpoint) {
        return FALSE;
    }
    if (!session) {
        return FALSE;
    }
    gchar *name;
    GstElement *webrtcbin;
    GstPad *new_tee_pad, *webrtcbin_pad;
    /* Check that this mountpoint doesn't already have this client connected */
    for (size_t i = 0; i < mountpoint->bin_count; i++) {
        if (session->client_uid == mountpoint->session_refs[i]->client_uid) {
            g_printerr("ERROR: Client %u already connected to mountpoint\n",
                       session->client_uid);
            return FALSE;
        }
    }
    /* Create webrtcbin element and link to webrtc_tee */
    name      = g_strdup_printf("webrtcbin_%u", session->client_uid);
    webrtcbin = gst_element_factory_make("webrtcbin", name);
    g_free(name);

    if (!webrtcbin) {
        g_printerr("ERROR: Unable to create webrtcbin_%u\n",
                   session->client_uid);
        return FALSE;
    }
    /* Add reference to session */
    session->webrtcbin_ref = webrtcbin;

    /* This is the gstwebrtc entry point where we create the offer and so on. It
     * will be called when the pipeline goes to PLAYING. */
    g_signal_connect(webrtcbin, "on-negotiation-needed",
        G_CALLBACK (on_negotiation_needed), (void*)session);
    /* We need to transmit this ICE candidate to the browser via the websockets
     * signalling server. Incoming ice candidates from the browser need to be
     * added by us too, see on_server_message() */
    g_signal_connect(webrtcbin, "on-ice-candidate",
        G_CALLBACK (send_ice_candidate_message), (void*)session);

    g_signal_emit_by_name(webrtcbin, "create-data-channel", "channel", NULL,
        &session->send_channel);
    if (session->send_channel) {
        g_print                     ("Created data channel\n");
        // TODO: undelete me for newer gstreamer version
        // connect_data_channel_signals(session->send_channel, (void*)session);
    } else {
        g_printerr(
            "WARNING: Could not create data channel, is usrsctp available?\n");
    }

    // TODO: undelete me for newer gstreamer version
    g_signal_connect(webrtcbin, "on-data-channel", G_CALLBACK (on_data_channel),
        (void*)session);

    /* Set properties */
    g_object_set(webrtcbin, "stun-server", STUN_SERVER, NULL);
    // NB: only works in newer gstreamer
    g_object_set(webrtcbin, "bundle-policy", 3, NULL);

    /* The order should be:
     * - add new elements to pipeline
     * - link new elements (but not yet to tee)
     * - set state of elements to PLAYING (assuming the pipeline is in
     * PLAYING), going from sink towards the queue.
     * - lastly, link the queue to the tee.
     */

    if (!gst_bin_add(GST_BIN(mountpoint->pipeline_ref->pipeline), webrtcbin)) {
        g_printerr("ERROR: Adding webrtcbin_%u to pipeline\n",
                   session->client_uid);
        gst_object_unref(webrtcbin);
        return FALSE;
    }

    GstState* pipeline_state = NULL;
    GstStateChangeReturn ret;

    ret = gst_element_get_state(mountpoint->pipeline_ref->pipeline,
        pipeline_state, NULL, GST_CLOCK_TIME_NONE);

    if (ret != GST_STATE_CHANGE_SUCCESS) {
        g_printerr("ERROR: unable to get pipeline state (%d).\n", ret);
        return FALSE;
    }

    /* set state of new webrtcbin element to match pipeline */
    if (mountpoint->pipeline_ref->playing) {
        /* set state of new webrtcbin element to playing */
        GstStateChangeReturn ret = gst_element_set_state(
            webrtcbin, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            // gst_object_unref(webrtcbin);
            g_printerr(
                "ERROR: Unable to set the webrtcbin_%u to playing state.\n",
                session->client_uid);
            return FALSE;
        }
    }
    else {
        ret = gst_element_set_state(webrtcbin, GST_STATE_READY);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            // gst_object_unref(webrtcbin);
            g_printerr(
                "ERROR: Unable to set the webrtcbin_%u to playing state.\n",
                session->client_uid);
            return FALSE;
        }
    }

    /* Get pads for linking */
    new_tee_pad   = gst_element_get_request_pad(
        mountpoint->pipeline_ref->webrtc_tee, "src_%u");
    webrtcbin_pad = gst_element_get_request_pad(webrtcbin, "sink_%u");

    if (!new_tee_pad || !webrtcbin_pad) {
        g_printerr(
            "ERROR: Unable to get request pads for webrtcbin_%u linkage.\n",
            session->client_uid);
        return FALSE;
    }

    /* Link new webrtcbin to webrtc_tee */
    GstPadLinkReturn pad_ret = gst_pad_link(new_tee_pad, webrtcbin_pad);
    if (pad_ret != GST_PAD_LINK_OK) {
        gst_object_unref(new_tee_pad);
        gst_object_unref(webrtcbin_pad);
        g_printerr("ERROR: Unable to link to new webrtcbin_%u: "
                   "GstPadLinkReturn: %d\n",
                   session->client_uid, ret);
        gst_debug_set_threshold_from_string ("*:2", TRUE);
        return FALSE;
    }

    /* Update mountpoint with new element and session ref */
    mountpoint->bin_count++;
    if (!_webrtc_mp_reallocate(mountpoint)) {
        g_printerr("ERROR: Failure allocating memory in mountpoint\n");
        mountpoint->bin_count--;
        return FALSE;
    }
    mountpoint->webrtcbins  [mountpoint->bin_count - 1] = webrtcbin;
    mountpoint->session_refs[mountpoint->bin_count - 1] = session;
    mountpoint->tee_pads    [mountpoint->bin_count - 1] = new_tee_pad;
    mountpoint->bin_pads    [mountpoint->bin_count - 1] = webrtcbin_pad;

    return TRUE;
}


static gint _webrtc_mp_get_index(
    webrtc_mp_t* mountpoint,
    guint            client_uid)
{
    gint index = -1;
    for (gint i = 0; i < (gint)(mountpoint->bin_count); i++) {
        if (mountpoint->session_refs[i]->client_uid == client_uid) {
            index = i;
            break;
        }
    }
    return index;
}


GstElement* webrtc_mp_get_element(
    webrtc_mp_t* mountpoint,
    guint            client_uid)
{
    if (!mountpoint) {
        return NULL;
    }
    gint index = _webrtc_mp_get_index(mountpoint, client_uid);
    if (index == -1) {
        return NULL;
    }
    else {
        return mountpoint->webrtcbins[index];
    }
}


gboolean webrtc_mp_remove_element(
    webrtc_mp_t* mountpoint,
    guint            client_uid)
{
    if (!mountpoint) {
        return FALSE;
    }
    gint index = _webrtc_mp_get_index(mountpoint, client_uid);
    if (index == -1) {
        return FALSE;
    }
    /* Unlink webrtcbin element */
    gst_pad_unlink        (mountpoint->tee_pads[index],
                           mountpoint->bin_pads[index]);
    gst_element_remove_pad(mountpoint->pipeline_ref->webrtc_tee,
                           mountpoint->tee_pads[index]);

    /* Set state to NULL*/
    GstStateChangeReturn ret = gst_element_set_state(
        mountpoint->webrtcbins[index], GST_STATE_NULL);
    g_print("return: %d\n", ret);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        // gst_object_unref(webrtcbin);
        g_printerr(
            "WARNING: Unable to set the webrtcbin_%u to NULL state.\n",
            client_uid);
        return FALSE;
    }

    if (!gst_bin_remove(GST_BIN (mountpoint->pipeline_ref->pipeline),
            mountpoint->webrtcbins[index])) {
        g_printerr(
            "WARNING: Unable to remove the webrtcbin_%u from the pipeline.\n",
            client_uid);
        return FALSE;
    }

    /* free memory allocated for bin and pads */
    gst_object_unref(mountpoint->tee_pads  [index]);
    gst_object_unref(mountpoint->bin_pads  [index]);

    /* Loop through rest of mountpoint and move everything back one */
    for (size_t i = index; i < (mountpoint->bin_count - 1); i++) {
        mountpoint->webrtcbins  [i] = mountpoint->webrtcbins  [i + 1];
        mountpoint->session_refs[i] = mountpoint->session_refs[i + 1];
        mountpoint->tee_pads    [i] = mountpoint->tee_pads    [i + 1];
        mountpoint->bin_pads    [i] = mountpoint->bin_pads    [i + 1];
    }

    /* reallocate memory */
    mountpoint->bin_count--;
    if (!_webrtc_mp_reallocate(mountpoint)) {
        g_printerr("ERROR: Failure reallocating memory in mountpoint\n");
        mountpoint->bin_count++;
        return FALSE;
    }

    return TRUE;
}


webrtc_session_t* webrtc_mp_get_session(
    webrtc_mp_t* mountpoint,
    guint            client_uid)
{
    if (!mountpoint) {
        return NULL;
    }
    gint index = _webrtc_mp_get_index(mountpoint, client_uid);
    if (index == -1) {
        return NULL;
    }
    else {
        return mountpoint->session_refs[index];
    }
}