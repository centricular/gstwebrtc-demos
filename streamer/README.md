Overview
========

This streaming example allows you to connect multiple clients to a single
streaming endpoint (or `mountpoint`).

Building
========

To build, just run the [Makefile](gst/Makefile).

Usage
=====

To use, serve the files in the `/js` directory (with `python3 -m http.server`
for example), and run the standalone signalling server:
```
python3 [./streaming-signalling-server.py](../signalling/streaming-signalling-server.py)
```
Then run the compiled binary in the `gst/` directory:
```
./streaming-app
```
