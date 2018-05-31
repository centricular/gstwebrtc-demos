extern crate glib;
extern crate gstreamer as gst;
extern crate gstreamer_sdp;
extern crate gstreamer_sdp_sys;
extern crate gstreamer_webrtc;
extern crate ws;
#[macro_use]
extern crate serde_json;

use glib::translate::*;
use gst::prelude::*;
use gst::{BinExt, ElementExt};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

#[derive(PartialEq, Eq, Debug)]
enum AppState {
    // AppStateUnknown = 0,
    AppStateErr = 1,
    ServerConnecting = 1000,
    ServerConnectionError,
    ServerConnected,
    ServerRegistering = 2000,
    ServerRegisteringError,
    ServerRegistered,
    ServerClosed,
    PeerConnecting = 3000,
    PeerConnectionError,
    PeerConnected,
    PeerCallNegotiating = 4000,
    PeerCallStarted,
    PeerCallStopping,
    PeerCallStopped,
    PeerCallError,
    None,
}

fn check_plugins() -> bool {
    let needed = vec![
        "opus",
        "vpx",
        "nice",
        "webrtc",
        "dtls",
        "srtp",
        "rtpmanager",
        "videotestsrc",
        "audiotestsrc",
    ];
    let registry = gst::Registry::get();
    let mut ret = true;
    for plugin_name in needed {
        let plugin = registry.find_plugin(&plugin_name.to_string());
        if plugin.is_none() {
            println!("Required gstreamer plugin '{}' not found", plugin_name);
            ret = false;
        }
    }
    return ret;
}

fn setup_call(app_state: &Arc<AtomicUsize>, out: &ws::Sender) -> AppState {
    app_state.store(AppState::PeerConnecting as usize, Ordering::Relaxed);
    out.send("SESSION 1").unwrap();
    return AppState::PeerConnecting;
}

fn register_with_server(app_state: &Arc<AtomicUsize>, out: &ws::Sender) -> AppState {
    app_state.store(AppState::ServerRegistering as usize, Ordering::Relaxed);
    out.send("HELLO 2").unwrap();
    return AppState::ServerRegistering;
}

fn sdp_message_as_text(offer: gstreamer_webrtc::WebRTCSessionDescription) -> Option<String> {
    unsafe {
        from_glib_full(gstreamer_sdp_sys::gst_sdp_message_as_text(
            (*offer.to_glib_none().0).sdp,
        ))
    }
}

fn send_sdp_offer(
    app_state: &Arc<AtomicUsize>,
    offer: gstreamer_webrtc::WebRTCSessionDescription,
    out: &ws::Sender,
) {
    if app_state.load(Ordering::Relaxed) < (AppState::PeerCallNegotiating as usize) {
        // TODO signal and cleanup
        panic!("Can't send offer, not in call");
    };
    let message = json!({
          "sdp": {
            "type": "offer",
            "sdp": sdp_message_as_text(offer).unwrap(),
          }
        });
    out.send(message.to_string()).unwrap();
}

fn on_offer_created(
    app_state: &Arc<AtomicUsize>,
    webrtc: gst::Element,
    promise: &gst::Promise,
    out: &ws::Sender,
) {
    assert_eq!(
        app_state.load(Ordering::Relaxed),
        AppState::PeerCallNegotiating as usize
    );
    let reply = promise.get_reply().unwrap();

    println!("{:?}", reply);

    let offer = reply
        .get_value("offer")
        .unwrap()
        .get::<gstreamer_webrtc::WebRTCSessionDescription>()
        .expect("Invalid argument");
    println!("{:?}", offer);
    let promise = gst::Promise::new();
    webrtc
        .emit(
            "set-local-description",
            &[&offer.to_value(), &promise.to_value()],
        )
        .unwrap();
    send_sdp_offer(app_state, offer, out)
}

fn on_negotiation_needed(
    app_state: &Arc<AtomicUsize>,
    values: &[glib::Value],
    out: &ws::Sender,
) -> Option<glib::Value> {
    println!("on-negotiation-needed {:?}", values);
    app_state.store(AppState::PeerCallNegotiating as usize, Ordering::Relaxed);

    let webrtc = values[0].get::<gst::Element>().expect("Invalid argument");
    let webrtc_clone = webrtc.clone();
    let out_clone = out.clone();
    let app_state_clone = app_state.clone();
    println!("{:?}", webrtc);
    let promise = gst::Promise::new_with_change_func(move |promise: &gst::Promise| {
        on_offer_created(&app_state_clone, webrtc, promise, &out_clone);
    });
    let options = gst::Structure::new_empty("options");
    let desc = webrtc_clone
        .emit("create-offer", &[&options.to_value(), &promise.to_value()])
        .unwrap();
    println!("{:?}", desc);
    None
}

fn handle_media_stream(pad: &gst::Pad, pipe: &gst::Element, convert_name: &str, sink_name: &str) {
    println!(
        "Trying to handle stream with {} ! {}",
        convert_name, sink_name,
    );
    let q = gst::ElementFactory::make("queue", None).unwrap();
    let conv = gst::ElementFactory::make(convert_name, None).unwrap();
    let sink = gst::ElementFactory::make(sink_name, None).unwrap();

    if convert_name == "audioconvert" {
        let resample = gst::ElementFactory::make("audioresample", None).unwrap();
        pipe.clone()
            .dynamic_cast::<gst::Bin>()
            .unwrap()
            .add_many(&[&q, &conv, &resample, &sink])
            .unwrap();
        q.sync_state_with_parent().unwrap();
        conv.sync_state_with_parent().unwrap();
        resample.sync_state_with_parent().unwrap();
        sink.sync_state_with_parent().unwrap();
        gst::Element::link_many(&[&q, &conv, &resample, &sink]).unwrap();
    } else {
        pipe.clone()
            .dynamic_cast::<gst::Bin>()
            .unwrap()
            .add_many(&[&q, &conv, &sink])
            .unwrap();
        q.sync_state_with_parent().unwrap();
        conv.sync_state_with_parent().unwrap();
        sink.sync_state_with_parent().unwrap();
        gst::Element::link_many(&[&q, &conv, &sink]).unwrap();
    }
    let qpad = q.get_static_pad("sink").unwrap();
    let ret = pad.link(&qpad);
    assert_eq!(ret, gst::PadLinkReturn::Ok);
}

fn on_incoming_decodebin_stream(
    values: &[glib::Value],
    pipe: &gst::Element,
) -> Option<glib::Value> {
    let pad = values[1].get::<gst::Pad>().expect("Invalid argument");
    if !pad.has_current_caps() {
        println!("Pad {:?} has no caps, can't do anything, ignoring", pad);
    }

    let caps = pad.get_current_caps().unwrap();
    let name = caps.get_structure(0).unwrap().get_name();
    println!("CAPS NAME {:?}", name);
    if name.starts_with("video") {
        handle_media_stream(&pad, &pipe, "videoconvert", "autovideosink");
    } else if name.starts_with("audio") {
        handle_media_stream(&pad, &pipe, "audioconvert", "autoaudiosink");
    } else {
        println!("Unknown pad {:?}, ignoring", pad);
    }
    None
}

fn on_incoming_stream(values: &[glib::Value], pipeline: &gst::Element) -> Option<glib::Value> {
    let webrtc = values[0].get::<gst::Element>().expect("Invalid argument");
    let decodebin = gst::ElementFactory::make("decodebin", None).unwrap();
    let pipeline_clone = pipeline.clone();
    decodebin
        .connect("pad-added", false, move |values| {
            on_incoming_decodebin_stream(values, &pipeline_clone)
        })
        .unwrap();
    pipeline
        .clone()
        .dynamic_cast::<gst::Bin>()
        .unwrap()
        .add(&decodebin)
        .unwrap();
    decodebin.sync_state_with_parent().unwrap();
    webrtc.link(&decodebin).unwrap();
    None
}

fn send_ice_candidate_message(
    app_state: &Arc<AtomicUsize>,
    values: &[glib::Value],
    out: &ws::Sender,
) -> Option<glib::Value> {
    if app_state.load(Ordering::Relaxed) < (AppState::PeerCallNegotiating as usize) {
        panic!("Can't send ICE, not in call");
    }

    let _webrtc = values[0].get::<gst::Element>().expect("Invalid argument");
    let mlineindex = values[1].get::<u32>().expect("Invalid argument");
    let candidate = values[2].get::<String>().expect("Invalid argument");
    let message = json!({
          "ice": {
            "candidate": candidate,
            "sdpMLineIndex": mlineindex,
          }
        });
    println!("{}", message.to_string());
    out.send(message.to_string()).unwrap();
    None
}

fn start_pipeline(app_state: &Arc<AtomicUsize>, out: &ws::Sender) -> gst::Element {
    let pipeline = gst::parse_launch(
        "webrtcbin name=sendrecv
        stun-server=stun://stun.l.google.com:19302
        videotestsrc pattern=ball ! videoconvert ! queue ! vp8enc deadline=1 ! rtpvp8pay !
        queue !
        application/x-rtp,media=video,encoding-name=VP8,payload=96 ! sendrecv.
        audiotestsrc wave=red-noise ! audioconvert ! audioresample ! queue ! opusenc ! rtpopuspay !
        queue !
        application/x-rtp,media=audio,encoding-name=OPUS,payload=97 ! sendrecv.
        ",
    ).unwrap();

    let webrtc = pipeline
        .clone()
        .dynamic_cast::<gst::Bin>()
        .unwrap()
        .get_by_name("sendrecv")
        .unwrap();

    webrtc.connect_pad_added(move |_, _src_pad| {
        println!("connnect pad added");
    });
    let out_clone = out.clone();
    let app_state_clone = app_state.clone();
    webrtc
        .connect("on-negotiation-needed", false, move |values| {
            on_negotiation_needed(&app_state_clone, values, &out_clone)
        })
        .unwrap();

    let out_clone = out.clone();
    let app_state_clone = app_state.clone();
    webrtc
        .connect("on-ice-candidate", false, move |values| {
            send_ice_candidate_message(&app_state_clone, values, &out_clone)
        })
        .unwrap();

    let pipeline_clone = pipeline.clone();
    webrtc
        .connect("pad-added", false, move |values| {
            on_incoming_stream(values, &pipeline_clone)
        })
        .unwrap();
    // TODO pad-added
    let ret = pipeline.set_state(gst::State::Playing);
    assert_ne!(ret, gst::StateChangeReturn::Failure);

    return webrtc;
}

struct WsClient {
    out: ws::Sender,
    webrtc: Option<gst::Element>,
    app_state: Arc<AtomicUsize>,
}

impl WsClient {
    fn update_state(&self, state: AppState) {
        self.app_state.store(state as usize, Ordering::Relaxed);
    }
}

impl ws::Handler for WsClient {
    fn on_open(&mut self, _: ws::Handshake) -> ws::Result<()> {
        self.update_state(AppState::ServerConnected);
        // TODO: replace with random
        self.update_state(register_with_server(&self.app_state, &self.out));
        Ok(())
    }

    fn on_message(&mut self, msg: ws::Message) -> ws::Result<()> {
        // Close the connection when we get a response from the server
        println!("Got message: {}", msg);
        let msg_text = msg.into_text().unwrap();
        let matched = match msg_text.as_ref() {
            "HELLO" => {
                if self.app_state.load(Ordering::Relaxed) != (AppState::ServerRegistering as usize)
                {
                    // TODO: signal and cleanup
                    panic!("ERROR: Received HELLO when not registering");
                }
                self.update_state(AppState::ServerRegistered);
                setup_call(&self.app_state, &self.out);
                true
            }
            "SESSION_OK" => {
                if self.app_state.load(Ordering::Relaxed) != (AppState::PeerConnecting as usize) {
                    // TODO: signal and cleanup
                    panic!("ERROR: Received SESSION_OK when not calling");
                }
                self.update_state(AppState::PeerConnected);
                self.webrtc = Some(start_pipeline(&self.app_state, &self.out));
                true
            }
            _ => false,
        };
        if matched {
            return Ok(());
        }
        if msg_text.starts_with("ERROR") {
            println!("Got error message! {}", msg_text);
            let error = match self.app_state.load(Ordering::Relaxed) {
                x if x == AppState::ServerConnecting as usize => AppState::ServerConnectionError,
                x if x == AppState::ServerRegistering as usize => AppState::ServerRegisteringError,
                x if x == AppState::PeerConnecting as usize => AppState::PeerConnectionError,
                x if x == AppState::PeerConnected as usize => AppState::PeerCallError,
                x if x == AppState::PeerCallNegotiating as usize => AppState::PeerCallError,
                _ => AppState::AppStateErr,
            };
            panic!("Got websocket error {:?}", error);
            // TODO: signal & cleanup
        }

        let json_msg: serde_json::Value = serde_json::from_str(&msg_text).unwrap();
        if json_msg.get("sdp").is_some() {
            assert_eq!(
                self.app_state.load(Ordering::Relaxed),
                (AppState::PeerCallNegotiating as usize)
            );

            if !json_msg["sdp"].get("type").is_some() {
                println!("ERROR: received SDP without 'type'");
                return Ok(());
            }
            let sdptype = &json_msg["sdp"]["type"];
            assert_eq!(sdptype, "answer");
            let text = &json_msg["sdp"]["sdp"];
            print!("Received answer:\n{}\n", text.as_str().unwrap());

            let ret =
                gstreamer_sdp::SDPMessage::parse_buffer(text.as_str().unwrap().as_bytes()).unwrap();
            let answer = gstreamer_webrtc::WebRTCSessionDescription::new(
                gstreamer_webrtc::WebRTCSDPType::Answer,
                ret,
            );
            let promise = gst::Promise::new();
            self.webrtc
                .as_ref()
                .unwrap()
                .emit(
                    "set-remote-description",
                    &[&answer.to_value(), &promise.to_value()],
                )
                .unwrap();
            self.update_state(AppState::PeerCallStarted);
        }
        if json_msg.get("ice").is_some() {
            println!("ice {:?}", json_msg);
            let candidate = json_msg["ice"]["candidate"].as_str().unwrap();
            let sdpmlineindex = json_msg["ice"]["sdpMLineIndex"].as_u64().unwrap() as u32;
            self.webrtc
                .as_ref()
                .unwrap()
                .emit(
                    "add-ice-candidate",
                    &[&sdpmlineindex.to_value(), &candidate.to_value()],
                )
                .unwrap();
        }

        Ok(())
        // self.out.close(ws::CloseCode::Normal)
    }
}

fn connect_to_websocket_server_async() {
    ws::connect("ws://signalling:8443", |out| WsClient {
        out: out,
        app_state: Arc::new(AtomicUsize::new(AppState::ServerConnecting as usize)),
        webrtc: None,
    }).unwrap();
}

fn main() {
    gst::init().unwrap();

    if !check_plugins() {
        return;
    }

    let main_loop = glib::MainLoop::new(None, false);
    connect_to_websocket_server_async();
    main_loop.run();
}
