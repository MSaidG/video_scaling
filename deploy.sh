#!/bin/bash

${CC} main.c -o decode_app_4 $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0 libdrm egl glesv2) -lm -ldl -Wall

scp decode_app_4 root@192.168.10.2:/run/media/ROOTFS-mmcblk1p2/home/videos
