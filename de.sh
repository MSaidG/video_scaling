#!/bin/bash

${CC} main.c -g -o main_test_dp_hdmi $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0 libdrm egl glesv2) -lm -ldl -Wall

scp main_test_dp_hdmi petalinux@192.168.10.2:/home/petalinux
