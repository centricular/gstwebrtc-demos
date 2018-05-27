FROM maxmcd/gstreamer:1.14-buster

RUN apt-get install -y libjson-glib-dev
# RUN apk update
# RUN apk add json-glib-dev libsoup-dev

WORKDIR /opt/
COPY . /opt/

RUN sed -i 's/wss:\/\/webrtc.nirbheek.in:8443/ws:\/\/signalling:8443/g' \
    /opt/webrtc-sendrecv.c
RUN sed -i 's/strict_ssl = TRUE/strict_ssl = FALSE/g' \
    /opt/webrtc-sendrecv.c

RUN make

# ENV GST_DEBUG=5
CMD echo "Waiting a few seconds for you to open the browser" \
    && sleep 7 \
    && ./webrtc-sendrecv --peer-id=1

