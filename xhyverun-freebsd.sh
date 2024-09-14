#!/bin/sh

IMG="<path of disk image for FreeBSD>"

xhyve \
    -A \
    -m 2G \
    -c 2 \
    -s 0:0,hostbridge \
    -s 2:0,virtio-net \
    -s 4:0,virtio-blk,$IMG \
    -s 31,lpc \
    -l com1,stdio \
    -f fbsd,test/userboot.so""
