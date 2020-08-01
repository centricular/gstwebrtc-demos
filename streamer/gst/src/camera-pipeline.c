#include "camera-pipeline.h"

#define UNUSED(x) (void)(x)

#define RAW_STREAM_FORMAT "byte-stream"

#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240
#define FRAMERATE    30


static gboolean set_properties(camera_pipe_t *data);

camera_pipe_t* camera_pipe_create()
{
    /* Initialise stuff */
    camera_pipe_t *data = (camera_pipe_t*)malloc(
        sizeof(camera_pipe_t));

    if (!data) {
        return NULL;
    }

    /* Create Gst elements */
    data->video_testsrc = gst_element_factory_make("videotestsrc",
                                                   "videotestsrc");
    data->video_convert = gst_element_factory_make("videoconvert",
                                                   "videoconvert");
    data->queue         = gst_element_factory_make("queue", "queue");

#ifdef __aarch64__
    data->video_encoder = gst_element_factory_make("omxh264enc",
                                                   "video_encoder");
#else
    data->video_encoder = gst_element_factory_make("vaapih264enc",
                                                   "video_encoder");
    if (!data->video_encoder) {
        data->video_encoder = gst_element_factory_make("x264enc",
                                                       "video_encoder");
    }
#endif

    data->rtp_payloader = gst_element_factory_make("rtph264pay",
                                                  "rtp_payloader");


    data->webrtc_queue = gst_element_factory_make("queue", "webrtc_queue");
    data->webrtc_tee   = gst_element_factory_make("tee", "webrtc_tee");
    data->fakesink     = gst_element_factory_make("fakesink", "fakesink");


    data->webrtc_mp     = webrtc_mp_create(data);
    if (!data->webrtc_mp) {
        g_error("Failed to create webrtc mountpoint!\n");
        return NULL;
    }

    /* Make capsfilters */
    data->encode_caps_filter  = gst_element_factory_make("capsfilter",
                                                         "encode_caps_filter");

    data->source_caps_filter  = gst_element_factory_make("capsfilter",
                                                         "source_caps_filter");

    data->pipeline = gst_pipeline_new("camera-pipeline");

    if (!data->pipeline || !data->video_testsrc || !data->video_convert ||
        !data->queue || !data->video_encoder || !data->rtp_payloader ||
        !data->webrtc_queue || !data->webrtc_tee || !data->fakesink ||
        !data->encode_caps_filter || !data->source_caps_filter) {
        // TODO: add error goto
        g_print("Not all elements could be created!\n");
        return NULL;
    }

    if (!set_properties(data)) {
        camera_pipe_delete(data);
        return NULL;
    }
    /* Initialise playing state */
    data->playing     = FALSE;

    /* Add src->payloader elements */
    gst_bin_add_many(GST_BIN (data->pipeline), data->video_testsrc,
                     data->source_caps_filter, data->video_convert,
                     data->queue, data->video_encoder,
                     data->encode_caps_filter, data->rtp_payloader,
                     data->webrtc_queue, data->webrtc_tee, data->fakesink,
                     NULL);


    /* link elements */

    if (!gst_element_link_many(data->video_testsrc, data->source_caps_filter,
                               data->video_convert, data->queue,
                               data->video_encoder, data->encode_caps_filter,
                               data->rtp_payloader, data->webrtc_queue,
                               data->webrtc_tee, data->fakesink, NULL)) {
        GST_ERROR("Elements could not be linked!\n");
        camera_pipe_delete(data);
        return NULL;
    }

    return data;
}


void camera_pipe_delete(camera_pipe_t* data)
{
    if (!data) {
        return;
    }

    gst_element_set_state(data->pipeline, GST_STATE_NULL);
    gst_object_unref(data->pipeline);

    webrtc_mp_delete(data->webrtc_mp);
    free(data);
}


gboolean camera_pipe_set_state(camera_pipe_t* data, GstState state)
{
    /* Start playing the pipeline */
    GST_INFO("Setting pipeline state to %d\n", state);
    GstStateChangeReturn ret = gst_element_set_state(data->pipeline, state);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        GST_ERROR ("Unable to set the pipeline to the state #%d.\n", state);
        return FALSE;
    }
    if ((state == GST_STATE_PLAYING) && (!data->playing)) {
        GST_INFO("Pipeline -> playing state\n");
        data->playing     = TRUE;
    }
    else {
        GST_INFO("Pipeline -> stopped state\n");
        data->playing = FALSE;
    }
    return TRUE;
}


static gboolean set_properties(camera_pipe_t *data)
{
    GstCaps *encode_caps, *source_caps;

    GString *encode_caps_str = g_string_new("");
    GString *source_caps_str = g_string_new("");

    g_string_printf(encode_caps_str,
                    "video/x-h264, stream-format=(string)%s, "
                    "profile=baseline",
                    RAW_STREAM_FORMAT);

    g_string_printf(source_caps_str,
                    "video/x-raw, width=(int)%d, "
                    "height=(int)%d, format=(string)I420, "
                    "framerate=(fraction)%d/1",
                    FRAME_WIDTH, FRAME_HEIGHT,
                    FRAMERATE);

    encode_caps = gst_caps_from_string(encode_caps_str->str);
    g_string_free(encode_caps_str, TRUE);

    source_caps = gst_caps_from_string(source_caps_str->str);
    g_string_free(source_caps_str, TRUE);

    if (!encode_caps || !source_caps) {
        GST_ERROR("Unable to create caps!\n");
        return FALSE;
    }

    /* Set element properties */
    g_object_set(data->encode_caps_filter, "caps", encode_caps, NULL);
    g_object_set(data->source_caps_filter, "caps", source_caps, NULL);

    g_object_set(data->rtp_payloader, "config-interval", 10, "pt", 96, NULL);

    g_object_set(data->video_testsrc, "is-live", TRUE, NULL);

    return TRUE;
}