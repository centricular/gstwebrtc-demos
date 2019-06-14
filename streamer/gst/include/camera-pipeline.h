#ifndef __CAMERA_PIPELINE_H__
#define __CAMERA_PIPELINE_H__

#include <gmodule.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct camera_pipe_s camera_pipe_t;

#include "webrtc-mountpoint.h"

struct camera_pipe_s {
    gint camera_id;

    GstElement *pipeline;
    GstElement *video_testsrc, *video_convert;
    GstElement *queue, *video_encoder;

    GstElement *rtp_payloader;
    GstElement *webrtc_queue, *webrtc_tee;

    webrtc_mp_t *webrtc_mp;

    GstElement *source_caps_filter, *encode_caps_filter;

    gboolean    playing;
};

struct camera_pipe_list_s {
    camera_pipe_t **pipeline;
    size_t              pipeline_count;
};

camera_pipe_t* camera_pipe_create   ();
void           camera_pipe_delete   (camera_pipe_t* data);

gboolean       camera_pipe_set_state(camera_pipe_t* data,
                                     GstState state);
#endif
