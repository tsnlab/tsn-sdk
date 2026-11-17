#!/usr/bin/env python3
"""rtp_stats.py - 수신한 RTP(UDP) 스트림의 손실·순서·흔들림·대역을 잰다.

수신 보드에서 root 로 실행한다. GStreamer udpsrc 와 같은 포트를 함께 볼 수 있다(패킷을 가로채지 않는다).
보드 드라이버가 하드웨어 수신 시각을 주면 그것을 쓰고, 없으면 소프트웨어 시각을 쓴다.

사용:
  sudo python3 rtp_stats.py --iface eth1 --port 5000 --seconds 30
출력: 1 초마다 한 줄, 끝에 요약 JSON 한 줄.
  - RTP 송신원(SSRC)마다 따로 센다(streams). 송신 GStreamer 를 다시 시작하면 SSRC 가 바뀌어 다른 스트림이 된다.
    요약 맨 위의 pkts·lost·reordered·duplicates 는 모든 스트림의 합, jitter_us·max_gap_us 는 패킷이 가장 많은 스트림(main_ssrc) 값이다.
  - lost: 스트림마다 받은 첫 순번 ~ 마지막 순번 안에서 끝까지 오지 않은 순번 수(16 비트 순번 넘김 처리).
    늦게 온 패킷은 reordered, 같은 순번을 두 번 받으면 duplicates 로 센다(손실이 아님).
    1 초 줄의 gap 은 그 초에 본 순번 빈 칸이다. 뒤에 늦게 도착하면 요약 lost 에서는 빠진다.
  - loss_exact: 순번 모호성을 탐지하지 못했으면 true, 탐지했으면 false. true 가 손실 수가 정확하다는 뜻은 아니다.
    한 번에 16,384 이상 건너뛰거나 되돌아가면 false 로 하고, 그때 lost 는 참고값이다.
    긴 단절이나 한 바퀴(65,536 패킷) 넘는 누락은 검출하지 못한다(예: 0 -> 65,536 을 통째로 잃으면 수신 순번이 0 -> 0 이라 lost 0, true).
    긴 단절이 의심되면 송신 쪽 총 패킷 수와 대조한다.
  - jitter_us: RFC 3550 도착 간격 흔들림(RTP 시계 90 kHz 기준). JPEG 한 장의 패킷들은 RTP 시각이 같으므로
    망 지연 변동보다 송신 묶음 간격의 영향이 크다. 같은 스트림, 같은 시각 출처(하드웨어 또는 소프트웨어) 안에서만 계산한다.
  - max_gap_us: 같은 스트림에서 연속 두 패킷 도착 간격의 최댓값(영상 한 장 간격 포함)
  - L2_mbps: 첫 패킷부터 마지막 패킷까지 시간으로 나눈 대역(active_seconds). 송신을 늦게 시작해도 낮아지지 않는다
  - capture_drops: 이 도구가 놓친 패킷 수. 0 이 아니면 lost 의 일부는 망 손실이 아니라 측정 손실일 수 있다
  - ts_source_switches: 하드웨어·소프트웨어 시각 출처가 바뀐 횟수. 바뀌면 jitter·max_gap 계산을 이어 붙이지 않는다

L1 지연 수집(--l1-ext-id 를 줄 때만, 송신 쪽 rtp_l1_tag.py 와 함께. 판정은 l1_finalize.py 가 한다):
  - 이름: 송신 앱 -> 수신 MAC 지연(송신 SW·커널·DMA·큐잉 포함). 순수 MAC 사이 네트워크 지연·화면 출력 지연이 아니다.
  - 값 = 수신 보드 하드웨어 수신 시각 - L1 원소의 송신 gPTP 시각. 소켓 버퍼·udpsrc·rtpjitterbuffer 대기는 들어가지 않는다.
  - 패킷 검사(하나라도 어긋나면 그 패킷은 L1 표본이 아니고 이유별로 센다. 손실·jitter 통계는 기존 그대로):
    캡처 잘림(MSG_TRUNC)·IPv4 길이 불일치·조각(fragment, 지원 안 함)·UDP 길이 불일치,
    RTP 버전·CSRC 길이·padding(P 비트) 길이, 확장이 padding 을 뺀 끝 안에 있음, one-byte 형식(0xBEDE),
    모든 원소 경계, ID 15 있음(거절), 그 ID 가 둘 이상(거절), 그 ID 원소가 L1 형식('L1', 버전 1, 16 B)이 아님(foreign_elem),
    L1 플래그가 정확히 0x01(bit0 = 보드 gPTP 출처, 나머지 예약 0)이 아님(bad_flags).
  - run(원소 안 32 비트)별·SSRC 별로 따로 모은다. 중복 판별도 (run, SSRC) 별 순번 상태로 따로 한다(다른 run·확장 없는 패킷의
    같은 순번이 이번 run 표본을 지우지 않음). 일반 손실·순서 통계는 기존처럼 SSRC 별이다.
    원소 안 송신 시각의 최소·최대(first_tx_ns·last_tx_ns)도 남긴다(l1_finalize.py 가 송신 태그 구간과 대조).
    어느 run 이 이번 송신 실행인지는 이 도구가 정하지 않는다(l1_finalize.py 가 송신 기록과 대조).
  - 하드웨어 수신 시각이 없으면 소프트웨어 시각으로 대신 계산하지 않는다(missing_hw_ts).
  - 시간축 근거로 수신 보드 상태 블록(형식·갱신 수·gPTP·남는 offset·count)을 시작·끝에 읽고(읽기만) 드라이버 rx_ts_gptp 값을 기록한다.
    상태를 읽은 장치의 PCI 와 캡처 --iface 의 PCI 를 시작·끝에 함께 남긴다. --dev 가 --iface 와 다른 보드면 시작하지 않는다.
    시작·끝 기록은 중간의 동기 이탈을 배제하지 않는다.
  - 회차: --acq-id 필수(같은 회차 송신 rtp_l1_tag.py 와 같은 값), --json 필수. 그 경로나 예약('<경로>.lock')이 있으면 시작하지 않는다
    (l1_out.py 배타 예약·게시, 먼저 난 결과를 덮지 않음).
    소켓 준비·수집 도중·상태 읽기 실패는 미완 기록(l1.complete false, l1.error)을 남기고 종료 코드 3.
    수집 중 Ctrl+C(KeyboardInterrupt)는 수집을 다시 잇지 않고, 그때까지의 계수와 stage·error·l1.interrupted=true·complete=false 를
    남긴 뒤 종료 코드 130 으로 끝난다(성공으로 처리하지 않음). SIGKILL·전원 상실 때의 기록은 보장하지 않는다.
    정리(하드웨어 시각 복원·PACKET_STATISTICS·close) 실패는 OSError 도 정리 오류다. PACKET_STATISTICS 미지원(ENOPROTOOPT·EOPNOTSUPP)만
    capture_drops_status unsupported 로 따로 적는다. 정리·게시 중 Ctrl+C 의 경계와 종료 코드는 defer_sigint 와 README 를 본다.
IPv4 머리 검사(모든 모드): EtherType IPv4 인 프레임은 캡처 길이 >= 20 B, 버전 4, IHL >= 5, 캡처 길이 >= IHL, UDP 머리 8 B 를 확인한 뒤에만
  읽는다. 어긋나면 그 프레임만 이유별로 세고(header_rejects, 0 이 아닐 때만 요약에 나옴) 다음 프레임을 계속 받는다.
"""
import argparse
import json
import socket
import struct
import time

SIOCSHWTSTAMP, SIOCGHWTSTAMP = 0x89B0, 0x89B1
SO_TIMESTAMPING = 37
FLAGS = 4 | 64 | 8 | 16    # RX_HARDWARE | RAW_HARDWARE | RX_SOFTWARE | SOFTWARE
RTP_CLOCK = 90000
ONE_BYTE_PROFILE = 0xBEDE
L1_LEN, L1_MAGIC, L1_VERSION, L1_FLAGS = 16, b"L1", 1, 0x01   # 플래그: bit0 = 보드 gPTP 출처(필수), 나머지 예약 0
MSG_TRUNC = 0x20
AMBIGUOUS_JUMP = 0x4000    # 이만큼 건너뛰거나 되돌아가면 손실 수를 확정하지 않는다


def set_hwts(sock, iface, rx_filter, tx_type=0, req=SIOCSHWTSTAMP):
    import ctypes
    import fcntl
    cfg = ctypes.create_string_buffer(struct.pack("iii", 0, tx_type, rx_filter))
    fcntl.ioctl(sock.fileno(), req, struct.pack("16sP", iface.encode(), ctypes.addressof(cfg)))
    return struct.unpack("iii", cfg.raw[:12])


def parse_l1(rtp, ext_id):
    """RTP 패킷(UDP payload) 전체 구조를 검사하고 ext_id 의 L1 원소를 읽는다. (상태, run, 송신 ns).
    상태: ok, no_ext, other_profile, no_id, foreign_elem, bad_flags, bad_rtp, bad_padding, bad_ext, id15, dup_id."""
    n = len(rtp)
    if n < 12 or rtp[0] >> 6 != 2:
        return "bad_rtp", None, None
    h = 12 + 4 * (rtp[0] & 0x0F)
    if h > n:
        return "bad_rtp", None, None
    end = n
    if rtp[0] & 0x20:
        pad = rtp[-1]
        if pad == 0 or pad > n - h:
            return "bad_padding", None, None
        end = n - pad
    if not rtp[0] & 0x10:
        return "no_ext", None, None
    if h + 4 > end:
        return "bad_ext", None, None
    profile, words = struct.unpack_from("!HH", rtp, h)
    ext_end = h + 4 + 4 * words
    if ext_end > end:
        return "bad_ext", None, None
    if profile != ONE_BYTE_PROFILE:
        return "other_profile", None, None
    found = None
    i = h + 4
    while i < ext_end:
        b = rtp[i]
        if b == 0:
            i += 1
            continue
        eid, ln = b >> 4, (b & 0x0F) + 1
        if eid == 15:
            return "id15", None, None
        if i + 1 + ln > ext_end:
            return "bad_ext", None, None
        if eid == ext_id:
            if found is not None:
                return "dup_id", None, None
            found = (i + 1, ln)
        i += 1 + ln
    if found is None:
        return "no_id", None, None
    o, ln = found
    if ln != L1_LEN or rtp[o:o + 2] != L1_MAGIC or rtp[o + 2] != L1_VERSION:
        return "foreign_elem", None, None
    if rtp[o + 3] != L1_FLAGS:
        return "bad_flags", None, None
    run, tx_ns = struct.unpack_from("!IQ", rtp, o + 4)
    return "ok", f"{run:08x}", tx_ns


def read_rx_param(name):
    try:
        with open(f"/sys/module/xdma/parameters/{name}") as f:
            return f.read().strip()
    except OSError:
        return None


def _tsnctl():
    import os
    import sys
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (os.path.join(here, "..", "common"), os.path.join(here, "..", "..", "tools", "tsnctl")):
        if os.path.exists(os.path.join(d, "tsnctl.py")):
            sys.path.insert(0, d)
            break
    sys.dont_write_bytecode = True
    import tsnctl
    return tsnctl


def read_status(dev, iface):
    """수신 보드 상태 블록(읽기만) + 상태를 읽은 장치 PCI(pci)와 캡처 iface 의 PCI(iface_pci). 장치·도구가 없으면 None."""
    try:
        tsnctl = _tsnctl()
        b = tsnctl.Bar(dev, iface=iface)
        try:
            ipci = tsnctl.find_dev(None, iface)[1]
        except (SystemExit, Exception):     # noqa: BLE001
            ipci = None
        if not tsnctl.has_mailbox(b):
            return None
        st = tsnctl.status_block(b)
        return {k: st.get(k) for k in ("format", "updates", "gptp", "resid", "fw_id")} | {"pci": b.bdf, "iface": iface, "iface_pci": ipci}
    except (SystemExit, Exception):     # noqa: BLE001 - 판정 못 함으로 남긴다
        return None


def new_l1run():
    return {"pkts": 0, "samples": [], "missing_hw_ts": 0, "abnormal": 0, "duplicates_skipped": 0, "first_rx_ns": None, "last_rx_ns": None,
            "first_tx_ns": None, "last_tx_ns": None,
            "seq": {"first": None, "max": None, "missing": set(), "reordered": 0, "duplicates": 0, "exact": True}}


def l1run_summary(r):
    v = sorted(r["samples"])

    def q(x):
        return round(v[min(len(v) - 1, int(x * len(v)))] / 1000, 2) if v else None
    return {"pkts": r["pkts"], "samples": len(v), "missing_hw_ts": r["missing_hw_ts"], "abnormal": r["abnormal"],
            "duplicates_skipped": r["duplicates_skipped"], "first_rx_ns": r["first_rx_ns"], "last_rx_ns": r["last_rx_ns"],
            "first_tx_ns": r["first_tx_ns"], "last_tx_ns": r["last_tx_ns"], "seq_exact": r["seq"]["exact"],
            "min_us": round(v[0] / 1000, 2) if v else None, "median_us": q(0.5), "p99_us": q(0.99),
            "max_us": round(v[-1] / 1000, 2) if v else None}


def new_stream():
    return {"pkts": 0, "bytes_l2": 0, "first": None, "max": None, "missing": set(), "reordered": 0, "duplicates": 0,
            "exact": True, "jitter": 0.0, "last_transit": None, "last_arr": None, "max_gap": 0, "last_src": None,
            "first_mono": None, "last_mono": None}


def track_seq(st, seq):
    """한 스트림의 순번 상태를 갱신한다. 반환: 'new'(새 순번), 'gap'(빈 칸 뒤 새 순번, 빈 칸 수), 'reorder', 'dup'."""
    if st["max"] is None:
        st["first"] = st["max"] = seq
        return "new", 0
    # 16 비트 순번을 넘김까지 이어 붙인 순번(ext): 지금까지 가장 큰 ext 에 가장 가까운 값
    ext = st["max"] + ((seq - st["max"] + 0x8000) & 0xFFFF) - 0x8000
    if ext > st["max"]:
        gap = ext - st["max"] - 1
        if gap >= AMBIGUOUS_JUMP:
            st["exact"] = False
        if gap:
            st["missing"].update(range(st["max"] + 1, ext))
        st["max"] = ext
        return ("gap", gap) if gap else ("new", 0)
    if ext in st["missing"]:                  # 늦게 온 패킷: 손실 아님
        st["missing"].discard(ext)
        st["reordered"] += 1
        return "reorder", 0
    if ext < st["first"]:                     # 첫 패킷보다 앞 순번이 늦게 옴
        if st["first"] - ext >= AMBIGUOUS_JUMP:
            st["exact"] = False
        st["missing"].update(range(ext + 1, st["first"]))
        st["first"] = ext
        st["reordered"] += 1
        return "reorder", 0
    if st["max"] - ext >= AMBIGUOUS_JUMP:     # 반 바퀴 가까이 되돌아감: 중복인지 새 구간인지 순번만으로 모름
        st["exact"] = False
    st["duplicates"] += 1
    return "dup", 0


def stream_summary(st):
    span = st["max"] - st["first"] + 1 if st["max"] is not None else 0
    active = (st["last_mono"] - st["first_mono"]) if st["first_mono"] is not None and st["last_mono"] > st["first_mono"] else None
    return {"pkts": st["pkts"], "seq_first": st["first"], "seq_last_ext": st["max"], "lost": len(st["missing"]),
            "reordered": st["reordered"], "duplicates": st["duplicates"], "loss_exact": st["exact"],
            "loss_ratio": round(len(st["missing"]) / max(1, span), 6),
            "active_seconds": active and round(active, 3),
            "L2_mbps": round(st["bytes_l2"] * 8 / active / 1e6, 2) if active else None,
            "jitter_us": round(st["jitter"] / RTP_CLOCK * 1e6, 1), "max_gap_us": round(st["max_gap"] / 1000, 1)}


def ip_udp_offsets(data, off):
    """EtherType IPv4 프레임의 IPv4·UDP 머리 경계를 읽기 전에 확인한다. (ihl, UDP 시작) 또는 (None, 이유).
    이유가 None 이면 UDP 가 아닌 정상 프레임(조용히 넘김)."""
    if len(data) < off + 20:
        return None, "truncated_ip_header"
    if data[off] >> 4 != 4:
        return None, "bad_ip_version"
    ihl = (data[off] & 0x0F) * 4
    if ihl < 20:
        return None, "bad_ihl"
    if data[off + 9] != 17:
        return None, None
    if len(data) < off + ihl:
        return None, "truncated_ip_header"
    if len(data) < off + ihl + 8:
        return None, "truncated_udp_header"
    return (ihl, off + ihl), None


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def defer_sigint(box):
    """L1 회차 정리·게시 동안 Ctrl+C(SIGINT)를 예외로 올리지 않고 센다. 반환 = 이전 처리기(없으면 None).
    기록 경계(box['sealed'] = True, 기록 JSON 을 만들기 직전) 앞의 신호는 extra_interrupts, 뒤의 신호는 after_record 로 센다.
    주 스레드가 아니면 처리기를 바꿀 수 없으므로 None(그때는 Ctrl+C 가 그대로 예외가 될 수 있음, 보장 밖)."""
    import signal

    def h(signum, frame):
        box["after_record" if box.get("sealed") else "extra_interrupts"] += 1
    try:
        return signal.signal(signal.SIGINT, h)
    except (ValueError, OSError):
        return None


def restore_sigint(old):
    import signal
    if old is not None:
        try:
            signal.signal(signal.SIGINT, old)
        except (ValueError, OSError):
            pass


STATS_UNSUPPORTED = ("ENOPROTOOPT", "EOPNOTSUPP")   # PACKET_STATISTICS 를 지원하지 않는다는 뜻의 errno 만 '미지원'(이 플랫폼 errno 값으로 비교)


def l1_cleanup(s, prev, iface, errs):
    """L1 회차의 캡처 소켓 정리. 단계마다 예외(OSError 포함)·KeyboardInterrupt 를 errs 에 남기고 다음 단계로 간다.
    - 하드웨어 시각 설정 복원 실패(OSError 포함)는 정리 오류다(설정이 바뀐 채 남을 수 있음).
    - PACKET_STATISTICS 읽기: errno 가 STATS_UNSUPPORTED 이면 '미지원'(오류 아님, capture_drops None), 그 밖의 OSError 는 정리 오류.
    - close 실패는 정리 오류.
    반환 (capture_drops 또는 None, 첫 정리 오류가 취소인지, 통계 상태 'ok'·'unsupported:<errno>'·'error', 첫 정리 오류 종류 또는 None).
    취소 여부는 첫 정리 오류 하나로 정한다(원인 문자열 cleanup_errors[0] 과 같은 오류). 뒤 오류의 종류는 바꾸지 않는다.
    취소 여부 = isinstance(첫 오류, KeyboardInterrupt)(하위 클래스 포함). 표시용 종류(클래스 이름)와 따로 잡는다."""
    import errno as _errno
    drops, stats, first = None, "error", []

    def note(msg, e):
        if not first:
            first.append((type(e).__name__, isinstance(e, KeyboardInterrupt)))
        errs.append(msg)
    if prev is not None:
        try:
            set_hwts(s, iface, prev[2], tx_type=prev[1])
        except (Exception, KeyboardInterrupt) as e:   # noqa: BLE001 - OSError 도 복원 실패로 기록
            note(f"cleanup hwts restore: {type(e).__name__}: {e}", e)
    try:   # 이 도구의 캡처 소켓이 놓친 프레임 수(망 손실이 아니라 측정 손실)
        drops = struct.unpack("II", s.getsockopt(263, 6, 8))[1]   # SOL_PACKET, PACKET_STATISTICS
        stats = "ok"
    except OSError as e:
        unsup = {getattr(_errno, n) for n in STATS_UNSUPPORTED if hasattr(_errno, n)}
        if isinstance(e.errno, int) and e.errno in unsup:
            stats = f"unsupported:{_errno.errorcode.get(e.errno, e.errno)}"
        else:
            note(f"cleanup getsockopt: {type(e).__name__}(errno {e.errno}): {e}", e)
    except (Exception, KeyboardInterrupt) as e:       # noqa: BLE001
        note(f"cleanup getsockopt: {type(e).__name__}: {e}", e)
    try:
        s.close()
    except (Exception, KeyboardInterrupt) as e:       # noqa: BLE001
        note(f"cleanup close: {type(e).__name__}: {e}", e)
    f0, c0 = first[0] if first else (None, False)
    return drops, c0, stats, f0


def main():
    import os
    import re
    import sys
    p = argparse.ArgumentParser(description="RTP 수신 품질 측정")
    p.add_argument("--iface", default="eth1")
    p.add_argument("--port", type=int, default=5000)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--json", help="요약을 이 파일에 저장(--l1-ext-id 를 주면 필수, 이미 있으면 시작하지 않음)")
    p.add_argument("--l1-ext-id", type=int, help="L1 지연 수집: 송신 rtp_l1_tag.py 의 RTP 헤더 확장 ID(1~14). 주지 않으면 L1 을 모으지 않는다")
    p.add_argument("--l1-max-us", type=float, default=1e6, help="L1 이 음수이거나 이 값보다 크면 abnormal 로 센다(기본 1 s)")
    p.add_argument("--acq-id", help="L1 회차 이름(송신 rtp_l1_tag.py --acq-id 와 같게, --l1-ext-id 를 주면 필수)")
    p.add_argument("--dev", help="수신 보드 장치(상태 블록 읽기용, 보드가 여러 장일 때). --iface 와 같은 보드여야 한다")
    a = p.parse_args()
    l1_on = a.l1_ext_id is not None
    if l1_on:
        if not 1 <= a.l1_ext_id <= 14:
            raise SystemExit("--l1-ext-id 는 1~14 입니다.")
        if not a.acq_id or not re.match(r"^[A-Za-z0-9_.-]{1,64}$", a.acq_id):
            raise SystemExit("--l1-ext-id 를 쓰면 --acq-id(영문·숫자·_.-, 64 자 이하)가 필요합니다.")
        if not a.json:
            raise SystemExit("--l1-ext-id 를 쓰면 --json(이 회차 새 경로)이 필요합니다.")
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import l1_out
        try:
            l1_res = l1_out.Out(a.json, owner={"tool": "rtp_stats", "acq_id": a.acq_id})
        except l1_out.Busy as e:
            raise SystemExit(str(e))
    l1_excluded = {}
    l1_runs = {}
    hdr_rejects = {}
    l1_err = None
    l1_interrupted = False
    l1_stage = "setup"
    l1_wall_start = utc_now() if l1_on else None
    l1_status_start = l1_status_end = None
    l1_rx_ts_gptp = None
    l1_cleanup_errs = []
    l1_box = {"extra_interrupts": 0, "after_record": 0, "sealed": False}
    l1_old_sigint = None
    l1_c_intr = False
    l1_c_first = None       # 첫 정리 오류 종류
    l1_stats_status = None
    l1_finish_cause = False

    streams = {}
    tot = {"hw_ts": 0, "sw_ts": 0, "ts_source_switches": 0}
    capture_drops = None
    s = None
    prev = None
    try:
        if l1_on:
            l1_status_start = read_status(a.dev, a.iface)
            l1_rx_ts_gptp = read_rx_param("rx_ts_gptp")
            if l1_status_start is None:
                raise RuntimeError("수신 보드 상태를 읽지 못했습니다(장치·메일박스)")
            if l1_status_start.get("pci") is None or l1_status_start.get("pci") != l1_status_start.get("iface_pci"):
                raise RuntimeError(f"상태 장치 PCIe {l1_status_start.get('pci')} 와 --iface {a.iface} PCIe {l1_status_start.get('iface_pci')} 가 다릅니다")
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
        s.bind((a.iface, 0))
        s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING, FLAGS)
        try:   # root 는 시스템 상한(rmem_max)을 넘겨 잡는다. 상한에 잘리면 묶음 도착 때 캡처가 프레임을 놓친다
            s.setsockopt(socket.SOL_SOCKET, 33, 64 << 20)   # SO_RCVBUFFORCE
        except OSError:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 << 20)
        s.settimeout(0.2)
        try:
            prev = set_hwts(s, a.iface, 0, req=SIOCGHWTSTAMP)
            set_hwts(s, a.iface, 1, tx_type=prev[1])          # 하드웨어 수신 시각 켜기(지원할 때)
        except OSError:
            prev = None
        l1_stage = "capture"
        sec = {"pkts": 0, "bytes_l2": 0, "gap": 0}
        t_end = time.monotonic() + a.seconds
        t_tick = time.monotonic() + 1
        print(f"수신 대기: {a.iface} UDP {a.port}, {a.seconds:.0f} s" + (f", L1 회차 {a.acq_id}" if l1_on else ""), flush=True)
        while time.monotonic() < t_end:
            if time.monotonic() >= t_tick:
                main_st = max(streams.values(), key=lambda x: x["pkts"]) if streams else None
                jit = main_st["jitter"] / RTP_CLOCK * 1e6 if main_st else 0.0
                print(f"{time.strftime('%H:%M:%S')} pkts={sec['pkts']} L2_mbps={sec['bytes_l2'] * 8 / 1e6:.2f} "
                      f"gap={sec['gap']} streams={len(streams)} jitter_us={jit:.1f}", flush=True)
                sec = {"pkts": 0, "bytes_l2": 0, "gap": 0}
                t_tick += 1
            try:
                data, anc, rflags, _ = s.recvmsg(2048, 256)
            except socket.timeout:
                continue
            off = 14
            if data[12:14] == b"\x81\x00":
                off = 18
            if data[off - 2:off] != b"\x08\x00":
                continue
            lay, why = ip_udp_offsets(data, off)
            if lay is None:
                if why:
                    hdr_rejects[why] = hdr_rejects.get(why, 0) + 1
                continue
            ihl, u = lay
            if struct.unpack_from("!H", data, u + 2)[0] != a.port or len(data) < u + 8 + 12:
                continue
            r = u + 8
            if data[r] >> 6 != 2:
                continue
            seq, rtp_ts, ssrc = struct.unpack_from("!HII", data, r + 2)
            l1_pkt = None
            if l1_on:                                  # L1 대상 여부는 패킷 전체 구조를 검사한 뒤에만
                tot_len = struct.unpack_from("!H", data, off + 2)[0]
                frag = struct.unpack_from("!H", data, off + 6)[0] & 0x3FFF
                ulen = struct.unpack_from("!H", data, u + 4)[0]
                if rflags & MSG_TRUNC or off + tot_len > len(data):
                    why = "truncated"
                elif frag:
                    why = "fragment"
                elif tot_len < ihl + 8 or ulen != tot_len - ihl:
                    why = "length_mismatch"
                else:
                    why, l1_run, l1_tx = parse_l1(data[r:u + ulen], a.l1_ext_id)
                if why == "ok":
                    l1_pkt = (l1_run, l1_tx)
                else:
                    l1_excluded[why] = l1_excluded.get(why, 0) + 1
            arr_ns = None
            for lvl, typ, val in anc:
                if lvl == socket.SOL_SOCKET and typ == SO_TIMESTAMPING and len(val) >= 48:
                    hs, hn = struct.unpack_from("qq", val, 32)
                    ss, sn = struct.unpack_from("qq", val, 0)
                    if hs or hn:
                        arr_ns, src = hs * 10**9 + hn, "hw_ts"
                    elif ss or sn:
                        arr_ns, src = ss * 10**9 + sn, "sw_ts"
            if arr_ns is None:
                arr_ns, src = time.time_ns(), "sw_ts"
            tot[src] += 1
            st = streams.setdefault(ssrc, new_stream())
            now_mono = time.monotonic()
            st["first_mono"] = st["first_mono"] if st["first_mono"] is not None else now_mono
            st["last_mono"] = now_mono
            l2 = len(data) + 4
            st["pkts"] += 1
            st["bytes_l2"] += l2
            sec["pkts"] += 1
            sec["bytes_l2"] += l2
            kind, gap = track_seq(st, seq)
            sec["gap"] += gap
            if l1_pkt is not None:
                lr = l1_runs.setdefault(l1_pkt[0], {}).setdefault(f"0x{ssrc:08X}", new_l1run())
                lr["pkts"] += 1
                lr["first_tx_ns"] = l1_pkt[1] if lr["first_tx_ns"] is None else min(lr["first_tx_ns"], l1_pkt[1])
                lr["last_tx_ns"] = l1_pkt[1] if lr["last_tx_ns"] is None else max(lr["last_tx_ns"], l1_pkt[1])
                l1_kind, _ = track_seq(lr["seq"], seq)          # 중복 판별 = (run, SSRC) 별 순번 상태
                if l1_kind == "dup":
                    lr["duplicates_skipped"] += 1
                elif src != "hw_ts":
                    lr["missing_hw_ts"] += 1
                else:
                    lr["first_rx_ns"] = arr_ns if lr["first_rx_ns"] is None else min(lr["first_rx_ns"], arr_ns)
                    lr["last_rx_ns"] = arr_ns if lr["last_rx_ns"] is None else max(lr["last_rx_ns"], arr_ns)
                    d = arr_ns - l1_pkt[1]
                    if d < 0 or d > a.l1_max_us * 1000:
                        lr["abnormal"] += 1
                    else:
                        lr["samples"].append(d)
            if kind in ("reorder", "dup"):           # 늦게 온 패킷·중복은 도착 간격 계산에서 뺀다
                continue
            if src != st["last_src"]:                # 시각 출처가 바뀌면 이어 붙이지 않는다
                if st["last_src"] is not None:
                    tot["ts_source_switches"] += 1
                st["last_transit"] = st["last_arr"] = None
                st["last_src"] = src
            transit = arr_ns * RTP_CLOCK / 1e9 - rtp_ts
            if st["last_transit"] is not None:
                d = abs(transit - st["last_transit"])
                if d < RTP_CLOCK:                    # 같은 프레임·연속 프레임만(시계 넘김 제외)
                    st["jitter"] += (d - st["jitter"]) / 16
            st["last_transit"] = transit
            if st["last_arr"] is not None:
                st["max_gap"] = max(st["max_gap"], arr_ns - st["last_arr"])
            st["last_arr"] = arr_ns
        if l1_on:
            l1_stage = "end_status"
            l1_status_end = read_status(a.dev, a.iface)
            if l1_status_end is None:
                raise RuntimeError("끝 상태를 읽지 못했습니다")
            l1_stage = "done"
    except (Exception, KeyboardInterrupt) as e:      # noqa: BLE001 - L1 회차는 미완 기록을 남긴다. L1 을 쓰지 않으면 예전처럼 그대로 올린다
        if not l1_on:
            raise
        l1_old_sigint = defer_sigint(l1_box)         # 첫 취소·오류 처리 중 추가 Ctrl+C 는 세기만
        l1_interrupted = isinstance(e, KeyboardInterrupt)
        l1_err = f"stage {l1_stage}: {type(e).__name__}: {e}"
    finally:
        if l1_on and l1_old_sigint is None:
            l1_old_sigint = defer_sigint(l1_box)     # 정상 끝에서도 정리·게시 동안은 같은 정책
        if s is not None and l1_on:
            capture_drops, l1_c_intr, l1_stats_status, l1_c_first = l1_cleanup(s, prev, a.iface, l1_cleanup_errs)
        elif s is not None:
            if prev is not None:
                try:
                    set_hwts(s, a.iface, prev[2], tx_type=prev[1])
                except OSError:
                    pass
            try:   # 이 도구의 캡처 소켓이 놓친 프레임 수(망 손실이 아니라 측정 손실)
                capture_drops = struct.unpack("II", s.getsockopt(263, 6, 8))[1]   # SOL_PACKET, PACKET_STATISTICS
            except OSError:
                capture_drops = None
            s.close()
    per = {f"0x{k:08X}": stream_summary(v) for k, v in streams.items()}
    main_key = max(per, key=lambda k: per[k]["pkts"]) if per else None
    firsts = [v["first_mono"] for v in streams.values()]
    lasts = [v["last_mono"] for v in streams.values()]
    active = (max(lasts) - min(firsts)) if streams and max(lasts) > min(firsts) else None
    bytes_l2 = sum(v["bytes_l2"] for v in streams.values())
    summ = {"pkts": sum(v["pkts"] for v in per.values()), "bytes_l2": bytes_l2,
            "lost": sum(v["lost"] for v in per.values()), "reordered": sum(v["reordered"] for v in per.values()),
            "duplicates": sum(v["duplicates"] for v in per.values()), "loss_exact": all(v["loss_exact"] for v in per.values()),
            **tot, "capture_drops": capture_drops, "seconds": a.seconds, "active_seconds": active and round(active, 3),
            "L2_mbps": round(bytes_l2 * 8 / active / 1e6, 2) if active else None,
            "main_ssrc": main_key, "jitter_us": per[main_key]["jitter_us"] if main_key else None,
            "max_gap_us": per[main_key]["max_gap_us"] if main_key else None, "streams": per}
    if hdr_rejects:
        summ["header_rejects"] = dict(sorted(hdr_rejects.items()))
    if l1_on:
        # 기록 경계: 여기서부터 온 Ctrl+C 는 기록에 넣지 않는다(after_record). 원인 결정은 경계 뒤에 한다.
        l1_box["sealed"] = True
        if l1_cleanup_errs and l1_err is None:      # 정리 단계의 첫 오류·취소가 이 회차의 원인
            l1_err = f"stage cleanup: {l1_cleanup_errs[0]}"
            l1_interrupted = l1_c_intr
            l1_stage = "cleanup"
        if l1_err is None and l1_box["extra_interrupts"]:    # 정상 끝의 정리 중 첫 Ctrl+C(실제 신호) = 이 회차의 취소 원인
            l1_err = "stage cleanup: KeyboardInterrupt: 정리 중 첫 Ctrl+C(SIGINT)"
            l1_interrupted = True
            l1_stage = "cleanup"
            l1_finish_cause = True
        summ["l1"] = {"format": "L1v1", "acq_id": a.acq_id, "ext_id": a.l1_ext_id, "name": "송신 앱 -> 수신 MAC 지연(송신 SW·커널·DMA·큐잉 포함)",
                      "rx_iface": a.iface, "rx_dev": a.dev, "rx_ts_gptp": l1_rx_ts_gptp, "l1_max_us": a.l1_max_us,
                      "wall_start_utc": l1_wall_start, "wall_end_utc": utc_now(),
                      "status_start": l1_status_start, "status_end": l1_status_end,
                      "excluded": dict(sorted(l1_excluded.items())), "header_rejects": dict(sorted(hdr_rejects.items())),
                      "runs": {run: {ssrc: l1run_summary(v) for ssrc, v in per_ssrc.items()} for run, per_ssrc in l1_runs.items()},
                      "judgement": "finalize_required", "stage": l1_stage, "error": l1_err, "interrupted": l1_interrupted,
                      "cleanup_errors": l1_cleanup_errs, "cleanup_first_error_type": l1_c_first,
                      "cleanup_first_error_cancel": l1_c_intr,
                      "extra_interrupts_during_finish": l1_box["extra_interrupts"],
                      "finish_interrupt_is_cause": l1_finish_cause, "capture_drops_status": l1_stats_status,
                      "interrupt_count_boundary": "기록 JSON 을 만들기 직전까지의 SIGINT 만 센다. 그 뒤(게시·해제 중)는 표준출력 after_record_interrupts",
                      "complete": l1_err is None,
                      "note": "판정은 l1_finalize.py <이 파일> <송신 기록> --gm-evidence <수동 근거> 로 한다. 시작·끝 상태 기록은 중간 동기 이탈을 배제하지 않는다."}
    if l1_on:    # 예약한 경로에 게시(먼저 난 결과를 덮지 않음). 게시 중 예외·취소도 받아 자기 예약은 푼다(남의 예약·결과는 그대로)
        pub = None
        try:
            pub = l1_res.publish(json.dumps(summ, ensure_ascii=False, indent=1))
        except (Exception, KeyboardInterrupt) as e:    # noqa: BLE001
            pub = f"게시 중 {type(e).__name__}: {e}"
        finally:
            try:
                l1_res.release()
            except (Exception, KeyboardInterrupt) as e:       # noqa: BLE001 - 내 예약만 지우는 동작, 실패하면 예약이 남음(README 복구 절차)
                l1_res.release_status = l1_out.release_exception_status(l1_res, e)
            restore_sigint(l1_old_sigint)
        after = l1_box["after_record"]      # 처리기 복원 뒤의 Ctrl+C 는 기본 동작(이 도구가 세지 않음, 보장 밖)
        if pub is not True:     # 디스크에 기록이 없다: 성공 JSON 을 내지 않고, 복구 근거로 게시 못 한 기록을 표준출력에 남긴다
            print(json.dumps({"published": False, "error": f"기록 게시 실패: {pub}", "record_path": a.json,
                              "after_record_interrupts": after, "reservation_release": l1_res.release_status,
                              "unpublished_record": summ}, ensure_ascii=False))
            raise SystemExit(3)
        print(json.dumps(dict(summ, after_record_interrupts=after, published=True, reservation_release=l1_res.release_status), ensure_ascii=False))
    else:
        print(json.dumps(summ, ensure_ascii=False))
    if not l1_on and a.json:   # L1 을 쓰지 않을 때는 예전 그대로(끝난 기록만 그 이름으로 보이게)
        with open(a.json + ".tmp", "w", encoding="utf-8") as f:
            json.dump(summ, f, ensure_ascii=False, indent=1)
        os.replace(a.json + ".tmp", a.json)
    if l1_on and l1_err is not None:
        raise SystemExit(130 if l1_interrupted else 3)


if __name__ == "__main__":
    main()
