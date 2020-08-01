#!/usr/bin/env python3
#
# streaming signalling server
#
# Copyright (C) 2019 Codethink Ltd.
# Copyright (C) 2017 Centricular Ltd.
#
#  Author: Nirbheek Chauhan <nirbheek@centricular.com>
#  Author: Aiden Jeffrey <aiden.jeffrey@codethink.co.uk>
#

import argparse
import asyncio
import json
import logging
import os
import random
import sys
import websockets

from concurrent.futures._base import TimeoutError

parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument('--addr', default='0.0.0.0', help='Address to listen on')
parser.add_argument('--port', default=8443, type=int, help='Port to listen on')
parser.add_argument('--keepalive-timeout', dest='keepalive_timeout', default=30, type=int, help='Timeout for keepalive (in seconds)')

options = parser.parse_args(sys.argv[1:])

ADDR_PORT         = (options.addr, options.port)
KEEPALIVE_TIMEOUT = options.keepalive_timeout
MAX_NUM_CLIENTS   = 1000


class Peer:
    def __init__(self, web_socket, in_session=False):
        self.web_socket = web_socket
        self.remote_address = web_socket.remote_address

        self.in_session = in_session


class SignalError(Exception):
    pass


clients = dict()
media_server = None


async def report_and_log_error(msg, socket=None):
    logger.error(msg)
    if socket:
        await socket.send(msg)


async def add_client(client):
    while True:
        uid = random.randint(0, MAX_NUM_CLIENTS)
        if uid not in clients:
            break
    clients[uid] = client
    return uid


############### Helper functions ###############

async def recv_msg_ping(web_socket):
    """
    Wait for a message forever, and send a regular ping to prevent bad routers
    from closing the connection.
    """
    msg = None
    while msg is None:
        try:
            msg = await asyncio.wait_for(web_socket.recv(), KEEPALIVE_TIMEOUT)
        except TimeoutError:
            print('Sending keepalive ping to {!r} in recv'.format(
                web_socket.remote_address))
            await web_socket.ping()
    return msg


async def remove_peer(uid):
    global media_server
    if uid in clients:
        # Let gstreamer know that this client died
        if media_server is not None:
            await media_server.web_socket.send(
                "UNBIND-SESSION-CLIENT {}".format(uid))
        else:
            raise SignalError("No media server to deregister from")
        await clients[uid].web_socket.close()
        del clients[uid]
        print("Cleaned up and disconnected from client {}".format(uid))
    elif uid == None:
        if media_server is not None:
            await media_server.web_socket.close()
            print("Cleaned up and disconnected from media server")
            media_server = None

############### Handler functions ###############

async def connection_handler(uid, peer, is_server=False):
    """ Once the peer is registered, the websocket's messages are bound
        to this function
    """
    global clients, media_server

    while True:
        # Receive command, wait forever if necessary
        msg = await recv_msg_ping(peer.web_socket)
        print("connection_handler: ", msg)
        if is_server:
            if msg.startswith("SESSION "):
                print("SESSION MESSAGE: {}".format(msg))
                _, client_uid, server_msg = msg.split(maxsplit=2)
                try:
                    client = clients[int(client_uid)]
                except (KeyError, ValueError):
                    print("CLIENTS:", clients, type(client_uid))
                    await report_and_log_error(
                        "ERROR session: unknown client uid msg {}".format(msg))
                else:
                    if server_msg == "BOUND":
                        client.in_session = True
                    elif server_msg == "UNBOUND":
                        client.in_session = False
                    else:
                        await report_and_log_error(
                            "ERROR Unknown SESSION report from"
                            " server {}".format(msg))
            else:
                print("FWD MESSAGE: {}".format(msg))
                # message should be forwarded to client, the uid of which
                # should be embedded in message
                try:
                    msg_data = json.loads(msg)
                except json.JSONDecodeError:
                    await peer.web_socket.send(
                        "ERROR bad data sent"
                        " msg {}".format(msg))
                if ("client_uid" not in msg_data) or (
                        msg_data["client_uid"] not in clients):
                    await report_and_log_error(
                        "ERROR forward unknown client uid"
                        " msg {}".format(msg))
                else:
                    client_uid = msg_data["client_uid"]
                    print("media-server -> {}: {}".format(client_uid, msg))
                    await clients[client_uid].web_socket.send(msg)

        else:
            # client connection
            print("{} -> media-server: {}".format(uid, msg))
            await media_server.web_socket.send(msg)


async def register_peer(web_socket):
    """
    Exchange hello, register peer
    """
    global media_server

    new_peer = Peer(web_socket, False)
    msg = await web_socket.recv()
    print("register_peer: ", msg)
    uid = None
    if msg == "REGISTER CLIENT":
        # TODO: do we need to restrict clients somehow (i.e. one per address)?
        uid = await add_client(new_peer)
        # Send back the assined uid
        await web_socket.send("ASSIGNED UID {}".format(uid))
        # inform gstreamer of the client
        if media_server is not None:
            msg = "BIND-SESSION-CLIENT {}".format(uid)
            print("BIND {} -> media-server: {}".format(uid, msg))
            await media_server.web_socket.send(msg)
        else:
            raise SignalError("Registering of clients only possible after media"
                              " server registered")
        print("Registered client {} at {}".format(uid,
                                                  web_socket.remote_address))
    elif msg == "REGISTER MEDIA":
        if media_server is None:
            media_server = new_peer
        else:
            await web_socket.close(code=1002, reason="already connected to a"
                                                     " gstreamer media source")
            raise Exception("Multiple media server connections detected {!r}".format(
                new_peer.remote_address))
        print("Registered media server at {}".format(
            web_socket.remote_address))
        await web_socket.send("REGISTERED")
    else:
        await web_socket.close(code=1002, reason='invalid protocol')
        raise Exception("Invalid hello from {}".format(
            new_peer.remote_address))

    return uid, new_peer


async def handler(web_socket, path):
    """
    All incoming messages are handled here. @path is unused.
    """
    print("Connected to {!r}".format(web_socket.remote_address))
    # TODO: is this a bug in waiting? can't work out how this doesn't swallow
    #       all the messages
    uid, new_peer = await register_peer(web_socket)
    try:
        await connection_handler(uid, new_peer, is_server=(uid is None))
    except websockets.ConnectionClosed:
        print("Connection to peer {!r} closed, exiting handler".format(
            web_socket.remote_address))
    finally:
        await remove_peer(uid)


print("Listening on http://{}:{}".format(*ADDR_PORT))
# Websocket server
web_socket_d = websockets.serve(handler, *ADDR_PORT, ssl=None, max_queue=16)

logger = logging.getLogger('webrtc.server')

logger.setLevel(logging.ERROR)
logger.addHandler(logging.StreamHandler())

asyncio.get_event_loop().run_until_complete(web_socket_d)
asyncio.get_event_loop().run_forever()
