#!/bin/bash

${CC} main.c -o gles_app $(pkg-config --cflags --libs libdrm egl glesv2 ncurses) -lm -ldl -Wall

scp gles_app root@192.168.10.2:/run/media/ROOTFS-mmcblk1p2/home/videos
