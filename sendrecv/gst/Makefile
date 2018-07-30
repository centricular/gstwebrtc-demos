CC     := gcc
LIBS   := $(shell pkg-config --libs --cflags gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0)
CFLAGS := -O0 -ggdb -Wall -fno-omit-frame-pointer \
          $(shell pkg-config --cflags gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-1.0 glib-2.0 libsoup-2.4 json-glib-1.0 glib-2.0)
webrtc-sendrecv: webrtc-sendrecv.c
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@
