extern crate clap;
extern crate glib;
extern crate gstreamer as gst;
extern crate gstreamer_sdp;
extern crate gstreamer_sdp_sys;
extern crate gstreamer_webrtc;
extern crate rand;
extern crate ws;
#[macro_use]
extern crate serde_json;

use gst::prelude::*;
use gst::{BinExt, ElementExt};
use rand::Rng;
use std::sync::{Arc, Mutex};

#[derive(PartialEq, PartialOrd, Eq, Debug)]
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
    PeerCallError,
}
#[derive(Debug)]
enum Error {
    GlibError(glib::Error),
    GlibBoolError(glib::BoolError),
    GstStateChangeError(gst::StateChangeError),
}

impl From<glib::Error> for Error {
    fn from(error: glib::Error) -> Self {
        Error::GlibError(error)
    }
}

impl From<glib::BoolError> for Error {
    fn from(error: glib::BoolError) -> Self {
        Error::GlibBoolError(error)
    }
}

impl From<gst::StateChangeError> for Error {
    fn from(error: gst::StateChangeError) -> Self {
        Error::GstStateChangeError(error)
    }
}

const STUN_SERVER: &'static str = "stun-server=stun://stun.l.google.com:19302 ";
const RTP_CAPS_OPUS: &'static str = "application/x-rtp,media=audio,encoding-name=OPUS,payload=";
const RTP_CAPS_VP8: &'static str = "application/x-rtp,media=video,encoding-name=VP8,payload=";

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

fn setup_call(app_control: &Arc<Mutex<AppControl>>) -> AppState {
    let mut app_control = app_control.lock().unwrap();
    app_control.app_state = AppState::PeerConnecting;
    println!(
        "Setting up signalling server call with {}",
        app_control.peer_id
    );
    app_control
        .ws_sender
        .send(format!("SESSION {}", app_control.peer_id))
        .unwrap();
    return AppState::PeerConnecting;
}

fn register_with_server(app_control: &Arc<Mutex<AppControl>>) -> AppState {
    let mut app_control = app_control.lock().unwrap();
    app_control.app_state = AppState::ServerRegistering;
    let our_id = rand::thread_rng().gen_range(10, 10_000);
    println!("Registering id {} with server", our_id);
    app_control
        .ws_sender
        .send(format!("HELLO {}", our_id))
        .unwrap();
    return AppState::ServerRegistering;
}

fn send_sdp_offer(
    app_control: &Arc<Mutex<AppControl>>,
    offer: gstreamer_webrtc::WebRTCSessionDescription,
) {
    let app_control = app_control.lock().unwrap();
    if app_control.app_state < AppState::PeerCallNegotiating {
        // TODO signal and cleanup
        panic!("Can't send offer, not in call");
    };
    let message = json!({
      "sdp": {
        "type": "offer",
        "sdp": offer.get_sdp().as_text().unwrap(),
      }
    });
    app_control.ws_sender.send(message.to_string()).unwrap();
}

fn on_offer_created(
    app_control: &Arc<Mutex<AppControl>>,
    webrtc: gst::Element,
    promise: &gst::Promise,
) {
    assert_eq!(
        app_control.lock().unwrap().app_state,
        AppState::PeerCallNegotiating
    );
    let reply = promise.get_reply().unwrap();

    let offer = reply
        .get_value("offer")
        .unwrap()
        .get::<gstreamer_webrtc::WebRTCSessionDescription>()
        .expect("Invalid argument");
    webrtc
        .emit("set-local-description", &[&offer, &None::<gst::Promise>])
        .unwrap();

    send_sdp_offer(app_control, offer)
}

fn on_negotiation_needed(
    app_control: &Arc<Mutex<AppControl>>,
    values: &[glib::Value],
) -> Option<glib::Value> {
    app_control.lock().unwrap().app_state = AppState::PeerCallNegotiating;
    let webrtc = values[0].get::<gst::Element>().expect("Invalid argument");
    let webrtc_clone = webrtc.clone();
    let app_control_clone = app_control.clone();
    let promise = gst::Promise::new_with_change_func(move |promise: &gst::Promise| {
        on_offer_created(&app_control_clone, webrtc, promise);
    });
    let options = gst::Structure::new_empty("options");
    webrtc_clone
        .emit("create-offer", &[&options, &promise])
        .unwrap();
    None
}

fn handle_media_stream(
    pad: &gst::Pad,
    pipe: &gst::Element,
    convert_name: &str,
    sink_name: &str,
) -> Result<(), Error> {
    println!(
        "Trying to handle stream with {} ! {}",
        convert_name, sink_name,
    );
    let q = gst::ElementFactory::make("queue", None).unwrap();
    let conv = gst::ElementFactory::make(convert_name, None).unwrap();
    let sink = gst::ElementFactory::make(sink_name, None).unwrap();

    let pipe_bin = pipe.clone().dynamic_cast::<gst::Bin>().unwrap();

    if convert_name == "audioconvert" {
        let resample = gst::ElementFactory::make("audioresample", None).unwrap();
        pipe_bin.add_many(&[&q, &conv, &resample, &sink])?;
        q.sync_state_with_parent()?;
        conv.sync_state_with_parent()?;
        resample.sync_state_with_parent()?;
        sink.sync_state_with_parent()?;
        gst::Element::link_many(&[&q, &conv, &resample, &sink])?;
    } else {
        pipe_bin.add_many(&[&q, &conv, &sink])?;
        q.sync_state_with_parent()?;
        conv.sync_state_with_parent()?;
        sink.sync_state_with_parent()?;
        gst::Element::link_many(&[&q, &conv, &sink])?;
    }
    let qpad = q.get_static_pad("sink").unwrap();
    let ret = pad.link(&qpad);
    assert_eq!(ret, gst::PadLinkReturn::Ok);
    Ok(())
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
    match if name.starts_with("video") {
        handle_media_stream(&pad, &pipe, "videoconvert", "autovideosink")
    } else if name.starts_with("audio") {
        handle_media_stream(&pad, &pipe, "audioconvert", "autoaudiosink")
    } else {
        println!("Unknown pad {:?}, ignoring", pad);
        Ok(())
    } {
        Ok(()) => return None,
        Err(err) => panic!("Error adding pad with caps {} {:?}", name, err),
    };
}

fn on_incoming_stream(values: &[glib::Value], pipe: &gst::Element) -> Option<glib::Value> {
    let webrtc = values[0].get::<gst::Element>().expect("Invalid argument");
    let decodebin = gst::ElementFactory::make("decodebin", None).unwrap();
    let pipe_clone = pipe.clone();
    decodebin
        .connect("pad-added", false, move |values| {
            on_incoming_decodebin_stream(values, &pipe_clone)
        })
        .unwrap();
    pipe.clone()
        .dynamic_cast::<gst::Bin>()
        .unwrap()
        .add(&decodebin)
        .unwrap();
    decodebin.sync_state_with_parent().unwrap();
    webrtc.link(&decodebin).unwrap();
    None
}

fn send_ice_candidate_message(
    app_control: &Arc<Mutex<AppControl>>,
    values: &[glib::Value],
) -> Option<glib::Value> {
    let app_control = app_control.lock().unwrap();
    if app_control.app_state < AppState::PeerCallNegotiating {
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
    app_control.ws_sender.send(message.to_string()).unwrap();
    None
}

fn start_pipeline(app_control: &Arc<Mutex<AppControl>>) -> Result<gst::Element, Error> {
    let pipe = gst::parse_launch(&format!(
        "webrtcbin name=sendrecv {}
        videotestsrc pattern=ball ! videoconvert ! queue ! vp8enc deadline=1 ! rtpvp8pay !
        queue ! {}96 ! sendrecv.
        audiotestsrc wave=red-noise ! audioconvert ! audioresample ! queue ! opusenc ! rtpopuspay !
        queue ! {}97 ! sendrecv.
        ",
        STUN_SERVER, RTP_CAPS_VP8, RTP_CAPS_OPUS
    ))?;

    let webrtc = pipe.clone()
        .dynamic_cast::<gst::Bin>()
        .unwrap()
        .get_by_name("sendrecv")
        .unwrap();
    let app_control_clone = app_control.clone();
    webrtc.connect("on-negotiation-needed", false, move |values| {
        on_negotiation_needed(&app_control_clone, values)
    })?;

    let app_control_clone = app_control.clone();
    webrtc.connect("on-ice-candidate", false, move |values| {
        send_ice_candidate_message(&app_control_clone, values)
    })?;

    let pipe_clone = pipe.clone();
    // TODO: replace with webrtc.connect_pad_added
    webrtc.connect("pad-added", false, move |values| {
        on_incoming_stream(values, &pipe_clone)
    })?;

    pipe.set_state(gst::State::Playing).into_result()?;

    return Ok(webrtc);
}

struct WsClient {
    webrtc: Option<gst::Element>,
    app_control: Arc<Mutex<AppControl>>,
}

struct AppControl {
    app_state: AppState,
    ws_sender: ws::Sender,
    peer_id: String,
}

impl WsClient {
    fn update_state(&self, state: AppState) {
        self.app_control.lock().unwrap().app_state = state
    }
}

impl ws::Handler for WsClient {
    fn on_open(&mut self, _: ws::Handshake) -> ws::Result<()> {
        self.update_state(AppState::ServerConnected);
        self.update_state(register_with_server(&self.app_control.clone()));
        Ok(())
    }

    fn on_message(&mut self, msg: ws::Message) -> ws::Result<()> {
        // Close the connection when we get a response from the server
        let msg_text = msg.into_text().unwrap();
        if msg_text == "HELLO" {
            if self.app_control.lock().unwrap().app_state != AppState::ServerRegistering {
                panic!("ERROR: Received HELLO when not registering");
            }
            self.update_state(AppState::ServerRegistered);
            setup_call(&self.app_control.clone());
            return Ok(());
        }
        if msg_text == "SESSION_OK" {
            if self.app_control.lock().unwrap().app_state != AppState::PeerConnecting {
                panic!("ERROR: Received SESSION_OK when not calling");
            }
            self.update_state(AppState::PeerConnected);
            self.webrtc = match start_pipeline(&self.app_control) {
                Ok(webrtc) => Some(webrtc),
                Err(err) => {
                    panic!("Failed to set up webrtc {:?}", err);
                }
            };
            return Ok(());
        }

        if msg_text.starts_with("ERROR") {
            println!("Got error message! {}", msg_text);
            let error = match self.app_control.lock().unwrap().app_state {
                AppState::ServerConnecting => AppState::ServerConnectionError,
                AppState::ServerRegistering => AppState::ServerRegisteringError,
                AppState::PeerConnecting => AppState::PeerConnectionError,
                AppState::PeerConnected => AppState::PeerCallError,
                AppState::PeerCallNegotiating => AppState::PeerCallError,
                AppState::ServerConnectionError => AppState::ServerConnectionError,
                AppState::ServerRegisteringError => AppState::ServerRegisteringError,
                AppState::PeerConnectionError => AppState::PeerConnectionError,
                AppState::PeerCallError => AppState::PeerCallError,
                AppState::AppStateErr => AppState::AppStateErr,
                AppState::ServerConnected => AppState::AppStateErr,
                AppState::ServerRegistered => AppState::AppStateErr,
                AppState::ServerClosed => AppState::AppStateErr,
                AppState::PeerCallStarted => AppState::AppStateErr,
            };
            self.app_control
                .lock()
                .unwrap()
                .ws_sender
                .close(ws::CloseCode::Normal)
                .unwrap();
            panic!("Got websocket error {:?}", error);
            // TODO: signal & cleanup
        }

        let json_msg: serde_json::Value = serde_json::from_str(&msg_text).unwrap();
        if json_msg.get("sdp").is_some() {
            assert_eq!(
                self.app_control.lock().unwrap().app_state,
                AppState::PeerCallNegotiating
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
                .emit("set-remote-description", &[&answer, &promise])
                .unwrap();
            self.update_state(AppState::PeerCallStarted);
        }
        if json_msg.get("ice").is_some() {
            let candidate = json_msg["ice"]["candidate"].as_str().unwrap();
            let sdpmlineindex = json_msg["ice"]["sdpMLineIndex"].as_u64().unwrap() as u32;
            self.webrtc
                .as_ref()
                .unwrap()
                .emit("add-ice-candidate", &[&sdpmlineindex, &candidate])
                .unwrap();
        }

        Ok(())
    }

    fn on_close(&mut self, _code: ws::CloseCode, _reason: &str) {
        self.app_control.lock().unwrap().app_state = AppState::ServerClosed;
    }
}

fn connect_to_websocket_server_async(peer_id: &str, server: &str) {
    println!("Connecting to server {}", server);
    ws::connect(server, |ws_sender| WsClient {
        webrtc: None,
        app_control: Arc::new(Mutex::new(AppControl {
            ws_sender: ws_sender,
            peer_id: peer_id.to_string(),
            app_state: AppState::ServerConnecting,
        })),
    }).unwrap();
}

fn main() {
    let matches = clap::App::new("Sendrcv rust")
        .arg(
            clap::Arg::with_name("peer-id")
                .help("String ID of the peer to connect to")
                .long("peer-id")
                .required(true)
                .takes_value(true),
        )
        .arg(
            clap::Arg::with_name("server")
                .help("Signalling server to connect to")
                .long("server")
                .required(false)
                .takes_value(true),
        )
        .get_matches();

    gst::init().unwrap();

    if !check_plugins() {
        return;
    }
    let main_loop = glib::MainLoop::new(None, false);
    connect_to_websocket_server_async(
        matches.value_of("peer-id").unwrap(),
        matches
            .value_of("server")
            .unwrap_or("wss://webrtc.nirbheek.in:8443"),
    );
    main_loop.run();
}
