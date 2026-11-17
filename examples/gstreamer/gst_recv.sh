#!/bin/sh
# gst_recv.sh - UDP 로 받은 JPEG/RTP 영상을 푼다(GStreamer udpsrc). 수신 품질은 rtp_stats.py 로 잰다.
# 사용: ./gst_recv.sh [포트=5000] [display]
#   display 를 주면 화면에 보여 준다(데스크톱 환경 필요). 없으면 화면 없이 받기만 한다.
set -e
PORT=${1:-5000}
if [ "$2" = "display" ]; then SINK="autovideosink sync=false"; else SINK="fakesink sync=false"; fi
# SINK holds an element and its property; word splitting is intended.
# shellcheck disable=SC2086
exec gst-launch-1.0 -e \
  udpsrc port="$PORT" caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=JPEG,payload=26" ! \
  rtpjitterbuffer latency=50 ! rtpjpegdepay ! jpegdec ! \
  $SINK
