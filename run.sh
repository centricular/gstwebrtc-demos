#!/bin/bash
sudo modprobe v4l2loopback exclusive_caps=1 video_nr=10,11,12,13 card_label="LEFT","CENTER","RIGHT","REAR"
gst-launch-1.0 -v v4l2src device=/dev/video0 ! v4l2sink device=/dev/video10 &
gst-launch-1.0 -v v4l2src device=/dev/video1 ! v4l2sink device=/dev/video11 & 
gst-launch-1.0 -v v4l2src device=/dev/video2 ! v4l2sink device=/dev/video12 &
gst-launch-1.0 -v v4l2src device=/dev/video3 ! v4l2sink device=/dev/video13 &
#docker-compose up
