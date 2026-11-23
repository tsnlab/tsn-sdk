#!/usr/bin/env python3
"""sensor_tx.py - 센서 데이터를 읽어 TSN 망으로 UDP 전송한다(공통 송신 틀).

송신 보드에서 root 로 실행한다(gPTP 시각을 PCIe 로 읽기 때문). 표준 라이브러리만 쓴다.
데이터 한 묶음(scan/frame)을 1,400 B 이하 조각으로 나누고, 각 조각 앞에 헤더를 붙인다.
헤더의 송신 시각은 보드 gPTP 시각이다. 수신 쪽은 이 값과 수신 하드웨어 시각으로 한 방향 지연을 잰다.

입력원(--source):
  sim:lidar[:<점 수>]          모의 라이다(기본 360 점/scan, 점마다 각도 u16·거리 u16·세기 u8)
  serial:<장치>[:<baud>]       USB 시리얼 센서. 한 줄(LF)을 한 묶음으로 보낸다
  cmd:<명령>                   외부 프로그램의 표준 출력. 한 줄을 한 묶음으로 보낸다(제조사 SDK 출력 등)
  file:<경로>                  파일을 --chunk 바이트씩 묶음으로 보낸다
실제 라이다를 붙이려면 아래 read_lidar() 자리에 장치 SDK 호출을 넣는다.
serial·file·cmd 입력은 보드에서 충분히 확인하지 않았다(sim:lidar 와 cmd: 출력 한 줄만 확인).
cmd: 의 명령은 셸로 실행된다. sudo 로 실행하면 root 권한으로 돈다. 믿을 수 있는 명령만 넣는다.
serial·cmd 입력은 0.2 s 마다 --seconds 를 확인한다(입력이 멈추거나 줄 끝이 오지 않아도 제시간에 끝난다).
cmd: 명령이 끝까지 돌지 않으면 끝날 때 그 명령의 프로세스 그룹을 끝낸다.

사용:
  sudo python3 sensor_tx.py --dest 192.0.2.20 --source sim:lidar --rate 10 --seconds 30
  sudo python3 sensor_tx.py --dest 192.0.2.20 --source serial:/dev/ttyACM0:115200
"""
import argparse
import math
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True   # sudo 로 실행해도 common/__pycache__ 를 root 소유로 남기지 않는다
HERE = os.path.dirname(os.path.abspath(__file__))
for d in (os.path.join(HERE, "..", "common"), os.path.join(HERE, "..", "..", "tools", "tsnctl")):
    if os.path.exists(os.path.join(d, "tsnctl.py")):
        sys.path.insert(0, d)
        break
import tsnctl  # noqa: E402

# 헤더(40 B, 네트워크 바이트 순서): magic 'TSNS', 버전(3), 스트림 ID, 예약(0), 묶음 번호, 조각 번호/개수, 패킷 순번, 송신 gPTP ns, 묶음 길이,
#   실행 식별값(64 비트). 실행 식별값 = 송신기를 시작할 때 os.urandom 으로 고르는 값. 수신기는 송신 주소·스트림 ID·실행 식별값이 다르면 섞지 않는다.
# 이전 판(버전 1·2) 수신기는 이 헤더를 받지 않는다(송수신기를 같은 판으로 맞춘다).
HDR = struct.Struct("!4sBBHIHHIQI")
MAGIC = b"TSNS"
RUN3 = struct.Struct("!Q")
MAX_CHUNK = 1400
HDR_VERSION = 3


def read_lidar(n_points, scan_no):
    """모의 라이다 한 scan. 실제 장치는 여기서 SDK 로 한 scan 을 읽어 bytes 로 돌려준다."""
    out = bytearray()
    for i in range(n_points):
        angle = int(i * 36000 / n_points)                          # 0.01 도
        dist = int(2000 + 1000 * math.sin((i + scan_no) / 20))     # mm
        out += struct.pack("!HHB", angle, dist & 0xFFFF, 200)
    return bytes(out)


def fd_lines(fd, poll_s=0.2):
    """fd 에서 바이트를 직접 읽어 LF 로 나눈 줄을 내준다. 0.2 s 마다(그리고 읽을 때마다) None 을 내줘
    호출한 쪽이 --seconds 를 확인하게 한다. 줄 끝(LF)이 오지 않아도 멈추지 않는다. EOF 면 남은 조각을 내주고 끝낸다."""
    buf = b""
    while True:
        if select.select([fd], [], [], poll_s)[0]:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                chunk = None
            if chunk == b"":                       # EOF(장치 닫힘·명령 끝)
                if buf:
                    yield buf
                return
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    yield line + b"\n"
        yield None


def source_iter(spec, chunk):
    kind, _, rest = spec.partition(":")
    if kind == "sim":
        n = int(rest.split(":")[1]) if rest.count(":") else 360
        k = 0
        while True:
            yield read_lidar(n, k)
            k += 1
    elif kind == "serial":
        import termios
        parts = rest.split(":")
        dev, baud = parts[0], int(parts[1]) if len(parts) > 1 else 115200
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        speed = getattr(termios, f"B{baud}")
        a[4] = a[5] = speed
        termios.tcsetattr(fd, termios.TCSANOW, a)
        try:
            yield from fd_lines(fd)
        finally:
            os.close(fd)
    elif kind == "cmd":
        # 버퍼 없이 파이프를 직접 읽는다(숨은 버퍼에 줄이 남아 늦게 나가거나 버려지지 않게). 끝낼 때는 프로세스 그룹째 끝낸다
        p = subprocess.Popen(rest, shell=True, stdout=subprocess.PIPE, bufsize=0, start_new_session=True)
        try:
            yield from fd_lines(p.stdout.fileno())
        finally:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    p.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
    elif kind == "file":
        with open(rest, "rb") as f:
            while True:
                b = f.read(chunk)
                if not b:
                    return
                yield b
    else:
        sys.exit(f"Unknown input source: {spec}")


def main():
    p = argparse.ArgumentParser(description="Send sensor data over TSN")
    p.add_argument("--dest", required=True, help="receiver IP address")
    p.add_argument("--port", type=int, default=6000)
    p.add_argument("--source", default="sim:lidar")
    p.add_argument("--rate", type=float, default=10.0, help="frame rate for sim and file inputs (Hz); serial and cmd follow the source rate")
    p.add_argument("--seconds", type=float, default=0, help="duration in seconds (0: run until the source ends or interrupted)")
    p.add_argument("--stream-id", type=int, default=1)
    p.add_argument("--chunk", type=int, default=4096, help="frame size in bytes for file input")
    p.add_argument("--no-gptp", action="store_true", help="send a zero timestamp instead of gPTP time (no root or PCIe required)")
    p.add_argument("--dev", help="board device (/dev/xdmaN_user). With multiple boards, use --dev or --iface")
    p.add_argument("--iface", help="select the board by Linux interface name (e.g. eth1 or ens4d1)")
    a = p.parse_args()
    if not 0 <= a.stream_id <= 255:
        sys.exit("--stream-id must be between 0 and 255.")
    run_id = int.from_bytes(os.urandom(8), "big")
    print(f"Sending: stream={a.stream_id}, run={run_id:016x}", flush=True)

    bar = None if a.no_gptp else tsnctl.Bar(a.dev, iface=a.iface)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
    paced = a.source.startswith(("sim", "file"))
    period = 1.0 / a.rate if paced and a.rate > 0 else 0
    t0 = time.monotonic()
    next_t = t0
    pkt_seq, frame_no, sent_bytes = 0, 0, 0
    for data in source_iter(a.source, a.chunk):
        if a.seconds and time.monotonic() - t0 >= a.seconds:
            break
        if data is None:
            continue
        if period:
            now = time.monotonic()
            if next_t > now:
                time.sleep(next_t - now)
            next_t += period
        parts = [data[i:i + MAX_CHUNK] for i in range(0, len(data), MAX_CHUNK)] or [b""]
        for idx, part in enumerate(parts):
            tx_ns = tsnctl.gptp_now_ns(bar) if bar else 0
            hdr = HDR.pack(MAGIC, HDR_VERSION, a.stream_id, 0, frame_no & 0xFFFFFFFF, idx, len(parts),
                           pkt_seq & 0xFFFFFFFF, tx_ns, len(data)) + RUN3.pack(run_id)
            s.sendto(hdr + part, (a.dest, a.port))
            pkt_seq += 1
            sent_bytes += len(part)
        frame_no += 1
        if frame_no % max(1, int(a.rate or 10)) == 0:
            print(f"{time.strftime('%H:%M:%S')} frames={frame_no} packets={pkt_seq} bytes={sent_bytes}", flush=True)
    print(f"Done: frames={frame_no}, packets={pkt_seq}, bytes={sent_bytes}")


if __name__ == "__main__":
    main()
