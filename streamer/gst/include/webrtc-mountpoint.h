#ifndef __WEBRTC_MP_H__
#define __WEBRTC_MP_H__

#include <gst/gst.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>

typedef struct webrtc_mp_s webrtc_mp_t;

#include "camera-pipeline.h"
#include "webrtc-server.h"

/*
 * Webrtc mountpoints
 * each pipeline has a mountpoint, with multiple webrtcbins (one for each
 * client connection)
 */
struct webrtc_mp_s {
    camera_pipe_t*     pipeline_ref;
    GstElement**       webrtcbins;
    GstPad**           tee_pads;
    GstPad**           bin_pads;
    webrtc_session_t** session_refs;
    size_t             bin_count;
};

webrtc_mp_t* webrtc_mp_create(camera_pipe_t* pipeline);
void         webrtc_mp_delete(webrtc_mp_t* mountpoint);

gboolean         webrtc_mp_add_element   (webrtc_mp_t*      mountpoint,
                                          webrtc_session_t* session);
GstElement*      webrtc_mp_get_element   (webrtc_mp_t* mountpoint,
                                          guint        client_uid);
gboolean         webrtc_mp_remove_element(webrtc_mp_t* mountpoint,
                                          guint        client_uid);
webrtc_session_t* webrtc_mp_get_session  (webrtc_mp_t* mountpoint,
                                          guint        client_uid);

gchar* get_string_from_json_object(JsonObject* object);

#endif