#ifndef __WEBRTC_SERVER_H__
#define __WEBRTC_SERVER_H__

#include <gst/gst.h>
#include <gst/sdp/sdp.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "camera-pipeline.h"

#include <string.h>

typedef enum {
    SERVER_STATE_UNKNOWN            = 0,
    SERVER_STATE_ERROR              = 1, /* generic error */
    SERVER_STATE_CONNECTING         = 2,
    SERVER_STATE_CONNECTION_ERROR   = 3,
    SERVER_STATE_CONNECTED          = 4, /* Ready to register */
    SERVER_STATE_REGISTERING        = 5,
    SERVER_STATE_REGISTRATION_ERROR = 6,
    SERVER_STATE_REGISTERED         = 7, /* Ready to accept client connections */
    SERVER_STATE_CLOSED             = 8, /* server connection closed somewhere */
    SERVER_STATE_TYPE_COUNT
} webrtc_server_state_e;

typedef enum {
    CLIENT_CONNECTING       = 0,
    CLIENT_CONNECTION_ERROR = 1,
    CLIENT_CONNECTED        = 2,
    STREAM_MOUNTED          = 3,
    STREAM_NEGOTIATING      = 4,
    STREAM_STARTED          = 5,
    STREAM_STOPPING         = 6,
    STREAM_STOPPED          = 7,
    STREAM_ERROR            = 8,
    SESSION_STATE_TYPE_COUNT
} webrtc_session_state_e;

/*
 * Session contains data encapsulating a client connection
 *      mountpoint_id   - the camera_id in camera_pipe_t
 *      webrtcbin_index - index into webrtc_mp_t
 */
typedef struct webrtc_session_s {
    guint                    client_uid;
    webrtc_session_state_e   state;
    gboolean                 active;
    SoupWebsocketConnection* ws_conn_ref;
    GstElement*              webrtcbin_ref;
    GObject*                 send_channel;
    GObject*                 receive_channel;
    GstClockTime             join_time;
} webrtc_session_t;


void webrtc_websocket_controller_setup   (
    camera_pipe_t* pipeline);
void webrtc_websocket_controller_teardown();


gboolean set_session_webrtcbinref(
    webrtc_session_t* session,
    camera_pipe_t*    pipeline);

#endif
