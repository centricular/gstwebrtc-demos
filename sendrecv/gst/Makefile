CC     := gcc
LIBS   := $(shell pkg-config --libs --cflags gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0)
CFLAGS := -O0 -ggdb -Wall -fno-omit-frame-pointer \
          -I/usr/include/gstreamer-1.0 \
          -I/usr/include/glib-2.0 \
          -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
          -I/usr/include/libsoup-2.4 \
          -I/usr/include/json-glib-1.0

webrtc-sendrecv: webrtc-sendrecv.c
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@
