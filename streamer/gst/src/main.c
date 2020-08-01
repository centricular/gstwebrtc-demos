#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib-unix.h>
#include <gst/gst.h>

#include "camera-pipeline.h"
#include "webrtc-server.h"

typedef struct custom_data_s {
    camera_pipe_t *pipeline;
    GMainLoop     *main_loop;
} custom_data_t;


gboolean exit_sighandler(GMainLoop *main_loop)
{
    g_info("Caught signal, stopping mainloop\n");
    g_main_loop_quit(main_loop);
    return TRUE;
}


static void custom_data_cleanup(custom_data_t *data)
{
    if (data->main_loop) {
        g_main_loop_unref(data->main_loop);
    }

    camera_pipe_delete(data->pipeline);

    webrtc_websocket_controller_teardown();
}


int main(int argc, char *argv[])
{
    custom_data_t data;

    /* Initialise stuff */
    gst_init(&argc, &argv);
    memset  (&data, 0, sizeof(data));

    data.pipeline = camera_pipe_create();

    webrtc_websocket_controller_setup(data.pipeline);

    if (!data.pipeline) {
        custom_data_cleanup(&data);
    }

    /* Create a GLib Main Loop and set up interupt handlers */
    data.main_loop = g_main_loop_new(NULL, FALSE);

    g_unix_signal_add(SIGINT , (void*)exit_sighandler, data.main_loop);
    g_unix_signal_add(SIGTERM, (void*)exit_sighandler, data.main_loop);

    /* Play the pipeline */
    if (!camera_pipe_set_state(data.pipeline, GST_STATE_PLAYING)) {
        custom_data_cleanup(&data);
        return -1;
    }

    /* Run main loop */
    g_main_loop_run(data.main_loop);

    /* Free resources */
    custom_data_cleanup(&data);
    return 0;

}