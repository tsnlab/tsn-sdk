#!/usr/bin/env python3
"""sensor_rx.py - sensor_tx.py 가 보낸 센서 데이터를 받아 묶음으로 다시 모으고 손실·지연을 기록한다.

수신 보드에서 root 로 실행한다. 표준 라이브러리만 쓴다.
수신 시각 = 보드 하드웨어 수신 시각(gPTP 시각). 송신 헤더의 gPTP 시각과 빼면 한 방향 지연이 나온다
(송신 쪽 소프트웨어 경로 포함). 하드웨어 시각이 없으면 지연은 계산하지 않는다.

출력:
  --csv <파일>   패킷마다 sender,stream,run,frame,chunk,seq,bytes,tx_ns,rx_ns,latency_us(검사를 통과한 패킷만)
  --out <폴더>   다시 모은 완전한 묶음을 frame_<송신IP>_<포트>_<스트림>_<실행식별값>_<번호>.bin 으로 저장
  끝에 요약 JSON 한 줄(묶음 수, 완전한 묶음, 패킷 손실, 잘못된 패킷과 이유, 지연 최소/중앙/99 %/최대)

송신자 구분(재조립·통계 키):
  - 키 = 송신 IP:포트 + 스트림 ID + 실행 식별값. 실행 식별값은 헤더 버전 3 의 64 비트 무작위 값(송신기가 시작할 때 os.urandom)이다.
    두 송신기가 같은 스트림 ID 를 써도, 송신기를 다시 시작해도 섞이지 않는다(두 실행이 같은 값을 고를 확률 2^-64).
  - 이전 판 송신기(버전 1 = 실행 구분 없음, 버전 2 = 16 비트)는 기본으로 거절한다(bad_packets 의 legacy_version).
    --accept-legacy 를 주면 받지만, 같은 송신 주소·포트에서 송신기를 다시 시작하면 이전 실행의 조각과 섞일 수 있다.
    이때는 송신기를 다시 시작할 때마다 수신기도 다시 시작한다.
  - --allow-from <IP> 를 주면 그 송신 주소에서 온 패킷만 받는다(나머지는 bad_packets 의 not_allowed).

조각 검사(어긋나면 bad_packets 로 세고 그 패킷은 버린다. 수신은 계속한다):
  - 헤더 버전 3(--accept-legacy 면 1·2), 조각 개수 1 이상, 0 <= 조각 번호 < 조각 개수, 묶음 길이 <= --max-frame-bytes
  - 조각 개수 = 묶음 길이를 1,400 B 로 나눈 올림(길이 0 이면 1), 조각 길이 = 마지막 조각 외 1,400 B, 마지막 조각은 나머지
  - 같은 묶음의 조각 개수·묶음 길이가 처음 받은 조각과 같아야 함
  - 같은 조각을 다시 받으면: 내용이 같으면 중복으로 세고, 다르면 conflict 로 세고 그 묶음을 깨진 묶음으로 둔다(저장하지 않음)
  - 이미 완성·저장한 묶음에 다른 내용 조각이 오면 conflict_after_complete 로 세고, 그 묶음을 완전한 묶음에서 빼고(invalidated, 묶음마다 한 번),
    저장한 파일을 <이름>.invalid 로 옮겨 격리한다(지우지 않음). 같은 이름이 이미 있으면 덮지 않고 .invalid.1, .2 ... 를 쓴다.
    요약 invalidated 에 묶음·이유·원래 파일(saved)·격리한 파일(quarantined)·오류(error)를 남긴다. 격리에 실패하면 quarantine_failed 로 센다.
    그 묶음에 충돌 조각이 더 오면 conflict_after_invalidated 로 사건만 센다
  - 완전한 묶음 = 모든 조각을 받고 깨지지 않았으며 다시 모은 길이가 묶음 길이와 같은 것. --out 파일은 이때 한 번만 새로 만든다
    (같은 이름 파일이 이미 있으면 덮지 않고 out_exists 로 센다)

손실 계산(송신자 키별로 따로 센다):
  - lost_packets = 받은 가장 작은 순번 ~ 가장 큰 순번 안에서 끝까지 오지 않은 순번 수.
  - 순서가 바뀌어 늦게 온 패킷은 reordered 로, 같은 순번을 두 번 받으면 duplicates 로 센다(손실이 아님).
  - 맨 앞·맨 뒤 패킷을 잃은 것은 이 범위로 알 수 없다. 송신 쪽 마지막 줄의 패킷 수와 streams.<ID>.packets(고유 순번 수)를 비교한다.
    맨 위 packets 는 중복까지 센 수신 횟수다.
  - 순번은 32 비트다. 순번 넘김(4,294,967,295 뒤 0)은 처리하지 않는다(초당 20 패킷이면 약 6.8 년 뒤).

사용:
  sudo python3 sensor_rx.py --iface eth1 --port 6000 --seconds 35 --csv rx.csv
"""
import argparse
import ctypes
import errno
import fcntl
import json
import os
import socket
import struct
import time

HDR = struct.Struct("!4sBBHIHHIQI")    # 공통 앞 32 B
RUN3 = struct.Struct("!Q")              # 버전 3: 앞 32 B 뒤의 64 비트 실행 식별값
MAGIC = b"TSNS"
SAFE_VERSIONS = (3,)                    # 기본으로 받는 버전
LEGACY_VERSIONS = (1, 2)                # --accept-legacy 일 때만 받는다(실행 구분이 없거나 약함)
MAX_CHUNK = 1400                        # sensor_tx.py 와 같아야 한다
SIOCSHWTSTAMP, SIOCGHWTSTAMP = 0x89B0, 0x89B1
SO_TIMESTAMPING = 37
FLAGS = 4 | 64          # RX_HARDWARE | RAW_HARDWARE


def set_hwts(sock, iface, rx_filter, tx_type=0, req=SIOCSHWTSTAMP):
    cfg = ctypes.create_string_buffer(struct.pack("iii", 0, tx_type, rx_filter))
    fcntl.ioctl(sock.fileno(), req, struct.pack("16sP", iface.encode(), ctypes.addressof(cfg)))
    return struct.unpack("iii", cfg.raw[:12])


class Reassembler:
    """검사를 통과한 조각만 송신자 키(송신 IP, 포트, 스트림 ID, 실행 식별값)별로 모은다. 소켓과 무관(시험에서 직접 넣는다)."""

    def __init__(self, out_dir=None, max_frame_bytes=16 << 20, allow_from=None, accept_legacy=False):
        self.out_dir, self.max_frame, self.allow, self.legacy = out_dir, max_frame_bytes, set(allow_from or ()), accept_legacy
        self.frames, self.streams = {}, {}
        self.pkts, self.bad, self.bad_reasons, self.out_exists, self.saved = 0, 0, {}, 0, {}
        self.invalidated = []          # 완성 뒤 무효가 된 묶음(묶음마다 한 번): frame·reason·idx·saved·quarantined·error
        self.quarantine_failed = 0

    def _quarantine(self, name):
        """저장 파일을 <이름>.invalid(있으면 .invalid.1, .2 ...)로 옮긴다. 이미 있는 파일은 덮지 않는다.
        새 이름은 os.link 로 원자적으로 만든다(있으면 실패). 그다음 원래 이름을 지운다. 돌려주는 값 = (격리된 이름, 오류)."""
        src = os.path.join(self.out_dir, name)
        for n in range(1000):
            dst_name = f"{name}.invalid" + (f".{n}" if n else "")
            dst = os.path.join(self.out_dir, dst_name)
            try:
                os.link(src, dst)
            except FileExistsError:
                continue
            except OSError as e:
                if e.errno not in (errno.EPERM, errno.EXDEV, errno.ENOTSUP, errno.ENOSYS, errno.EMLINK):
                    return None, f"격리 실패({e.__class__.__name__}: {e}) - {name} 는 그대로 있으나 무효 묶음이다"
                try:                                    # 하드 링크를 못 쓰는 파일 시스템: 새 이름을 배타적으로 만들어 복사
                    with open(src, "rb") as fi, open(dst, "xb") as fo:
                        fo.write(fi.read())
                except FileExistsError:
                    continue
                except OSError as e2:
                    return None, f"격리 실패({e2.__class__.__name__}: {e2}) - {name} 는 그대로 있으나 무효 묶음이다"
            try:
                os.unlink(src)
            except OSError as e:
                return dst_name, f"격리 사본은 만들었으나 원래 이름을 지우지 못함({e}) - {name} 는 무효 묶음이다"
            return dst_name, None
        return None, f"격리 실패(빈 이름 없음) - {name} 는 그대로 있으나 무효 묶음이다"

    def _bad(self, why):
        self.bad += 1
        self.bad_reasons[why] = self.bad_reasons.get(why, 0) + 1
        return None

    def feed(self, data, src):
        """한 패킷. 받아들이면 (key, frame, idx, seq, body, tx_ns) 를, 버리면 None 을 돌려준다."""
        if self.allow and src[0] not in self.allow:
            return self._bad("not_allowed")
        if len(data) < HDR.size or data[:4] != MAGIC:
            return self._bad("header")
        _, ver, sid, run16, frame, idx, cnt, seq, tx_ns, total = HDR.unpack_from(data)
        if ver in SAFE_VERSIONS:
            if len(data) < HDR.size + RUN3.size:
                return self._bad("header")
            run = f"{RUN3.unpack_from(data, HDR.size)[0]:016x}"
            body = data[HDR.size + RUN3.size:]
        elif ver in LEGACY_VERSIONS:
            if not self.legacy:
                return self._bad("legacy_version")
            run = "v1" if ver == 1 else f"v2-{run16:04x}"
            body = data[HDR.size:]
        else:
            return self._bad("version")
        if cnt < 1 or idx >= cnt:
            return self._bad("index")
        if total > self.max_frame:
            return self._bad("too_large")
        want_cnt = max(1, -(-total // MAX_CHUNK))
        want_len = MAX_CHUNK if idx < cnt - 1 else total - MAX_CHUNK * (cnt - 1)
        if cnt != want_cnt or len(body) != want_len:
            return self._bad("length")
        key = (src[0], src[1], sid, run)
        f = self.frames.get((key, frame))
        if f is None:
            f = self.frames[(key, frame)] = {"cnt": cnt, "total": total, "parts": {}, "broken": False, "done": False}
        elif f["cnt"] != cnt or f["total"] != total:
            return self._bad("frame_mismatch")
        old = f["parts"].get(idx)
        if old is not None and old != body:
            f["broken"] = True
            if f["done"]:                                   # 이미 완성(저장)한 묶음
                if f.get("invalid"):                        # 이미 무효로 바꾼 묶음: 상태 전환·격리는 다시 하지 않고 사건만 센다
                    return self._bad("conflict_after_invalidated")
                f["invalid"] = True                         # 유효 -> 무효 전환은 묶음마다 한 번
                ent = {"frame": f"{key[0]}:{key[1]}/{key[2]}/{key[3]}#{frame}", "reason": "conflict_after_complete",
                       "idx": idx, "saved": None, "quarantined": None, "error": None}
                name = self.saved.pop((key, frame), None)
                if name:
                    ent["saved"] = name
                    ent["quarantined"], ent["error"] = self._quarantine(name)
                    if ent["error"]:
                        self.quarantine_failed += 1
                self.invalidated.append(ent)
                return self._bad("conflict_after_complete")
            return self._bad("conflict")
        self.pkts += 1
        st = self.streams.setdefault(key, {"seen": set(), "max": None, "reordered": 0, "duplicates": 0})
        if seq in st["seen"]:
            st["duplicates"] += 1
        else:
            if st["max"] is not None and seq < st["max"]:
                st["reordered"] += 1
            st["seen"].add(seq)
            st["max"] = seq if st["max"] is None else max(st["max"], seq)
        if old is None:
            f["parts"][idx] = body
        if not f["done"] and not f["broken"] and len(f["parts"]) == cnt:
            blob = b"".join(f["parts"][i] for i in range(cnt))
            if len(blob) == total:
                f["done"] = True
                if self.out_dir:
                    name = f"frame_{key[0]}_{key[1]}_{sid}_{run}_{frame}.bin"
                    try:
                        with open(os.path.join(self.out_dir, name), "xb") as fo:
                            fo.write(blob)
                        self.saved[(key, frame)] = name
                    except FileExistsError:
                        self.out_exists += 1
        return key, frame, idx, seq, body, tx_ns

    def summary(self):
        per = {}
        for key, st in sorted(self.streams.items(), key=lambda kv: tuple(str(x) for x in kv[0])):
            lo = min(st["seen"])
            per[f"{key[0]}:{key[1]}/{key[2]}/{key[3]}"] = {
                "stream": key[2], "run": key[3], "packets": len(st["seen"]), "seq_first": lo, "seq_last": st["max"],
                "lost_packets": st["max"] - lo + 1 - len(st["seen"]), "reordered": st["reordered"], "duplicates": st["duplicates"]}
        fr = self.frames.values()
        return {"packets": self.pkts, "lost_packets": sum(v["lost_packets"] for v in per.values()),
                "reordered": sum(v["reordered"] for v in per.values()), "duplicates": sum(v["duplicates"] for v in per.values()),
                "streams": per, "bad_packets": self.bad, "bad_reasons": dict(sorted(self.bad_reasons.items())),
                "frames": len(self.frames), "complete_frames": sum(1 for f in fr if f["done"] and not f["broken"]),
                "broken_frames": sum(1 for f in fr if f["broken"] and not f["done"]),
                "invalidated_frames": len(self.invalidated), "invalidated": self.invalidated, "quarantine_failed": self.quarantine_failed,
                "out_exists": self.out_exists,
                "legacy_accepted": self.legacy}


def pct(v, q):
    return v[min(len(v) - 1, int(q * len(v)))] if v else None


def main():
    p = argparse.ArgumentParser(description="센서 데이터 TSN 수신")
    p.add_argument("--iface", default="eth1", help="하드웨어 수신 시각을 켤 인터페이스")
    p.add_argument("--port", type=int, default=6000)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--csv")
    p.add_argument("--out")
    p.add_argument("--allow-from", action="append", help="이 송신 IP 의 패킷만 받는다(여러 번 지정 가능)")
    p.add_argument("--max-frame-bytes", type=int, default=16 << 20, help="묶음 길이 상한(기본 16 MiB)")
    p.add_argument("--accept-legacy", action="store_true",
                   help="이전 판 송신기(헤더 버전 1·2)도 받는다. 같은 송신 주소에서 송신기를 다시 시작하면 섞일 수 있다(README 참고)")
    a = p.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, 33, 16 << 20)      # SO_RCVBUFFORCE
    except OSError:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 << 20)
    s.bind(("", a.port))
    s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING, FLAGS)
    s.settimeout(0.5)
    prev = None
    try:
        prev = set_hwts(s, a.iface, 0, req=SIOCGHWTSTAMP)
        set_hwts(s, a.iface, 1, tx_type=prev[1])
    except OSError:
        print("하드웨어 수신 시각을 켜지 못했습니다. 지연은 계산하지 않습니다.")
        prev = None

    csv = open(a.csv, "w", encoding="utf-8") if a.csv else None
    if csv:
        csv.write("sender,stream,run,frame,chunk,seq,bytes,tx_ns,rx_ns,latency_us\n")
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    ra = Reassembler(a.out, a.max_frame_bytes, a.allow_from, a.accept_legacy)
    lat = []
    t_end = time.monotonic() + a.seconds
    print(f"수신 대기: UDP {a.port}, {a.seconds:.0f} s", flush=True)
    try:
        while time.monotonic() < t_end:
            try:
                data, anc, _, src = s.recvmsg(2048, 256)
            except socket.timeout:
                continue
            r = ra.feed(data, src)
            if r is None:
                continue
            key, frame, idx, seq, body, tx_ns = r
            rx_ns = 0
            for lvl, typ, val in anc:
                if lvl == socket.SOL_SOCKET and typ == SO_TIMESTAMPING and len(val) >= 48:
                    hs, hn = struct.unpack_from("qq", val, 32)
                    rx_ns = hs * 1_000_000_000 + hn
            lus = (rx_ns - tx_ns) / 1000 if rx_ns and tx_ns else None
            if lus is not None:
                lat.append(lus)
            if csv:
                csv.write(f"{key[0]}:{key[1]},{key[2]},{key[3]},{frame},{idx},{seq},{len(body)},{tx_ns},{rx_ns},"
                          f"{'' if lus is None else f'{lus:.2f}'}\n")
    finally:
        if prev is not None:
            try:
                set_hwts(s, a.iface, prev[2], tx_type=prev[1])
            except OSError:
                pass
        s.close()
        if csv:
            csv.close()
    lat.sort()
    summ = ra.summary()
    summ["latency_us"] = {"min": pct(lat, 0) and round(lat[0], 1), "median": pct(lat, 0.5) and round(pct(lat, 0.5), 1),
                          "p99": pct(lat, 0.99) and round(pct(lat, 0.99), 1), "max": round(lat[-1], 1) if lat else None,
                          "samples": len(lat)}
    print(json.dumps(summ, ensure_ascii=False))


if __name__ == "__main__":
    main()
