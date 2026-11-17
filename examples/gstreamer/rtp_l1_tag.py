#!/usr/bin/env python3
"""rtp_l1_tag.py - 송신 쪽 RTP 중계기. 패킷마다 보드 gPTP 시각을 RTP 헤더 확장(L1 원소)에 넣어 TSN 망으로 보낸다.

수신 쪽 rtp_stats.py --l1-ext-id 가 이 시각과 수신 보드 하드웨어 수신 시각의 차를 모으고,
두 쪽 기록이 같은 회차인지 l1_finalize.py 가 확인한 뒤에만 지연을 판정한다.
재는 값: '송신 앱 -> 수신 MAC 지연(송신 SW·커널·DMA·큐잉 포함)'. 순수 MAC 사이 네트워크 지연이나 화면 출력 지연이 아니다.

구성(송신 보드): gst_send.sh 127.0.0.1 5004 -> (루프백 UDP) -> rtp_l1_tag.py -> (--egress-dev, UDP --port) -> 수신 보드
root 로 실행한다(gPTP 시각을 PCIe 로 읽고 송신 장치에 소켓을 묶기 때문). 표준 라이브러리만 쓴다.

시작 경계: 패킷마다 PCIe 로 읽은 송신 보드 gPTP 시각. 읽은 뒤의 확장 삽입·계수·sendto·커널·드라이버·DMA·보드 송신 큐 대기는 지표에 들어간다.
  GStreamer 부호화·패킷화와 루프백 전달, 중계기가 시각을 읽기 전 처리는 들어가지 않는다.
L1 원소(이 예제가 정한 형식, RFC 8285 one-byte header 의 로컬 ID 하나를 쓴다. ID 자체에 표준 의미는 없다):
  머리 1 B = (ID << 4) | 15, 데이터 16 B = 'L1'(2 B) + 형식 버전 1(1 B) + 플래그(1 B) + run(4 B) + gPTP ns(8 B, 부호 없음)
  플래그: bit0 = 시각 출처가 보드 gPTP(필수, 1). bit1-7 = 예약(0). 수신은 플래그가 정확히 0x01 이 아니면 bad_flags 로 뺀다.
  run = 이 중계기 실행마다 os.urandom 으로 고르는 32 비트 값. 인증값이 아니다. 서로 다른 두 실행의 run 이 우연히 같을 확률은
    2^-32 이고, 같으면 수신은 두 실행을 구별하지 못한다. 그래서 판정은 run 에 더해 원소의 송신 시각이 이번 기록의
    [첫 태그, 마지막 태그] 안에 있는지도 본다(l1_finalize.py).
  확장이 없는 패킷에는 확장 24 B(머리 4 B + 원소 17 B + 0 채움 3 B)가 붙는다. rtpjpegpay mtu=1400 이면 IPv4 패킷 1,452 B 이하.
넣지 않고 그대로 보내는 패킷(계수에 남김): RTP 가 아님, 길이·padding 이 맞지 않음, 다른 확장 형식(two-byte 등), 확장 구조 손상,
  ID 15 가 있음, 같은 ID 가 이미 있음(id_in_use). 기존 확장 원소는 지우거나 덮지 않는다.
송신 경로: --iface(보드 netdev)로 시각을 읽을 보드를 고르고, --egress-dev(기본 = --iface)에 소켓을 묶는다.
  --egress-dev 는 --iface 자신이거나, --iface 하나만 아래 장치로 가진 802.1Q VLAN 장치(sysfs uevent DEVTYPE=vlan)여야 한다.
  bridge·bond·macvlan 등 그 밖의 위 장치나 아래 장치가 둘 이상인 장치는 거절한다. 실제 OS 송신 경로(route·bridge 동작)는 보드에서 확인하지 않았다.
회차: --acq-id(같은 회차의 수신 rtp_stats.py 와 같은 값)를 기록에 남긴다. --json 경로가 이미 있거나 그 경로의 예약('<경로>.lock')이 있으면
  시작하지 않는다(회차마다 새 경로). 경로 예약·게시 규칙은 l1_out.py(배타 예약, 실행별 임시 파일, 먼저 난 결과를 덮지 않는 게시).
  장치 열기·소켓 준비·끝 상태 읽기 실패도 그 시도의 미완 기록(complete false, error)을 남기고 종료 코드 3 으로 끝난다.
  인자·경로·송신 장치 관계 확인에서 거절하면 기록을 쓰지 않는다(아무것도 시작하지 않음).
  정리(소켓 close) 실패는 OSError 도 정리 오류(미완 3)다. 중계 중 Ctrl+C 는 정상 끝, 그 밖의 단계·정리 중 첫 Ctrl+C 는 interrupted true·종료 130
  (수신 rtp_stats 와 같은 규칙). 기록 JSON 을 만든 뒤의 Ctrl+C 는 표준출력 after_record_interrupts 에만 남긴다.
기록의 tagged_per_ssrc 는 sendto 전에 센 태그 시도 수다. 실제 성공 송신 수가 아니므로 counts.send_errors 와 함께 본다.
gPTP 시각을 읽지 못하면 시작하지 않는다. 호스트 시계로 대신 채우지 않는다.

사용:
  sudo python3 rtp_l1_tag.py --iface eth1 --dest 192.0.2.20 --listen-port 5004 --port 5000 --seconds 35 --acq-id A1 --json A1/tag.json
"""
import argparse
import json
import os
import re
import socket
import struct
import sys
import time

sys.dont_write_bytecode = True   # sudo 로 실행해도 common/__pycache__ 를 root 소유로 남기지 않는다
HERE = os.path.dirname(os.path.abspath(__file__))
for d in (os.path.join(HERE, "..", "common"), os.path.join(HERE, "..", "..", "tools", "tsnctl")):
    if os.path.exists(os.path.join(d, "tsnctl.py")):
        sys.path.insert(0, d)
        break
import tsnctl  # noqa: E402
sys.path.insert(0, HERE)
import l1_out  # noqa: E402

ONE_BYTE_PROFILE = 0xBEDE
L1_MAGIC, L1_VERSION, L1_FLAG_GPTP = b"L1", 1, 0x01
L1_LEN = 16
SO_BINDTODEVICE = 25
SYS_NET = "/sys/class/net"
ACQ_RE = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")


def l1_element(ext_id, run, tx_ns):
    return bytes([(ext_id << 4) | (L1_LEN - 1)]) + L1_MAGIC + bytes([L1_VERSION, L1_FLAG_GPTP]) + struct.pack("!IQ", run, tx_ns & 0xFFFFFFFFFFFFFFFF)


def rtp_layout(pkt):
    """(헤더 끝, padding 뺀 끝) 또는 (None, 이유)."""
    n = len(pkt)
    if n < 12 or pkt[0] >> 6 != 2:
        return None, "not_rtp"
    h = 12 + 4 * (pkt[0] & 0x0F)
    if h > n:
        return None, "bad_rtp"
    end = n
    if pkt[0] & 0x20:
        p = pkt[-1]
        if p == 0 or p > n - h:
            return None, "bad_padding"
        end = n - p
    return (h, end), None


def tag_packet(pkt, ext_id, run, tx_ns):
    """L1 원소를 넣은 패킷과 상태. 상태가 'tagged' 가 아니면 원래 패킷을 그대로 돌려준다."""
    lay, why = rtp_layout(pkt)
    if lay is None:
        return pkt, why
    h, end = lay
    elem = l1_element(ext_id, run, tx_ns)
    if not pkt[0] & 0x10:                                        # 확장 없음
        data = elem + b"\0" * (-len(elem) % 4)
        return bytes([pkt[0] | 0x10]) + pkt[1:h] + struct.pack("!HH", ONE_BYTE_PROFILE, len(data) // 4) + data + pkt[h:], "tagged"
    if h + 4 > end:
        return pkt, "bad_ext"
    profile, words = struct.unpack_from("!HH", pkt, h)
    ext_end = h + 4 + 4 * words
    if ext_end > end:                                            # 확장이 패킷 끝이나 padding 을 침범
        return pkt, "bad_ext"
    if profile != ONE_BYTE_PROFILE:
        return pkt, "other_ext_profile"
    i, used = h + 4, h + 4
    while i < ext_end:
        b = pkt[i]
        if b == 0:                                               # 채움 바이트
            i += 1
            continue
        eid, ln = b >> 4, (b & 0x0F) + 1
        if eid == 15:                                            # 뒤를 해석할 수 없으므로 손대지 않는다
            return pkt, "id15_present"
        if i + 1 + ln > ext_end:
            return pkt, "bad_ext"
        if eid == ext_id:
            return pkt, "id_in_use"
        i += 1 + ln
        used = i
    data = pkt[h + 4:used] + elem
    data += b"\0" * (-len(data) % 4)
    return pkt[:h] + struct.pack("!HH", ONE_BYTE_PROFILE, len(data) // 4) + data + pkt[ext_end:], "tagged"


def egress_ok(iface, egress, sys_net=SYS_NET):
    """egress 가 iface 자신이거나 iface 하나만 아래 장치로 가진 802.1Q VLAN 장치인지. (참, 설명)."""
    if egress == iface:
        return True, "same"
    base = os.path.join(sys_net, egress)
    try:
        uevent = open(os.path.join(base, "uevent"), encoding="utf-8").read().split()
        lowers = sorted(x for x in os.listdir(base) if x.startswith("lower_"))
    except OSError:
        return False, f"{egress} 의 sysfs 정보를 읽지 못했습니다"
    if "DEVTYPE=vlan" not in uevent:
        return False, f"{egress} 는 VLAN 장치가 아닙니다(bridge·bond 등 다른 위 장치는 지원하지 않음)"
    if lowers != [f"lower_{iface}"]:
        return False, f"{egress} 의 아래 장치가 {iface} 하나가 아닙니다({lowers})"
    return True, f"vlan over {iface}"


def iface_pci(iface):
    try:
        return tsnctl.find_dev(None, iface)[1]
    except (SystemExit, Exception):     # noqa: BLE001 - 확인 못 함으로 남긴다
        return None


def status(bar, iface):
    """상태 블록(읽기만) + 상태를 읽은 장치 PCI·시각용 netdev 의 PCI. 읽지 못하면 예외."""
    if not tsnctl.has_mailbox(bar):
        return None
    st = tsnctl.status_block(bar)
    return {k: st.get(k) for k in ("format", "updates", "gptp", "resid", "fw_id")} | {"pci": bar.bdf, "iface": iface, "iface_pci": iface_pci(iface)}


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def defer_sigint(box):
    """정리·게시 동안 Ctrl+C 를 예외로 올리지 않고 센다(주 스레드가 아니면 None, 보장 밖). rtp_stats.defer_sigint 와 같은 규칙:
    기록 경계(box['sealed']) 앞은 extra_interrupts, 뒤는 after_record."""
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


def write_record(out, rec):
    """예약한 경로에 게시(먼저 난 결과는 덮지 않음). 성공 True, 실패면 이유."""
    return out.publish(json.dumps(rec, ensure_ascii=False, indent=1))


def main():
    p = argparse.ArgumentParser(description="RTP 송신 중계: L1 gPTP 시각 헤더 확장")
    p.add_argument("--dest", required=True, help="수신 보드 IP")
    p.add_argument("--port", type=int, default=5000, help="수신 UDP 포트(수신 쪽 gst_recv.sh·rtp_stats.py 와 같게)")
    p.add_argument("--listen-port", type=int, default=5004, help="gst_send.sh 가 보내는 루프백 UDP 포트")
    p.add_argument("--ext-id", type=int, default=1, help="RTP 헤더 확장 로컬 ID(1~14). 수신 쪽 --l1-ext-id 와 같게")
    p.add_argument("--iface", required=True, help="시각을 읽을 보드 netdev(예 eth1)")
    p.add_argument("--egress-dev", help="송신 소켓을 묶을 장치(기본 = --iface, VLAN 이면 예 eth1.10)")
    p.add_argument("--seconds", type=float, default=0, help="0 = 끝까지(Ctrl+C)")
    p.add_argument("--acq-id", required=True, help="이 회차 이름(영문·숫자·_.-, 64 자 이하). 수신 rtp_stats.py --acq-id 와 같게")
    p.add_argument("--json", required=True, help="이 회차 송신 기록(l1_finalize.py 입력). 이미 있으면 시작하지 않는다")
    p.add_argument("--dev", help="보드 장치(/dev/xdmaN_user). 주면 --iface 와 같은 보드인지 확인한다")
    a = p.parse_args()
    if not 1 <= a.ext_id <= 14:
        sys.exit("--ext-id 는 1~14 입니다(RFC 8285 one-byte header).")
    if not ACQ_RE.match(a.acq_id):
        sys.exit("--acq-id 는 영문·숫자·_.- 로 64 자 이하입니다.")
    egress = a.egress_dev or a.iface
    ok, how = egress_ok(a.iface, egress)
    if not ok:
        sys.exit(f"송신 장치 확인 실패: {how}")
    try:
        res_out = l1_out.Out(a.json, owner={"tool": "rtp_l1_tag", "acq_id": a.acq_id})
    except l1_out.Busy as e:
        sys.exit(str(e))
    rec = {"format": "L1v1", "acq_id": a.acq_id, "run": None, "ext_id": a.ext_id, "time_source": "board gPTP via PCIe (tsnctl.gptp_now_ns)",
           "board": None, "egress": {"dev": egress, "relation": how}, "dest": a.dest, "port": a.port, "listen_port": a.listen_port,
           "wall_start_utc": utc_now(), "wall_end_utc": None, "status_start": None, "status_end": None,
           "counts": None, "tagged_per_ssrc": None, "first_tag_gptp_ns": None, "last_tag_gptp_ns": None,
           "tagged_per_ssrc_note": "sendto 전에 센 태그 시도 수(실제 성공 송신 수 아님). counts.send_errors 와 함께 본다",
           "stage": "setup", "error": None, "interrupted": False, "complete": False}
    cnt = {"received": 0, "sent": 0, "tagged": 0, "send_errors": 0}
    per_ssrc = {}
    first_ns = last_ns = None
    socks = []
    err = None
    cleanup_errs = []
    box = {"extra_interrupts": 0, "after_record": 0, "sealed": False}
    intr = c_intr = finish_cause = False
    c_first = None          # 첫 정리 오류 종류(표시용). 취소 여부 c_intr 는 isinstance 로 따로
    try:
        bar = tsnctl.Bar(a.dev, iface=a.iface)        # 장치를 못 열면 SystemExit -> 미완 기록(호스트 시계로 대신 채우지 않음)
        rec["board"] = {"dev": bar.dev, "pci": bar.bdf, "iface": a.iface, "iface_pci": iface_pci(a.iface)}
        if a.dev and rec["board"]["iface_pci"] != bar.bdf:
            raise RuntimeError(f"--dev {a.dev}(PCIe {bar.bdf}) 와 --iface {a.iface}(PCIe {rec['board']['iface_pci']}) 가 다른 보드입니다")
        run = int.from_bytes(os.urandom(4), "big")
        rec["run"] = f"{run:08x}"
        rec["status_start"] = status(bar, a.iface)
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        socks.append(rx)
        rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        rx.bind(("127.0.0.1", a.listen_port))
        rx.settimeout(0.5)
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        socks.append(tx)
        tx.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
        tx.setsockopt(socket.SOL_SOCKET, SO_BINDTODEVICE, egress.encode() + b"\0")
        rec["stage"] = "relay"
        t_end = time.monotonic() + a.seconds if a.seconds else None
        print(f"중계 시작: 127.0.0.1:{a.listen_port} -> {a.dest}:{a.port} ({egress}), 확장 ID {a.ext_id}, run {run:08x}, 회차 {a.acq_id}", flush=True)
        try:
            while t_end is None or time.monotonic() < t_end:
                try:
                    pkt = rx.recv(65535)
                except socket.timeout:
                    continue
                cnt["received"] += 1
                tx_ns = tsnctl.gptp_now_ns(bar)                # 시작 경계
                out, st = tag_packet(pkt, a.ext_id, run, tx_ns)
                cnt[st] = cnt.get(st, 0) + 1
                if st == "tagged":
                    k = f"0x{struct.unpack_from('!I', pkt, 8)[0]:08X}"
                    per_ssrc[k] = per_ssrc.get(k, 0) + 1
                    first_ns = tx_ns if first_ns is None else first_ns
                    last_ns = tx_ns
                try:
                    tx.sendto(out, (a.dest, a.port))
                    cnt["sent"] += 1
                except OSError:
                    cnt["send_errors"] += 1
        except KeyboardInterrupt:
            pass
        rec["stage"] = "end_status"
        rec["status_end"] = status(bar, a.iface)
        rec["stage"] = "done"
    except KeyboardInterrupt as e:            # 하위 클래스 포함(isinstance). 표시는 클래스 이름
        err = f"stage {rec['stage']}: {type(e).__name__}"
        intr = True
    except (SystemExit, Exception) as e:      # noqa: BLE001 - 기록에 남기고 미완으로 둔다
        err = f"stage {rec['stage']}: {type(e).__name__}: {e}"
    finally:
        old_sigint = defer_sigint(box)          # rtp_stats 와 같은 규칙: 정리·게시 동안 추가 Ctrl+C 는 세기만
        for s in socks:
            try:
                s.close()
            except (Exception, KeyboardInterrupt) as e:   # noqa: BLE001 - OSError 포함, 정리 중 예외·취소도 기록에 남김
                if not cleanup_errs:
                    c_first = type(e).__name__                    # 표시용 종류
                    c_intr = isinstance(e, KeyboardInterrupt)     # 취소 여부(하위 클래스 포함), 첫 오류 하나로
                cleanup_errs.append(f"cleanup close: {type(e).__name__}: {e}")
    box["sealed"] = True                    # 기록 경계: 이 뒤 Ctrl+C 는 기록에 넣지 않음(after_record)
    if cleanup_errs and err is None:
        err = f"stage cleanup: {cleanup_errs[0]}"
        intr = c_intr
        rec["stage"] = "cleanup"
    if err is None and box["extra_interrupts"]:     # 정상 끝의 정리 중 첫 Ctrl+C(실제 신호) = 이 회차의 취소 원인
        err = "stage cleanup: KeyboardInterrupt: 정리 중 첫 Ctrl+C(SIGINT)"
        intr = finish_cause = True
        rec["stage"] = "cleanup"
    rec.update(counts=cnt, tagged_per_ssrc=per_ssrc, first_tag_gptp_ns=first_ns, last_tag_gptp_ns=last_ns,
               wall_end_utc=utc_now(), error=err, interrupted=intr, complete=err is None, cleanup_errors=cleanup_errs,
               cleanup_first_error_type=c_first, cleanup_first_error_cancel=c_intr,
               extra_interrupts_during_finish=box["extra_interrupts"], finish_interrupt_is_cause=finish_cause,
               interrupt_count_boundary="기록 JSON 을 만들기 직전까지의 SIGINT 만 센다. 그 뒤(게시·해제 중)는 표준출력 after_record_interrupts")
    pub = None
    try:
        pub = write_record(res_out, rec)
    except (Exception, KeyboardInterrupt) as e:     # noqa: BLE001
        pub = f"게시 중 {type(e).__name__}: {e}"
    finally:
        try:
            res_out.release()
        except (Exception, KeyboardInterrupt) as e:        # noqa: BLE001
            res_out.release_status = l1_out.release_exception_status(res_out, e)
        restore_sigint(old_sigint)
    after = box["after_record"]      # 처리기 복원 뒤의 Ctrl+C 는 기본 동작(이 도구가 세지 않음, 보장 밖)
    if pub is not True:     # 디스크에 기록 없음: 성공 요약을 내지 않고 게시 못 한 기록을 복구 근거로 표준출력에
        print(json.dumps({"acq_id": a.acq_id, "published": False, "complete": False, "error": f"기록 게시 실패: {pub}",
                          "record_path": a.json, "after_record_interrupts": after, "reservation_release": res_out.release_status,
                          "unpublished_record": rec}, ensure_ascii=False))
        sys.exit(3)
    print(json.dumps({"acq_id": a.acq_id, "run": rec["run"], "counts": cnt, "complete": rec["complete"], "error": err,
                      "interrupted": intr, "published": True, "after_record_interrupts": after,
                      "reservation_release": res_out.release_status}, ensure_ascii=False))
    if not rec["complete"]:
        sys.exit(130 if intr else 3)     # 수신 rtp_stats 와 같은 규칙(취소 130, 그 밖 미완 3)


if __name__ == "__main__":
    main()
