/*
 * Server for negotiating and streaming a webrtc stream
 * with a browser JS app.
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 * Author: Aiden Jeffrey    <aiden.jeffrey@codethink.co.uk>
 */
#include "webrtc-mountpoint.h"
#include "webrtc-server.h"

#define UNUSED(x) (void)(x)

#define MAX_WEBRTC_SESSIONS 1000

static SoupWebsocketConnection *ws_conn      = NULL;
static webrtc_server_state_e    server_state = SERVER_STATE_UNKNOWN;

static const gchar             *server_url   = "ws://localhost:8443";
static webrtc_session_t         sessions[MAX_WEBRTC_SESSIONS] = {0};


static void log_error(
    const gchar*           msg,
    webrtc_server_state_e  desired_server_state,
    webrtc_session_state_e session_state)
{
    if (msg) {
        g_printerr("%s, server-state: %d, session_state: %d\n",
                   msg, desired_server_state, session_state);
    }
    /* Don't want a server in error continuing */
    if ((desired_server_state == SERVER_STATE_ERROR) ||
        (desired_server_state == SERVER_STATE_CONNECTION_ERROR) ||
        (desired_server_state == SERVER_STATE_REGISTRATION_ERROR)) {
        server_state = SERVER_STATE_UNKNOWN;
    }
    else {
        server_state = desired_server_state;
    }
}


static gboolean register_with_server(void)
{
    gchar *hello;

    if (soup_websocket_connection_get_state(ws_conn) !=
            SOUP_WEBSOCKET_STATE_OPEN) {
        return FALSE;
    }

    g_print("Registering with signalling server\n");
    server_state = SERVER_STATE_REGISTERING;

    /* Register with the server with a random integer id. Reply will be received
     * by on_server_message() */
    hello = g_strdup_printf("REGISTER MEDIA");
    soup_websocket_connection_send_text(ws_conn, hello);
    g_free(hello);

    return TRUE;
}


static gint get_client_session_index(guint uid)
{
    gint i = -1;
    gboolean found_uid = FALSE;
    for (i = 0; i < MAX_WEBRTC_SESSIONS; i++) {
        if ((sessions[i].client_uid == uid) && (sessions[i].active)) {
            found_uid = TRUE;
            break;
        }
    }
    if (!found_uid) {
        return -1;
    }
    else {
        return i;
    }
}


static gint bind_client_session(guint uid)
{
    gint i = -1;
    gboolean found_space = FALSE;
    for (i = 0; i < MAX_WEBRTC_SESSIONS; i++) {
        if (!sessions[i].active) {
            sessions[i].client_uid = uid;
            sessions[i].state      = CLIENT_CONNECTING;
            sessions[i].active     = TRUE;
            found_space = TRUE;
            break;
        }
    }
    if (!found_space) {
        return -1;
    }
    else {
        return i;
    }
}


static gint unbind_client_session(guint uid)
{
    gint session_index = get_client_session_index(uid);
    if (session_index != -1) {
        sessions[session_index].active = FALSE;
    }
    return session_index;
}


/*
 * Extracts webrtcbin from webrtc_mp_t if exists
 */
gboolean set_session_webrtcbinref(
    webrtc_session_t* session,
    camera_pipe_t*    pipeline)
{
    if (pipeline == NULL) {
        return FALSE;
    }

    GstElement* webrtcbin = webrtc_mp_get_element(
        pipeline->webrtc_mp, session->client_uid);

    if (webrtcbin == NULL) {
        gchar* msg = g_strdup_printf(
            "ERROR: No webrtcbin element for client_uid %u",
            session->client_uid);
        log_error(msg, server_state, STREAM_ERROR);
        g_free   (msg);
        return FALSE;
    }
    else {
        session->webrtcbin_ref = webrtcbin;
        return TRUE;
    }
}


/*
 * Looks for valid server messages and processes them
 * returns -1 if error, 0 if no server message found, 1 if server message
 * found and processed.
 */
static gint process_server_messages(
    gchar*                   text,
    camera_pipe_t*           pipeline,
    SoupWebsocketConnection* ws_conn)
{
    gint              client_uid = -1;
    webrtc_session_t *session;
    gint              session_index;

    if (pipeline == NULL) {
        return -1;
    }

    if (g_strcmp0(text, "REGISTERED") == 0) {
        /* Server has accepted our registration, we are ready to send commands */
        if (server_state != SERVER_STATE_REGISTERING) {
            log_error ("ERROR: Received REGISTERED when "
                       "not registering", SERVER_STATE_ERROR, 0);
            return -1;
        }
        server_state = SERVER_STATE_REGISTERED;
        g_print("Registered with server\n");
    } else if (sscanf(text, "BIND-SESSION-CLIENT %d", &client_uid) == 1) {
        /* Register client with webrtc server */
        /* check that this client doesn't already have a session */
        if (get_client_session_index((guint)client_uid) != -1) {
            gchar* msg = g_strdup_printf(
                "ERROR: client %d already in session", client_uid);
            log_error(msg, server_state, CLIENT_CONNECTION_ERROR);
            g_free   (msg);
            return -1;
        }
        /* no current session, so make one */
        if ((session_index = bind_client_session((guint)client_uid)) == -1) {
            gchar* msg = g_strdup_printf(
                "ERROR: no space to register %d client session", client_uid);
            log_error(msg, server_state, CLIENT_CONNECTION_ERROR);
            g_free   (msg);
            return -1;
        }
        session = &(sessions[session_index]);
        if (session->state != CLIENT_CONNECTING) {
            log_error("ERROR: Received BIND-SESSION-CLIENT when not connecting",
                      server_state, CLIENT_CONNECTION_ERROR);
            return -1;
        }
        session->state = CLIENT_CONNECTED;
        /* Inform signalling server that bind was successful */
        gchar* ret_text = g_strdup_printf(
            "SESSION %u BOUND", session->client_uid);

        soup_websocket_connection_send_text(ws_conn, ret_text);
        g_free                             (ret_text);
    } else if (sscanf(text, "UNBIND-SESSION-CLIENT %d", &client_uid) == 1) {
        /* De-register client with webrtc server */
        if ((session_index = unbind_client_session((guint)client_uid)) == -1) {
            gchar* msg = g_strdup_printf(
                "ERROR: no client session %d", client_uid);
            log_error(msg, server_state, CLIENT_CONNECTION_ERROR);
            g_free   (msg);
            return -1;
        }
        /* Destroy any webrtcbins associated with this session */
        session = &(sessions[session_index]);

        /* Remove webrtcbin element if one exists */
        if (!webrtc_mp_remove_element(pipeline->webrtc_mp,
                                      session->client_uid)) {
            gchar* msg = g_strdup_printf(
                "WARNING: Problem removing client_uid %u",
                session->client_uid);
            log_error(msg, server_state, STREAM_ERROR);
            g_free   (msg);
        }

        /* Inform signalling server that unbind was successful */
        gchar* ret_text = g_strdup_printf(
            "SESSION %u UNBOUND", session->client_uid);

        soup_websocket_connection_send_text(ws_conn, ret_text);
        g_free                             (ret_text);

    } else if (g_str_has_prefix (text, "ERROR")) {
        /* Handle errors */
        switch (server_state) {
            case SERVER_STATE_CONNECTING: {
                server_state = SERVER_STATE_CONNECTION_ERROR;
                break;
            }
            case SERVER_STATE_REGISTERING: {
                server_state = SERVER_STATE_REGISTRATION_ERROR;
                break;
            }
            default: {
                break;
            }
        }
        log_error(text, 0, 0);
        /* although we received an error, the function itself didn't error */
        return 1;
    }
    else {
        /* No server messages found */
        return 0;
    }
    return 1;
}

/*
 * Mirrors the json message back to the sender with success = FALSE and some
 * error info.
 */
static void _return_json_failure(SoupWebsocketConnection* ws_conn,
                                 JsonObject*              json_return,
                                 gchar*                   msg)
{
    json_object_set_boolean_member(
        json_return, "success", FALSE);
    json_object_set_string_member(
        json_return, "return-message", msg);

    gchar* text = get_string_from_json_object(json_return);
    json_object_unref                        (json_return);

    soup_websocket_connection_send_text(ws_conn, text);
    g_free                             (text);
}


/*
 * Looks for valid json messages forwarded direct from client and processes
 * them returns -1 if error, 0 if no server message found, 1 if json
 * message found and processed.
 */
static gint process_json_messages(
    gchar*                   text,
    camera_pipe_t*           pipeline,
    SoupWebsocketConnection* ws_conn)
{
    JsonNode   *root;
    JsonObject *object, *child, *json_return;;
    gchar      *return_text;

    JsonParser *parser = json_parser_new ();

    if (!json_parser_load_from_data (parser, text, -1, NULL)) {
        g_printerr ("Unknown message '%s', ignoring", text);
        g_object_unref (parser);
        return -1;
    }

    root = json_parser_get_root (parser);
    if (!JSON_NODE_HOLDS_OBJECT (root)) {
        g_printerr ("Unknown json message '%s', ignoring", text);
        g_object_unref (parser);
        return -1;
    }
    object = json_node_get_object (root);

    gint                  client_uid = -1;
    webrtc_session_t* session;
    gint                  session_index;

    /* Get client_uid from json message */
    if (json_object_has_member(object, "client_uid")) {
        client_uid = json_object_get_int_member(object, "client_uid");
        if ((session_index = get_client_session_index(client_uid)) != -1) {
            session = &(sessions[session_index]);
        }
        else {
            gchar* msg = g_strdup_printf(
                "ERROR: trying to access non-existent client session %d",
                client_uid);
            log_error(msg, server_state, STREAM_ERROR);
            g_free   (msg);
            return -1;
        }
    }
    else {
        log_error("ERROR: json message received without client_uid",
                  server_state, STREAM_ERROR);
        return -1;
    }

    /* Check type of JSON message */
    /* commands are of the form {command: {type: foo, data: bar} */
    if (json_object_has_member(object, "command")) {
        const gchar* cmd_type;
        child = json_object_get_object_member(object, "command");

        if (!json_object_has_member(child, "type")) {
            gchar* msg = "ERROR: received command without 'type'";
            log_error(msg, server_state, STREAM_ERROR);

            _return_json_failure(ws_conn, object, msg);
            return -1;
        }

        cmd_type = json_object_get_string_member(child, "type");
        if (g_strcmp0(cmd_type, "connect-to-mountpoint") == 0) {
            /* Add session object to sessions and update mountpoint */
            if (session->state > CLIENT_CONNECTED) {
                gchar* msg = g_strdup_printf(
                    "ERROR: client %u is already connected to",
                    session->client_uid);
                log_error(msg, server_state, STREAM_ERROR);
                _return_json_failure(ws_conn, object, msg);
                g_free   (msg);
                return -1;
            }
            if (!pipeline->playing) {
                gchar* msg = g_strdup_printf(
                    "ERROR: mountpoint is not playing");
                log_error(msg, server_state, STREAM_ERROR);
                _return_json_failure(ws_conn, object, msg);
                g_free   (msg);
                return -1;
            }

            /* Update session and create webrtcbin element */
            session->state = STREAM_MOUNTED;

            /* Add element (webrtcbin) to mountpoint for this client session.
             * This element will become active when the element sees the
             * `on-negotiation-needed` signal (either when pipeline -> PLAYING,
             * or we put the signal on manually)
             */
            if (!webrtc_mp_add_element(pipeline->webrtc_mp, session)) {
                gchar* msg = g_strdup_printf(
                    "ERROR: Adding webrtcbin element to "
                    "mountpoint for client %u",
                    session->client_uid);
                log_error(msg, server_state, STREAM_ERROR);
                _return_json_failure(ws_conn, object, msg);
                g_free   (msg);
                return -1;
            }

            /* Return success message */
            json_return = object;
            json_object_set_boolean_member(
                json_return, "success", TRUE);

            return_text = get_string_from_json_object(json_return);

            soup_websocket_connection_send_text(ws_conn, return_text);
            g_free                             (return_text);
        }
        else if (g_strcmp0(cmd_type, "disconnect-mountpoint") == 0) {
            if (session->state <= CLIENT_CONNECTED) {
                gchar* msg = g_strdup_printf(
                    "ERROR: client %u is not currently connected to a stream",
                    session->client_uid);
                log_error(msg, server_state, STREAM_ERROR);
                _return_json_failure(ws_conn, object, msg);
                g_free   (msg);
                return -1;
            }
            /* Remove webrtcbin in session->mountpoint assigned to this client */
            if (!webrtc_mp_remove_element(pipeline->webrtc_mp,
                                          (guint)client_uid)) {
                gchar* msg = g_strdup_printf(
                    "ERROR: Removing webrtcbin element from "
                    "mountpoint for client %u",
                    session->client_uid);
                log_error(msg, server_state, STREAM_ERROR);
                _return_json_failure(ws_conn, object, msg);
                g_free   (msg);
                return -1;
            }
            /* Update session */
            session->state = CLIENT_CONNECTED;

            /* Return success message */
            json_return = object;
            json_object_set_boolean_member(
                json_return, "success", TRUE);

            return_text = get_string_from_json_object(json_return);

            soup_websocket_connection_send_text(ws_conn, return_text);
            g_free                             (return_text);
        }
        else {
            gchar* msg = g_strdup_printf(
                "ERROR: unknown command type %s ", cmd_type);
                log_error(msg, server_state, STREAM_ERROR);
                g_free   (msg);
            return -1;
        }
    }
    else if (json_object_has_member(object, "sdp")) {
        int                          ret;
        GstSDPMessage               *sdp;
        const gchar                 *sdp_text, *sdptype;
        GstWebRTCSessionDescription *answer;

        if (session->state != STREAM_NEGOTIATING) {
            gchar* msg = g_strdup_printf(
                "ERROR: trying to negotiate stream for session %d ",
                client_uid);
            log_error(msg, server_state, session->state);
            g_free   (msg);
            return -1;
        }

        child = json_object_get_object_member(object, "sdp");

        if (!json_object_has_member(child, "type")) {
            log_error("ERROR: received SDP without 'type'",
                      server_state, STREAM_ERROR);
            return -1;
        }

        sdptype = json_object_get_string_member(child, "type");
        if (g_strcmp0(sdptype, "answer") != 0) {
            log_error("ERROR: SDP message not of `answer` type",
                      server_state, STREAM_ERROR);
            return -1;
        }
        sdp_text = json_object_get_string_member(child, "sdp");

        g_print("Received SDP answer:\n%s\n", sdp_text);

        ret = gst_sdp_message_new(&sdp);
        g_assert_cmphex (ret, ==, GST_SDP_OK);

        ret = gst_sdp_message_parse_buffer((guint8*) sdp_text,
                                           strlen (sdp_text), sdp);
        if (ret != GST_SDP_OK) {
            gchar* msg = g_strdup_printf(
                "ERROR: parsing SDP message %s", sdp_text);
            log_error(msg, server_state, STREAM_ERROR);
            g_free   (msg);
            return -1;
        }

        answer = gst_webrtc_session_description_new(
            GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        if (answer == NULL) {
            log_error("ERROR: NULL SDP answer",
                      server_state, STREAM_ERROR);
            return -1;
        }

        if (!session->webrtcbin_ref) {
            set_session_webrtcbinref(session, pipeline);
        }

        if (session->webrtcbin_ref == NULL) {
            gchar* msg = g_strdup_printf(
                "ERROR: No webrtcbin found for session %u",
                session->client_uid);
            log_error(msg, server_state, STREAM_ERROR);
            g_free   (msg);
            return -1;
        }

        /* Set remote description on our pipeline */
        GstPromise *promise = gst_promise_new();
        g_signal_emit_by_name(
            session->webrtcbin_ref, "set-remote-description", answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref    (promise);

        session->state = STREAM_STARTED;

    }
    else if (json_object_has_member(object, "ice")) {
        const gchar *candidate;
        gint sdpmlineindex;

        child = json_object_get_object_member     (object, "ice");
        candidate = json_object_get_string_member (child, "candidate");
        sdpmlineindex = json_object_get_int_member(child, "sdpMLineIndex");

        if (!session->webrtcbin_ref) {
            set_session_webrtcbinref(session, pipeline);
        }

        if (session->webrtcbin_ref == NULL) {
            log_error("ERROR: No webrtcbin found for session",
                      server_state, STREAM_ERROR);
            return -1;
        }

        /* Add ice candidate sent by remote peer */
        g_signal_emit_by_name(session->webrtcbin_ref, "add-ice-candidate",
                              sdpmlineindex, candidate);
    }
    else {
        gchar* msg = g_strdup_printf(
            "WARNING: Ignoring unknown JSON message:\n%s\n", text);
        log_error(msg, server_state, STREAM_ERROR);
        g_free   (msg);
        return 0;
    }
    g_object_unref (parser);
    return 1;
}


static void on_server_closed(
    SoupWebsocketConnection* conn,
    camera_pipe_t*           pipeline)
{
    UNUSED(conn);
    UNUSED(pipeline);
    server_state = SERVER_STATE_CLOSED;
    log_error("Server connection closed", 0, 0);
    /* Update sessions with NULL for ws_conn */
    for (size_t i = 0; i < MAX_WEBRTC_SESSIONS; i++) {
        sessions[i].ws_conn_ref = NULL;
    }
}


/* One mega message handler for our asynchronous calling mechanism */
static void on_server_message(
    SoupWebsocketConnection* conn,
    SoupWebsocketDataType    type,
    GBytes*                  message,
    camera_pipe_t*           pipeline)
{
    gchar *text = NULL;

    switch (type) {
        case SOUP_WEBSOCKET_DATA_BINARY: {
            g_printerr("Received unknown binary message, ignoring\n");
            return;
        }
        case SOUP_WEBSOCKET_DATA_TEXT: {
            gsize size;
            const gchar *data = g_bytes_get_data(message, &size);
            /* Convert to NULL-terminated string */
            text = g_strndup(data, size);
            break;
        }
        default: {
            log_error("Unknown websocket data type\n",
                SERVER_STATE_ERROR, STREAM_ERROR);
            goto out;
        }
    }

    /* Check that we can return messages to signalling server */
    if (soup_websocket_connection_get_state(conn) !=
            SOUP_WEBSOCKET_STATE_OPEN) {
        log_error("No websocket connection\n",
            SERVER_STATE_ERROR, STREAM_ERROR);
        goto out;
    }

    if (process_server_messages(text, pipeline, conn) == 0) {
        process_json_messages(text, pipeline, conn);
    }
    out:
        g_free(text);
}


static void on_server_connected(
    SoupSession*   session,
    GAsyncResult*  res,
    camera_pipe_t* pipeline)
{
    GError *error = NULL;

    ws_conn = soup_session_websocket_connect_finish (session, res, &error);
    if (error) {
        log_error   (error->message, SERVER_STATE_CONNECTION_ERROR, 0);
        g_error_free(error);
        return;
    }

    /* Update sessions with ws_conn */
    for (size_t i = 0; i < MAX_WEBRTC_SESSIONS; i++) {
        sessions[i].ws_conn_ref = ws_conn;
    }

    g_assert_nonnull (ws_conn);

    server_state = SERVER_STATE_CONNECTED;
    g_print("Connected to signalling server\n");

    g_signal_connect(ws_conn, "closed",  G_CALLBACK (on_server_closed),  pipeline);
    g_signal_connect(ws_conn, "message", G_CALLBACK (on_server_message), pipeline);

    /* Register with the server so it knows about us and can accept commands */
    register_with_server();
}


/*
 * Connect to the signalling server. This is the entrypoint for everything else.
 */
void webrtc_websocket_controller_setup(camera_pipe_t* pipeline)
{
    SoupLogger  *logger;
    SoupMessage *message;
    SoupSession *soup_session;
    const char  *http_aliases[] = {"ws", NULL};

    soup_session = soup_session_new_with_options(
        SOUP_SESSION_SSL_STRICT, FALSE,
        SOUP_SESSION_HTTP_ALIASES, http_aliases, NULL);

    logger = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
    soup_session_add_feature(soup_session, SOUP_SESSION_FEATURE(logger));
    g_object_unref(logger);

    message = soup_message_new(SOUP_METHOD_GET, server_url);

    g_print("Connecting to server...\n");

    /* Once connected, we will register */
    soup_session_websocket_connect_async(soup_session, message, NULL,
        NULL, NULL, (GAsyncReadyCallback) on_server_connected, pipeline);
    server_state = SERVER_STATE_CONNECTING;
}


void webrtc_websocket_controller_teardown()
{
    if (ws_conn) {
        if (soup_websocket_connection_get_state(ws_conn) ==
                SOUP_WEBSOCKET_STATE_OPEN) {
            soup_websocket_connection_close(ws_conn, 1000, "");
        }
        else {
            g_object_unref(ws_conn);
        }
    }
}
