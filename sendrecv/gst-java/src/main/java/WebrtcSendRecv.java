import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import org.asynchttpclient.ws.WebSocket;
import org.asynchttpclient.ws.WebSocketListener;
import org.asynchttpclient.ws.WebSocketUpgradeHandler;
import org.freedesktop.gstreamer.*;
import org.freedesktop.gstreamer.Element.PAD_ADDED;
import org.freedesktop.gstreamer.elements.WebRTCBin;
import org.freedesktop.gstreamer.elements.WebRTCBin.CREATE_OFFER;
import org.freedesktop.gstreamer.elements.WebRTCBin.ON_ICE_CANDIDATE;
import org.freedesktop.gstreamer.elements.WebRTCBin.ON_NEGOTIATION_NEEDED;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;

import static org.asynchttpclient.Dsl.asyncHttpClient;

/**
 * Demo gstreamer app for negotiating and streaming a sendrecv webrtc stream
 * with a browser JS app.
 *
 * @author stevevangasse
 */
public class WebrtcSendRecv {

    private static final Logger logger = LoggerFactory.getLogger(WebrtcSendRecv.class);
    private static final String SERVER_URL = "wss://webrtc.nirbheek.in:8443";
    private static final String VIDEO_BIN_DESCRIPTION = "videotestsrc ! videoconvert ! queue ! vp8enc deadline=1 ! rtpvp8pay ! queue ! capsfilter caps=application/x-rtp,media=video,encoding-name=VP8,payload=97";
    private static final String AUDIO_BIN_DESCRIPTION = "audiotestsrc ! audioconvert ! audioresample ! queue ! opusenc ! rtpopuspay ! queue ! capsfilter caps=application/x-rtp,media=audio,encoding-name=OPUS,payload=96";

    private final String sessionId;
    private final ObjectMapper mapper = new ObjectMapper();
    private WebSocket websocket;
    private WebRTCBin webRTCBin;
    private Pipeline pipe;

    public static void main(String[] args) throws Exception {
        if (args.length == 0) {
            logger.error("Please pass a session id from the signalling server: java -jar build/libs/gst-java.jar 1234");
            return;
        }
        String sessionId = args[0];
        logger.info("Using session id from args: " + sessionId);
        WebrtcSendRecv webrtcSendRecv = new WebrtcSendRecv(sessionId);
        webrtcSendRecv.startCall();
    }

    private WebrtcSendRecv(String sessionId) {
        this.sessionId = sessionId;
        Gst.init();
        webRTCBin = new WebRTCBin("sendrecv");

        Bin video = Gst.parseBinFromDescription(VIDEO_BIN_DESCRIPTION, true);
//        Bin audio = Gst.parseBinFromDescription(AUDIO_BIN_DESCRIPTION, true);

        pipe = new Pipeline();
        pipe.addMany(webRTCBin, video);
        video.link(webRTCBin);
//        audio.link(webRTCBin);
        setupPipeLogging(pipe);

        // When the pipeline goes to PLAYING, the on_negotiation_needed() callback will be called, and we will ask webrtcbin to create an offer which will match the pipeline above.
        webRTCBin.connect(onNegotiationNeeded);
        webRTCBin.connect(onIceCandidate);
        webRTCBin.connect(onIncomingStream);
    }

    private void startCall() throws Exception {
        websocket = asyncHttpClient()
                .prepareGet(SERVER_URL)
                .execute(
                        new WebSocketUpgradeHandler
                                .Builder()
                                .addWebSocketListener(webSocketListener)
                                .build())
                .get();

        Gst.main();
    }

    private WebSocketListener webSocketListener = new WebSocketListener() {

        @Override
        public void onOpen(WebSocket websocket) {
            logger.info("websocket onOpen");
            websocket.sendTextFrame("HELLO 852978");
        }

        @Override
        public void onClose(WebSocket websocket, int code, String reason) {
            logger.info("websocket onClose: " + code + " : " + reason);
        }

        @Override
        public void onTextFrame(String payload, boolean finalFragment, int rsv) {
            if (payload.equals("HELLO")) {
                websocket.sendTextFrame("SESSION " + sessionId);
            } else if (payload.equals("SESSION_OK")) {
                pipe.play();
            } else if (payload.startsWith("ERROR")) {
                logger.error(payload);
                Gst.quit();
            } else {
                handleSdp(payload);
            }
        }

        @Override
        public void onError(Throwable t) {
            logger.error("onError", t);
        }
    };

    private void handleSdp(String payload) {
        try {
            JsonNode answer = mapper.readTree(payload);
            if (answer.has("sdp")) {
                String sdpStr = answer.get("sdp").get("sdp").textValue();
                logger.info("answer SDP:\n{}", sdpStr);
                SDPMessage sdpMessage = new SDPMessage();
                sdpMessage.parseBuffer(sdpStr);
                WebRTCSessionDescription description = new WebRTCSessionDescription(WebRTCSDPType.ANSWER, sdpMessage);
                webRTCBin.setRemoteDescription(description);
            }
            else if (answer.has("ice")) {
                String candidate = answer.get("ice").get("candidate").textValue();
                int sdpMLineIndex = answer.get("ice").get("sdpMLineIndex").intValue();
                logger.info("Adding ICE candidate: {}", candidate);
                webRTCBin.addIceCandidate(sdpMLineIndex, candidate);
            }
        } catch (IOException e) {
            logger.error("Problem reading payload", e);
        }
    }

    private CREATE_OFFER onOfferCreated = offer -> {
        logger.info("createOffer called");
        webRTCBin.setLocalDescription(offer);

        JsonNode rootNode = mapper.createObjectNode();
        JsonNode sdpNode = mapper.createObjectNode();
        ((ObjectNode) sdpNode).put("type", "offer");
        ((ObjectNode) sdpNode).put("sdp", offer.getSDPMessage().toString().replace("sha-256 (null)", "sha-256 48:F5:B7:AA:35:38:E8:81:93:7B:10:F1:BE:74:8D:54:EB:8C:51:ED:4D:9B:84:D2:88:4A:EC:B7:7D:7C:2E:00"));
        ((ObjectNode) rootNode).set("sdp", sdpNode);

        try {
            String json = mapper.writeValueAsString(rootNode);
            logger.info(json);
            websocket.sendTextFrame(json);
        } catch (JsonProcessingException e) {
            logger.error("Couldn't write JSON", e);
        }
    };

    private ON_NEGOTIATION_NEEDED onNegotiationNeeded = elem -> {
        logger.info("onNegotiationNeeded: " + elem.getName());

        // When webrtcbin has created the offer, it will hit our callback and we send SDP offer over the websocket to signalling server
        webRTCBin.createOffer(onOfferCreated);
    };

    private ON_ICE_CANDIDATE onIceCandidate = (sdpMLineIndex, candidate) -> {
        JsonNode rootNode = mapper.createObjectNode();
        JsonNode iceNode = mapper.createObjectNode();
        ((ObjectNode) iceNode).put("candidate", candidate);
        ((ObjectNode) iceNode).put("sdpMLineIndex", sdpMLineIndex);
        ((ObjectNode) rootNode).set("ice", iceNode);

        try {
            String json = mapper.writeValueAsString(rootNode);
            logger.info("ON_ICE_CANDIDATE: " + json);
            websocket.sendTextFrame(json);
        } catch (JsonProcessingException e) {
            logger.error("Couldn't write JSON", e);
        }
    };

    private PAD_ADDED onIncomingStream = (element, pad) -> {
        logger.info("Receiving stream! Element: {} Pad: {}", element.getName(), pad.getName());
        // TODO: Send stream to autovideosink and autoaudiosink
    };

    private void setupPipeLogging(Pipeline pipe) {
        Bus bus = pipe.getBus();
        bus.connect((Bus.EOS) source -> {
            logger.info("Reached end of stream: " + source.toString());
            Gst.quit();
        });

        bus.connect((Bus.ERROR) (source, code, message) -> {
            logger.error("Error from source: '{}', with code: {}, and message '{}'", source, code, message);
        });

        bus.connect((source, old, current, pending) -> {
            if (source instanceof Pipeline) {
                logger.info("Pipe state changed from {} to new {}", old, current);
            }
        });
    }
}

