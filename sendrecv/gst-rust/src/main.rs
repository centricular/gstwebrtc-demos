extern crate glib;
extern crate gstreamer as gst;
extern crate ws;
use gst::prelude::*;
use gst::{BinExt, ElementExt};
// use std::io;

#[derive(PartialEq, Eq)]
enum AppState {
    AppStateUnknown,
    AppStateErr,
    ServerConnecting,
    ServerConnectionError,
    ServerConnected,
    ServerRegistering,
    ServerRegisteringError,
    ServerRegistered,
    ServerClosed,
    PeerConnecting,
    PeerConnectionError,
    PeerConnected,
    PeerCallNegotiating,
    PeerCallStarted,
    PeerCallStopping,
    PeerCallStopped,
    PeerCallError,
    None,
}

struct WsClient {
    out: ws::Sender,
    app_state: AppState,
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

fn setup_call(out: &ws::Sender) -> AppState {
    out.send("SESSION 1").unwrap();
    return AppState::PeerConnecting;
}

fn register_with_server(out: &ws::Sender) -> AppState {
    out.send("HELLO 2").unwrap();
    return AppState::ServerRegistering;
}

fn start_pipeline(_out: &ws::Sender) -> AppState {
    let pipeline = gst::parse_launch(
        "webrtcbin name=sendrecv
        stun-server=stun://stun.l.google.com:19302
        videotestsrc pattern=ball ! videoconvert ! queue ! vp8enc deadline=1 ! rtpvp8pay !
        queue !
        application/x-rtp,media=video,encoding-name=VP8,payload=
        96 ! sendrecv.
        audiotestsrc wave=red-noise ! audioconvert ! audioresample ! queue ! opusenc ! rtpopuspay !
        queue !
        application/x-rtp,media=audio,encoding-name=OPUS,payload=
        97 ! sendrecv.
        ",
    ).unwrap();
    let webrtc1 = pipeline
        .clone()
        .dynamic_cast::<gst::Bin>()
        .unwrap()
        .get_by_name("sendrecv")
        .unwrap();

    // let bus = webrtc1.get_bus().unwrap();
    // bus.add_watch(move |foo, msg| {
    //     println!("{:?}", foo);
    //     println!("{:?}", msg);
    //     glib::Continue(true)
    // });

    webrtc1.connect_pad_added(move |_, _src_pad| {
        println!("connnect pad added");
    });
    let ret = webrtc1.connect("on-negotiation-needed", false, |values| {
        println!("on-negotiation-needed {:?}", values);
        None
    });
    println!("connect ret ONN {:?}", ret);
    let ret = pipeline.set_state(gst::State::Playing);
    assert_ne!(ret, gst::StateChangeReturn::Failure);

    return AppState::None;
}

impl ws::Handler for WsClient {
    fn on_open(&mut self, _: ws::Handshake) -> ws::Result<()> {
        // TODO: replace with random
        self.app_state = register_with_server(&self.out);
        Ok(())
    }

    fn on_message(&mut self, msg: ws::Message) -> ws::Result<()> {
        // Close the connection when we get a response from the server
        println!("Got message: {}", msg);
        let new_state = match format!("{}", msg).as_ref() {
            "HELLO" => {
                self.app_state = AppState::ServerRegistered;
                setup_call(&self.out)
            }
            "SESSION_OK" => {
                self.app_state = AppState::PeerConnected;
                start_pipeline(&self.out)
            }
            _ => AppState::None,
        };
        if new_state != AppState::None {
            self.app_state = new_state;
        }
        Ok(())
        // self.out.close(ws::CloseCode::Normal)
    }
}

fn connect_to_websocket_server_async() {
    let result = ws::connect("ws://signalling:8443", |out| WsClient {
        out: out,
        app_state: AppState::AppStateUnknown,
    });
    match result {
        Ok(_) => {
            println!("Connected to");
        }
        Err(error) => panic!("There was a problem opening the file: {:?}", error),
    };
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
