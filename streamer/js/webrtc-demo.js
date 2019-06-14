/*
 * Javascript app for negotiating and streaming a webrtc video stream
 * with a GStreamer app. On connection to signalling server, a uid is
 * requested and all communication is addressed via that uid
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 * Author: Aiden Jeffrey <aiden.jeffrey@codethink.co.uk>
 */

// Set this to override the automatic detection in websocketServerConnect()
var ws_server;
var ws_port;
// Override with your own STUN servers if you want
var rtc_configuration = {iceServers: [{urls: "stun:stun.services.mozilla.com"},
                                      {urls: "stun:stun.l.google.com:19302"}]};

var connect_attempts = 0;
var client_uid = 0;
var peer_connection;
var send_channel;
var ws_conn;

var connect_button;
var disconnect_button;

var ice_candidate_queue = [];


/** Waits for document to load, then sets up listeners */
document.addEventListener("DOMContentLoaded", function(event) {

    connect_button = document.getElementById("connect_btn");
    connect_button.addEventListener("click", function() {
        cmd = {"command": {"type": "connect-to-mountpoint",
                           "data": null},
               "client_uid": client_uid}
        ws_conn.send(JSON.stringify(cmd));
    });

    disconnect_button = document.getElementById("disconnect_btn");
    disconnect_button.addEventListener("click", function() {
        console.log("Disconnecting");
        // Request mountpoints from media server
        cmd = {"command": {"type": "disconnect-mountpoint",
                           "data": null},
               "client_uid": client_uid}
        ws_conn.send(JSON.stringify(cmd));
    });
});


function resetState() {
    // This will call onServerClose()
    ws_conn.close();
}

function handleIncomingError(error) {
    setError("ERROR: " + error);
    resetState();
}

function getVideoElement() {
    return document.getElementById("stream");
}

function setStatus(text) {
    console.log(text);
    var span = document.getElementById("status")
    // Don't set the status if it already contains an error
    if (!span.classList.contains("error"))
        span.textContent = text;
}

function setError(text) {
    console.error(text);
    var span = document.getElementById("status")
    span.textContent = text;
    span.classList.add("error");
}

function resetVideo() {
    // Reset the video element and stop showing the last received frame
    var videoElement = getVideoElement();
    videoElement.pause();
    videoElement.src = "";
    videoElement.load();
}


// SDP offer received from peer, set remote description and create an answer
function onIncomingSDP(sdp) {
    peer_connection.setRemoteDescription(sdp).then(() => {
        setStatus("Remote SDP set");
        if (sdp.type != "offer")
            return;
        setStatus("Got SDP offer");
        while (ice_candidate_queue.length > 0) {
            var candidate = ice_candidate_queue.shift();
            peer_connection.addIceCandidate(candidate).catch(setError);
        }
        peer_connection.createAnswer()
            .then(onLocalDescription).catch(setError);
    }).catch(setError);
}


// Local description was set, send it to peer
function onLocalDescription(desc) {
    console.log("Got local description: " + JSON.stringify(desc));
    peer_connection.setLocalDescription(desc).then(function() {
        setStatus("Sending SDP answer");
        sdp = {"sdp": peer_connection.localDescription,
               "client_uid": client_uid}
        ws_conn.send(JSON.stringify(sdp));
    });
}


// ICE candidate received from peer, add it to the peer connection
function onIncomingICE(ice) {
    var candidate = new RTCIceCandidate(ice);
    if(!peer_connection || !peer_connection.remoteDescription.type){
        // put candidate on queue
        ice_candidate_queue.push(candidate);
    }
    else {
        peer_connection.addIceCandidate(candidate).catch(setError);
    }
}


function get_json_message(data) {
    try {
        msg = JSON.parse(data);
    } catch (e) {
        if (e instanceof SyntaxError) {
            handleIncomingError("Error parsing incoming JSON: " + data);
        } else {
            handleIncomingError("Unknown error parsing response: " + data);
        }
        return null;
    }
    return msg;
}


function remove_options(selectbox)
{
    var i;
    for (i = selectbox.options.length - 1 ; i >= 0 ; i--)
    {
        selectbox.remove(i);
    }
}


/** Processes command data
 * @param {json} msg - the json message to parse
 */
function process_command(msg) {
    console.log("process_command: ", msg);
    if (msg.command == null || !msg.success) {
        console.error("Command failed!", msg)
        return false;
    }
    else if (msg.command.type == "connect-to-mountpoint") {
        console.log("CONNECT TO MOUNTPOINT:", msg);
    }
    else if (msg.command.type == "disconnect-mountpoint") {
        console.log("DISCONNECT MOUNTPOINT:", msg);
        close_peer_connection();
    }
    else {
        console.error(
            "Command type not recognised! " + msg.command.type)
        return false;
    }
}


function onServerMessage(event) {
    console.log("onServerMessage:" + event.data);
    if (event.data.startsWith("ASSIGNED UID")) {
        var fields = event.data.split(" ");
        client_uid = Number(fields[2]);
        document.getElementById("client_uid").textContent = client_uid;
        setStatus("Registered with media server");
        return;
    }
    else {
        if (event.data.startsWith("ERROR")) {
            handleIncomingError(event.data);
            return;
        }
        // Handle incoming JSON SDP and ICE messages
        msg = get_json_message(event.data);
        if (msg == null) {
            return;
        }

        // Incoming JSON signals the beginning of streaming session
        if (msg.command != null) {
            process_command(msg);
        }
        else if (!peer_connection) {
            create_call(msg);
        }

        if (msg.sdp != null) {
            onIncomingSDP(msg.sdp);
        } else if (msg.ice != null) {
            onIncomingICE(msg.ice);
        } else if (msg.command != null) {
            console.log("Command mirror:", msg);
        } else {
            handleIncomingError("Unknown incoming JSON: " + msg);
        }
    }
}


function close_peer_connection() {
    if (peer_connection) {
        peer_connection.close();
        peer_connection = null;
    }
}


function onServerClose(event) {
    setStatus("Disconnected from server");
    resetVideo();

    close_peer_connection();

    // Reset after a second
    window.setTimeout(websocketServerConnect, 1000);
}

function onServerError(event) {
    setError("Unable to connect to server, did you add an exception for the certificate?")
    // Retry after 3 seconds
    window.setTimeout(websocketServerConnect, 3000);
}

function websocketServerConnect() {
    connect_attempts++;
    if (connect_attempts > 3) {
        setError("Too many connection attempts, aborting. Refresh page to try again");
        return;
    }
    // Clear errors in the status span
    var span = document.getElementById("status");
    span.classList.remove("error");
    span.textContent = "";
    // Fetch the peer id to use
    ws_port = ws_port || "8443";
    if (window.location.protocol.startsWith ("file")) {
        ws_server = ws_server || "127.0.0.1";
    } else if (window.location.protocol.startsWith ("http")) {
        ws_server = ws_server || window.location.hostname;
    } else {
        throw new Error ("Don't know how to connect to the signalling server with uri" + window.location);
    }
    var ws_url = "ws://" + ws_server + ":" + ws_port;
    setStatus("Connecting to server " + ws_url);
    ws_conn = new WebSocket(ws_url);
    /* When connected, immediately register with the server */
    ws_conn.addEventListener("open", (event) => {
        ws_conn.send("REGISTER CLIENT");
        setStatus("Registering with signalling server");
    });
    ws_conn.addEventListener("error", onServerError);
    ws_conn.addEventListener("message", onServerMessage);
    ws_conn.addEventListener("close", onServerClose);
}

function onRemoteTrack(event) {
    if (getVideoElement().srcObject !== event.streams[0]) {
        console.log("Incoming stream");
        getVideoElement().srcObject = event.streams[0];
    }
}

function errorUserMediaHandler() {
    setError("Browser doesn't support getUserMedia!");
}

const handleDataChannelOpen = (event) =>{
    console.log("dataChannel.OnOpen", event);
};

const handleDataChannelMessageReceived = (event) =>{
    console.log("dataChannel.OnMessage:", event, event.data.type);

    setStatus("Received data channel message");
    if (typeof event.data === "string" || event.data instanceof String) {
        console.log("Incoming string message: " + event.data);
        textarea = document.getElementById("text")
        textarea.value = textarea.value + "\n" + event.data
    } else {
        console.log("Incoming data message");
    }
    send_channel.send("Hi! (from browser)");
};

const handleDataChannelError = (error) =>{
    console.log("dataChannel.OnError:", error);
};

const handleDataChannelClose = (event) =>{
    console.log("dataChannel.OnClose", event);
};

function onDataChannel(event) {
    setStatus("Data channel created");
    let receiveChannel = event.channel;
    receiveChannel.onopen = handleDataChannelOpen;
    receiveChannel.onmessage = handleDataChannelMessageReceived;
    receiveChannel.onerror = handleDataChannelError;
    receiveChannel.onclose = handleDataChannelClose;
}


/** This should be called when sdp is sent from Media Server */
function create_call(msg) {
    console.log("CREATE_CALL")
    // Reset connection attempts because we connected successfully
    connect_attempts = 0;

    console.log("Creating RTCPeerConnection");

    peer_connection = new RTCPeerConnection(rtc_configuration);
    send_channel = peer_connection.createDataChannel("label", null);
    send_channel.onopen = handleDataChannelOpen;
    send_channel.onmessage = handleDataChannelMessageReceived;
    send_channel.onerror = handleDataChannelError;
    send_channel.onclose = handleDataChannelClose;
    peer_connection.ondatachannel = onDataChannel;
    peer_connection.ontrack = onRemoteTrack;

    if (!msg.sdp) {
        console.log("WARNING: First message wasn't an SDP message!?", msg);
    }

    peer_connection.onicecandidate = (event) => {
    // We have a candidate, send it to the remote party with the
    // same uuid
    if (event.candidate == null) {
            console.log("ICE Candidate was null, done");
            return;
    }
    ws_conn.send(JSON.stringify({"ice": event.candidate,
                                 "client_uid": client_uid}));
    };

    setStatus("Created peer connection for call, waiting for SDP");
}
