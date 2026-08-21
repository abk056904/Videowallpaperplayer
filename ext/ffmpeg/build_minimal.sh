#!/bin/bash
# Minimal FFmpeg build for VideoWallpaper — only H.264/HEVC/VP9/AV1 + audio decoders.
# Run from MSYS2 MinGW64 shell.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/ffmpeg-7.1" && pwd)"
PREFIX="$(cd "$(dirname "$0")/minimal" && pwd)"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

cd "$SRC_DIR"
make distclean 2>/dev/null || true

./configure \
    --prefix="$PREFIX" \
    --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-network \
    --disable-autodetect \
    --enable-d3d11va \
    --disable-decoders \
    --enable-decoder=h264 --enable-decoder=hevc \
    --enable-decoder=vp9 --enable-decoder=av1 \
    --enable-decoder=pcm_s16le --enable-decoder=pcm_f32le \
    --enable-decoder=aac --enable-decoder=mp3 \
    --enable-decoder=opus --enable-decoder=pcm_alaw --enable-decoder=pcm_mulaw \
    --disable-demuxers \
    --enable-demuxer=matroska --enable-demuxer=mov --enable-demuxer=avi \
    --enable-demuxer=mpegts --enable-demuxer=ogg --enable-demuxer=mpegps \
    --enable-demuxer=flv --enable-demuxer=mp3 --enable-demuxer=aac \
    --enable-demuxer=wav --enable-demuxer=concat \
    --disable-parsers \
    --enable-parser=h264 --enable-parser=hevc --enable-parser=vp9 \
    --enable-parser=av1 --enable-parser=aac --enable-parser=opus \
    --disable-filters \
    --enable-filter=null --enable-filter=aresample --enable-filter=anull \
    --disable-protocols --enable-protocol=file

make -j"$(nproc)"
make install

# Remove unused libs (app only links avcodec/avformat/avutil/swscale/swresample)
rm -f "$PREFIX"/bin/avdevice-*.dll "$PREFIX"/bin/avfilter-*.dll
rm -f "$PREFIX"/lib/avdevice* "$PREFIX"/lib/avfilter*

echo "=== Build complete ==="
ls -lh "$PREFIX"/bin/*.dll
du -sh "$PREFIX"/bin/
