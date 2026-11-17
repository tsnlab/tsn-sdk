#!/usr/bin/env python3
"""rx_window_check.py - 수신 프레임의 하드웨어 시각으로 TAS 시간 창 안 도착을 센다.

수신 보드(TSN HAT 또는 TSN NIC)에서 root 로 실행한다. 표준 라이브러리만 쓴다.
보드 드라이버는 수신 프레임에 gPTP 시각을 붙인다. 송신 보드의 TAS cycle 은 기준 시각(base, gPTP 정수 초)에서 시작한다.
도착 위치 = (수신 시각 - base) mod cycle. 두 보드 모두 gPTP 가 동기되어 있어야 한다.
  - cycle 이 1 s 를 나누어 떨어지면(예 1000 us, 500 us, 250 us) 정수 초 base 를 빼도 위치가 같으므로 수신 시각 mod cycle 로 센다.
  - 나누어 떨어지지 않는 cycle(예 300 us)은 실행하지 않는다. 그때의 기준 시각을 수신 쪽에서 맞추는 방법을 확인하지 않았다.

사용:
  sudo python3 rx_window_check.py --iface eth1 --cycle 1000 --open 500

판정 범위:
  - 센 것 = 하드웨어 수신 시각이 있는 프레임(유효 표본) 중 위치가 [0, 열림 시간 + 허용 지연) 안에 있는 수.
    허용 지연 기본 20 us(마지막 프레임 송출 약 12.3 us + 보드·스위치 지연 약 3 us + 여유).
  - 세지 않은 것: 하드웨어 시각이 없는 프레임(no_timestamp), 이 도구의 캡처가 놓친 프레임(capture_drops),
    망에서 잃어 수신하지 못한 프레임. 결과에 앞의 두 수와 관측 시간을 함께 적는다.
  - 이 값은 수신 위치다. 송신 출발 시각을 직접 잰 것이 아니다. 한 cycle 이상 늦은 프레임도 위치가 창 안으로 돌아올 수 있다.
  - 같은 망의 다른 트래픽이 출구에서 끼어들면 일부 프레임이 늦게 도착할 수 있다.
실행이 끝나면 인터페이스의 하드웨어 시각 설정을 원래대로 되돌린다.
"""
import argparse
import ctypes
import fcntl
import json
import socket
import struct
import time

SIOCSHWTSTAMP, SIOCGHWTSTAMP = 0x89B0, 0x89B1
SO_TIMESTAMPING = 37
FLAGS = 4 | 64            # RX_HARDWARE | RAW_HARDWARE
HWTSTAMP_FILTER_ALL = 1


def hwts(sock, iface, rx_filter, req=SIOCSHWTSTAMP, tx_type=0):
    cfg = ctypes.create_string_buffer(struct.pack("iii", 0, tx_type, rx_filter))
    ifr = struct.pack("16sP", iface.encode(), ctypes.addressof(cfg))
    fcntl.ioctl(sock.fileno(), req, ifr)
    return struct.unpack("iii", cfg.raw[:12])


def capture(iface, port, maxn, timeout_s):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    s.bind((iface, 0))
    s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING, FLAGS)
    try:   # root 는 시스템 상한(rmem_max)을 넘겨 잡는다. 상한에 잘리면 묶음 도착 때 캡처가 프레임을 놓친다
        s.setsockopt(socket.SOL_SOCKET, 33, 64 << 20)   # SO_RCVBUFFORCE
    except OSError:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 << 20)
    prev = hwts(s, iface, 0, SIOCGHWTSTAMP)
    ts_list, no_ts = [], 0
    drops = None
    try:
        hwts(s, iface, HWTSTAMP_FILTER_ALL, tx_type=prev[1])
        s.settimeout(0.5)
        t_end = time.monotonic() + timeout_s
        print(f"수신 대기: {iface} UDP {port}, 최대 {maxn} 프레임 / {timeout_s:.0f} s", flush=True)
        while len(ts_list) < maxn and time.monotonic() < t_end:
            try:
                data, anc, _, _ = s.recvmsg(2048, 256)
            except socket.timeout:
                if ts_list:
                    break
                continue
            if len(data) < 42 or data[12:14] != b"\x08\x00" or data[23] != 17:
                continue
            ihl = (data[14] & 0x0F) * 4
            if struct.unpack_from("!H", data, 14 + ihl + 2)[0] != port:
                continue
            ts = None
            for lvl, typ, val in anc:
                if lvl == socket.SOL_SOCKET and typ == SO_TIMESTAMPING and len(val) >= 48:
                    sec, nsec = struct.unpack_from("qq", val, 32)
                    ts = sec * 1_000_000_000 + nsec
            if ts:
                ts_list.append(ts)
            else:
                no_ts += 1
    finally:
        try:
            hwts(s, iface, prev[2], tx_type=prev[1])
        except OSError:
            pass
        try:   # 이 도구의 캡처 소켓이 놓친 프레임 수(PACKET_STATISTICS tp_drops)
            drops = struct.unpack("II", s.getsockopt(263, 6, 8))[1]
        except OSError:
            drops = None
        s.close()
    return ts_list, no_ts, drops


def check_args(cycle_us, open_us, slack_us):
    """설정값 확인. 맞지 않으면 이유 문자열, 맞으면 None."""
    if cycle_us <= 0:
        return "--cycle 은 0 보다 커야 합니다."
    if not 0 < open_us < cycle_us:
        return "--open 은 0 보다 크고 --cycle 보다 작아야 합니다."
    if not 0 <= slack_us < cycle_us - open_us:
        return "--slack 은 0 이상이고 (cycle - open) 보다 작아야 합니다. 아니면 모든 위치가 창 안이 됩니다."
    if 1_000_000 % cycle_us:
        return "--cycle 은 1 s(1000000 us)를 나누어 떨어지는 값이어야 합니다(예 1000, 500, 250). 다른 값은 확인하지 않았습니다."

    return None


def analyze(ts_list, cycle_us, open_us, slack_us, base_sec=None):
    cyc = cycle_us * 1000
    base = (base_sec or 0) * 1_000_000_000
    rel = [t - base for t in ts_list]
    phases = [t % cyc for t in rel]
    lim = int((open_us + slack_us) * 1000)
    inside = sum(1 for p in phases if p < lim)
    # 주기마다 첫 도착 위치(창이 열린 뒤 몇 us 만에 첫 프레임이 왔나)
    first = {}
    for t in rel:
        k = t // cyc
        if k not in first:
            first[k] = t % cyc
    firsts = sorted(first.values())
    bins = [0] * 10
    for p in phases:
        bins[min(9, p * 10 // cyc)] += 1
    return {
        "hw_ts_samples": len(phases),
        "inside_window": inside,
        "inside_ratio": round(inside / len(phases), 5) if phases else None,
        "window_us": [0, open_us + slack_us],
        "first_arrival_us_median": round(firsts[len(firsts) // 2] / 1000, 2) if firsts else None,
        "max_phase_us": round(max(phases) / 1000, 2) if phases else None,
        "observed_s": round((max(ts_list) - min(ts_list)) / 1e9, 3) if ts_list else None,
        "histogram_10bins": bins,
    }


def main():
    p = argparse.ArgumentParser(description="수신 하드웨어 시각으로 TAS 창 확인")
    p.add_argument("--iface", default="eth1")
    p.add_argument("--port", type=int, default=5201, help="UDP 목적지 포트(iperf3 기본 5201)")
    p.add_argument("--cycle", type=int, required=True, help="송신 TAS cycle us")
    p.add_argument("--open", type=int, required=True, help="송신 큐 1 열림 시간 us(cycle 시작부터)")
    p.add_argument("--slack", type=float, default=20.0, help="허용 지연 us")
    p.add_argument("--frames", type=int, default=50000)
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--json", help="결과 JSON 파일")
    a = p.parse_args()
    err = check_args(a.cycle, a.open, a.slack)
    if err:
        raise SystemExit(err)
    ts_list, no_ts, drops = capture(a.iface, a.port, a.frames, a.timeout)
    r = analyze(ts_list, a.cycle, a.open, a.slack)
    r.update(no_timestamp=no_ts, capture_drops=drops, cycle_us=a.cycle, open_us=a.open, slack_us=a.slack)
    print(json.dumps(r, ensure_ascii=False))
    if r["hw_ts_samples"]:
        print(f"허용 도착 구간 [0, {a.open + a.slack:g} us) 안 {r['inside_window']}/{r['hw_ts_samples']} ({r['inside_ratio']*100:.2f} %, "
              f"하드웨어 시각이 있는 표본 기준). 시각 없음 {no_ts}, 캡처 손실 {drops}, 관측 {r['observed_s']} s. "
              f"주기별 첫 도착 중앙값 {r['first_arrival_us_median']} us, 가장 늦은 위치 {r['max_phase_us']} us")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(r, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
