#!/bin/sh

make uninstall && make clean && make -j 8 && make install && rmmod module/limic.ko ; insmod module/limic.ko
