#!/bin/bash

${CC} main.c -o anim_gles_app $(pkg-config --cflags --libs libdrm egl glesv2 ncurses) -lm -ldl -Wall

scp anim_gles_app root@192.168.10.2:/run/media/ROOTFS-mmcblk1p2/home/videos
