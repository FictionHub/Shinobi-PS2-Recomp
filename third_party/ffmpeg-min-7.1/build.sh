#!/bin/bash
# Minimal FFmpeg 7.1 for the Shinobi recomp: only what Kernel/Stubs/MPEG.cpp uses (the MPEG-2 video decoder, its
# parser, swscale for the colour conversion). Same versions as the prebuilt it replaces (avcodec-61, avutil-59,
# swscale-8), so the DLLs drop in without rebuilding the game. LGPL (no --enable-gpl).
set -e
cd "$(dirname "$0")/ffmpeg-7.1"
if [ ! -f config.h ]; then
./configure --toolchain=msvc --arch=x86_64 --target-os=win64 \
  --prefix="$(cd .. && pwd)/out" \
  --enable-shared --disable-static --disable-debug --disable-doc --disable-programs \
  --disable-everything --disable-autodetect --disable-network \
  --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample --disable-postproc \
  --enable-avcodec --enable-avutil --enable-swscale \
  --enable-decoder=mpeg2video --enable-parser=mpegvideo \
  --enable-x86asm
fi
make -j12
make install
