#!/bin/sh
# gst_send.sh - 시험 영상을 JPEG/RTP 로 만들어 UDP 로 보낸다(GStreamer udpsink).
# 사용: ./gst_send.sh <수신 IP> [포트=5000] [폭=1280] [높이=720] [fps=30] [JPEG 품질=85] [패턴=smpte]
# 대역은 해상도·fps·품질·패턴으로 정해진다. 패턴 smpte(색 막대) = 약 16 Mbps, snow(잡음) = 큰 대역(1280x720 30 fps 기준).
set -e
RX=${1:?Specify the receiver IP address}
PORT=${2:-5000}
W=${3:-1280}
H=${4:-720}
FPS=${5:-30}
Q=${6:-85}
PAT=${7:-smpte}
exec gst-launch-1.0 -e \
  videotestsrc is-live=true pattern="$PAT" ! \
  "video/x-raw,width=$W,height=$H,framerate=$FPS/1" ! \
  videoconvert ! video/x-raw,format=I420 ! jpegenc quality="$Q" ! \
  rtpjpegpay mtu=1400 ! \
  udpsink host="$RX" port="$PORT" sync=false
