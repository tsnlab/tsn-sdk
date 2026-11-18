#!/usr/bin/env python3
"""tsnctl - TSN HAT / TSN NIC 를 PCIe(BAR)로만 설정하고 읽는다(예제용 축소판). UART 를 쓰지 않는다.
메일박스 = PCIe BAR 안에서 보드 펌웨어와 상태를 주고받는 영역(FPGA 이미지 v3 부터). 이 판은 상태 블록 읽기만 한다.

보드가 꽂힌 호스트(Raspberry Pi 5 또는 PC)에서 root 로 실행한다. 표준 라이브러리만 쓴다(Python 3.9 이상).
드라이버(xdma, se_mode=1)가 적재되어 /dev/xdmaN_user 가 있어야 한다. 보드가 여러 장이면 --dev 또는 --iface 로 고른다.

명령:
  info                                   코어 버전·드라이버 설정·현재 시각
  link [--port all]                      포트 링크 상태·속도(PHY)
  status                                 gPTP 상태(role·asCapable·offset·pdelay)·보드 MAC·DNA·펌웨어 ID(메일박스가 있는 FPGA 이미지)
  sync [--seconds 2] [--offset-limit 500] gPTP 동기 판정 synced(SlavePort·|offset|·상태 갱신), servo_active 는 참고
  tas set --port eth1 --cycle 1000 --window 0x02:500 --window 0x00:500
  tas off --port eth1 | tas status --port eth1
  cbs set --port eth1 --queue 1 --rate 300   (L2 Mbps)
  cbs off --port eth1 --queue 1 | --all
  guardband --port eth1 --queue 1 --us 12.3

포트: eth1 = 보드 TSN 포트 1, eth2 = 포트 2(호스트 인터페이스 이름과 다를 수 있다).
큐: 호스트(리눅스)가 보내는 프레임 = 큐 1(VLAN PCP 와 무관), gPTP = 큐 0(FPGA 이미지 v3.3 부터, 이전 이미지는 큐 3).
    큐 번호가 작을수록 먼저 나간다(큐 0 이 가장 먼저, 802.1Q traffic class 번호 관례와 반대). tas set 은 큐 0 과 큐 3 을 모두 연다(두 이미지 공통).
--dry-run: 보드에 쓰지 않고 쓰기 순서만 출력한다(빈 레지스터 모형).
"""
import argparse
import errno
import fcntl
import json
import os
import socket
import struct
import sys
import time

DEV = "/dev/xdma0_user"            # dry-run 과 옛 호출의 기본값. 실제 장치는 find_dev() 로 고른다
SYS_XDMA = "/sys/class/xdma"        # 드라이버가 만든 문자 장치(xdmaN_user)의 sysfs
SYS_NET = "/sys/class/net"
RTC = 0x20800                       # 포트 1 RTC(셰이퍼·게이트 시각 기준)
PORTS = {"eth1": {"shaper": 0x22000, "ctrl": 0x2F000},
         "eth2": {"shaper": 0x42000, "ctrl": 0x4F000}}
# gPTP 송신 큐: FPGA 이미지 v3.3 = 0(최상위), v3.2b 이전 = 3. 이미지가 섞여도 되도록 둘 다 gPTP 큐로 다룬다
PTP_QUEUES = (0, 3)
PTP_GATE_BITS = (1 << 0) | (1 << 3)
SYSTICK_NS = 8
CBS_CLOCK_HZ = 125_000_000
CBS_SCALE = 65536


def list_devs(sys_xdma=SYS_XDMA):
    """보드마다 하나씩인 /dev/xdmaN_user 와 그 PCIe BDF 목록. [(dev, bdf), ...]"""
    out = []
    try:
        names = sorted(n for n in os.listdir(sys_xdma) if n.startswith("xdma") and n.endswith("_user"))
    except OSError:
        return out
    for n in names:
        try:
            bdf = os.path.basename(os.path.realpath(os.path.join(sys_xdma, n, "device")))
        except OSError:
            bdf = None
        out.append(("/dev/" + n, bdf))
    return out


def find_dev(dev=None, iface=None, sys_xdma=SYS_XDMA, sys_net=SYS_NET):
    """쓸 보드를 고른다. --dev 가 있으면 그것, --iface 가 있으면 그 netdev 의 PCIe 장치,
    둘 다 없으면 보드가 하나일 때만 그 보드. 보드가 둘 이상인데 고르지 않으면 멈춘다(다른 카드를 건드리지 않게)."""
    devs = list_devs(sys_xdma)
    if dev:
        return dev, dict(devs).get(dev)
    if iface:
        try:
            bdf = os.path.basename(os.path.realpath(os.path.join(sys_net, iface, "device")))
        except OSError:
            bdf = None
        hit = [d for d in devs if d[1] and d[1] == bdf]
        if not hit:
            raise SystemExit(f"{iface} 에 해당하는 보드 장치를 찾지 못했습니다(PCIe {bdf}). 보드 목록: {devs or '없음'}")
        return hit[0]
    if len(devs) == 1:
        return devs[0]
    if not devs:
        return DEV, None                                 # sysfs 를 못 읽는 환경: 예전 기본값(열 때 오류로 알림)
    raise SystemExit("보드가 여러 장입니다. --dev 또는 --iface 로 하나를 고르십시오: "
                     + ", ".join(f"{d} (PCIe {b})" for d, b in devs))


class Bar:
    """BAR0 32 비트 읽기·쓰기. dry_run 이면 메모리 모형에 쓰고 쓰기 기록만 남긴다.
    dev 를 주지 않으면 find_dev() 로 고른다(보드가 둘 이상이면 멈춤)."""

    def __init__(self, dev=None, dry_run=False, iface=None):
        self.dry = dry_run
        self.mem, self.log = {}, []
        if dry_run:
            self.dev, self.bdf = dev or DEV, None
        else:
            self.dev, self.bdf = find_dev(dev, iface)
        dev = self.dev
        if not dry_run:
            try:
                self.fd = os.open(dev, os.O_RDWR | os.O_SYNC)
            except OSError as e:
                sys.exit(f"{dev} 를 열 수 없습니다({e.strerror}). root 로 실행하고 xdma 드라이버 적재를 확인하십시오.")

    def rd(self, off):
        if self.dry:
            return self.mem.get(off, 0)
        return struct.unpack("<I", os.pread(self.fd, 4, off))[0]

    def wr(self, off, val):
        val &= 0xFFFFFFFF
        if self.dry:
            self.mem[off] = val
            self.log.append(f"W 0x{off:05X} = 0x{val:08X}")
            return
        os.pwrite(self.fd, struct.pack("<I", val), off)


# ---- 시각 ---------------------------------------------------------------------------------------
def ofs_sec(hi, lo):
    """RTC offset 초 = 48 비트 부호 있는 값(+0x28 하위 16 비트 = 상위, +0x24 = 하위 32 비트).
    GM 이 다시 켜져 GM 시각이 보드 curtime 보다 작으면 음수가 된다(드라이버 se_rtc.c 와 같은 해석)."""
    v = ((hi & 0xFFFF) << 32) | lo
    return v - (1 << 48) if v & (1 << 47) else v


def latch_curtime(b, tries=4):
    """RTC 래치 뒤 초 -> ns -> 초로 읽고 두 초가 같을 때만 받는다. 보드 펌웨어도 같은 래치를 써서,
    초와 ns 사이에 펌웨어가 다시 래치하고 초 경계를 넘으면 1 s 틀린 값이 된다(보드 실측)."""
    for _ in range(tries):
        b.wr(RTC + 0x10, 0)
        sec, ns, sec2 = b.rd(RTC + 0x14), b.rd(RTC + 0x10) & 0x3FFFFFFF, b.rd(RTC + 0x14)
        if sec == sec2 or b.dry:
            return sec, ns
    return sec2, b.rd(RTC + 0x10) & 0x3FFFFFFF


def rtc_now(b):
    """curtime 과 offset 을 함께 잠그고 읽는다. gPTP 시각 = curtime + offset."""
    cur_sec, cur_ns = latch_curtime(b)
    ofs = ofs_sec(b.rd(RTC + 0x28), b.rd(RTC + 0x24))
    return {"curtime_sec": cur_sec, "curtime_ns": cur_ns, "offset_sec": ofs,
            "offset_ns": b.rd(RTC + 0x20) & 0x3FFFFFFF, "gptp_sec": (cur_sec + ofs) & 0xFFFFFFFF}


def gptp_now_ns(b):
    """gPTP 시각(ns) = curtime + RTC offset. 수신 하드웨어 시각과 같은 시간축이다(PCIe 읽기 약 수 us)."""
    cur_sec, cur_ns = latch_curtime(b)                    # curtime·offset 을 함께 잠금
    ofs = ofs_sec(b.rd(RTC + 0x28), b.rd(RTC + 0x24))
    ofs_ns = b.rd(RTC + 0x20) & 0x3FFFFFFF
    return (cur_sec + ofs) * 1_000_000_000 + cur_ns + ofs_ns


def read_offset(b):
    lo, hi, ns = b.rd(RTC + 0x24), b.rd(RTC + 0x28), b.rd(RTC + 0x20)
    return (hi << 32) | lo, ns


# ---- 셰이퍼 ---------------------------------------------------------------------------------------
def shaper(port):
    if port not in PORTS:
        raise SystemExit("포트는 eth1 또는 eth2 입니다.")
    return PORTS[port]["shaper"]


def tas_status(b, port, samples=200):
    sh = shaper(port)
    seen = set()
    for _ in range(samples if not b.dry else 1):
        seen.add((b.rd(sh + 0x1000) >> 16) & 0xFF)
        if not b.dry:
            time.sleep(0.001)
    cr = b.rd(sh + 0x1000)
    return {"port": port, "enabled": cr & 1, "cfgerr": (cr >> 2) & 1, "cfgpend": (cr >> 3) & 1,
            "gate_states_seen": [f"0x{g:02X}" for g in sorted(seen)],
            "admin_cycle_ns": b.rd(sh + 0x1004), "list_len": b.rd(sh + 0x1010) & 0xFF,
            "oper_cycle_ns": b.rd(sh + 0x1014), "oper_base_sec": b.rd(sh + 0x101C),
            "tx_overrun": [b.rd(sh + 0x1040 + 4 * q) for q in range(8)],
            "guardband_ticks": [b.rd(sh + 0x1020 + 4 * q) & 0x7FFFFF for q in range(8)],
            "cbs_enabled": [b.rd(sh + 0x120 + 4 * q) & 1 for q in range(8)],
            "cbs_idleslope": [b.rd(sh + 0x140 + 4 * q) for q in range(8)],
            "cbs_l2_mbps": [round(b.rd(sh + 0x140 + 4 * q) * CBS_CLOCK_HZ / CBS_SCALE / 1e6, 2) for q in range(8)]}


def tas_off(b, port):
    sh = shaper(port)
    b.wr(sh + 0x1000, b.rd(sh + 0x1000) & ~1)


def tas_set(b, port, cycle_us, windows, allow_ptp_close=False, base_rel_s=2):
    """windows = [(gate_mask, dur_us)]. GCL -> cycle·길이 -> base(gPTP 초 경계) -> CFGCHG -> EN.
    보드 실측으로 확인한 순서다. 오류(cfgerr)면 끈다."""
    sh = shaper(port)
    depth_bits = (b.rd(sh + 0x04) >> 24) & 0xFF
    depth = (1 << depth_bits) if depth_bits else 8
    if not 1 <= len(windows) <= depth:
        raise SystemExit(f"창 개수는 1~{depth} 개입니다.")
    if sum(d for _, d in windows) != cycle_us:
        raise SystemExit("창 길이 합이 --cycle 과 같아야 합니다.")
    entries = []
    for gate, dur_us in windows:
        if not 0 <= gate <= 0xFF or dur_us <= 0:
            raise SystemExit(f"잘못된 창: {gate:#x}:{dur_us}")
        if not allow_ptp_close:
            gate |= PTP_GATE_BITS
        entries.append((gate, dur_us * 1000))
    tas_off(b, port)
    for i, (gate, dur_ns) in enumerate(entries):
        b.wr(sh + 0x1800 + 8 * i, gate & 0xFF)               # OP 0 = 게이트 상태 설정
        b.wr(sh + 0x1804 + 8 * i, dur_ns // SYSTICK_NS)
    b.wr(sh + 0x1004, cycle_us * 1000)
    b.wr(sh + 0x1010, len(entries))
    base = (rtc_now(b)["gptp_sec"] + base_rel_s) & 0xFFFFFFFF
    b.wr(sh + 0x100C, base)
    b.wr(sh + 0x1008, 0)
    b.wr(sh + 0x1000, b.rd(sh + 0x1000) | 0x6)               # CFGCHG | CFGERR(1 쓰면 지움)
    b.wr(sh + 0x1000, b.rd(sh + 0x1000) | 0x1)               # EN
    if not b.dry:
        time.sleep(base_rel_s + 0.6)
    st = tas_status(b, port)
    if not b.dry and (st["enabled"] != 1 or st["cfgerr"]):
        tas_off(b, port)
        raise SystemExit(f"TAS 가 정상 적용되지 않아 껐습니다: {st}")
    st["base_sec"] = base
    st["windows"] = [f"0x{g:02X}:{d // 1000}us" for g, d in entries]
    return st


def cbs_slope(l2_mbps):
    if not 0 < l2_mbps <= 1000:
        raise SystemExit("CBS 대역은 0 초과 1000 Mbps 이하입니다(L2: 이더넷 헤더~FCS).")
    return round(l2_mbps * 1e6 * CBS_SCALE / CBS_CLOCK_HZ)


def cbs_set(b, port, queue, l2_mbps, allow_ptp_queue=False):
    sh = shaper(port)
    if not 0 <= queue <= 7:
        raise SystemExit("큐는 0~7 입니다.")
    if queue in PTP_QUEUES and not allow_ptp_queue:
        raise SystemExit(f"큐 {queue} 는 gPTP 큐입니다. 셰이핑하면 시각 동기가 늦어집니다. 꼭 필요하면 --allow-ptp-queue 를 줍니다.")
    slope = cbs_slope(l2_mbps)
    b.wr(sh + 0x120 + 4 * queue, 0)
    b.wr(sh + 0x140 + 4 * queue, slope)
    b.wr(sh + 0x120 + 4 * queue, 1)
    return {"port": port, "queue": queue, "l2_mbps": l2_mbps, "idleslope": slope,
            "enabled": b.rd(sh + 0x120 + 4 * queue) & 1}


def cbs_off(b, port, queue):
    sh = shaper(port)
    b.wr(sh + 0x120 + 4 * queue, 0)


def guardband(b, port, queue, us):
    sh = shaper(port)
    ticks = round(us * 1000 / SYSTICK_NS)
    if not 0 <= ticks <= 0x7FFFFF:
        raise SystemExit("guardband 범위를 벗어났습니다.")
    b.wr(sh + 0x1020 + 4 * queue, ticks)
    return {"port": port, "queue": queue, "ticks": b.rd(sh + 0x1020 + 4 * queue) & 0x7FFFFF}


# ---- 정보·동기 ------------------------------------------------------------------------------------
def driver_params():
    d = "/sys/module/xdma/parameters"
    out = {}
    for k in ("se_mode", "rx_poll_mode", "rx_ts_gptp", "se_tx_dest_port", "tx_prefetch"):
        try:
            with open(os.path.join(d, k)) as f:
                out[k] = f.read().strip()
        except OSError:
            pass
    try:
        with open("/sys/module/xdma/srcversion") as f:
            out["srcversion"] = f.read().strip()
    except OSError:
        pass
    return out


def info(b):
    ver = b.rd(0x2000)
    param = b.rd(0x2004)
    ports = {}
    for n, p in PORTS.items():
        cfg = b.rd(p["ctrl"] + 0x14)
        ports[n] = {"fpe_included": bool(cfg & (1 << 29))}
    return {"core_version": f"{(ver >> 24) & 0xFF}.{(ver >> 16) & 0xFF}.{ver & 0xFFFF}",
            "core_version_raw": f"0x{ver:08X}", "core_param_raw": f"0x{param:08X}",
            "qci_included": bool(param & (1 << 18)), "ports": ports,
            "driver": driver_params(), "time": rtc_now(b)}


SLAVE_PORT = 9                                     # 802.1AS 포트 role: 3 Disabled, 6 Master, 7 Passive, 9 Slave
OFFSET_LIMIT_NS = 500                              # 남는 offset 판정 범위(ns)
SYNC_INTERVAL_S = 0.125                            # GM Sync 간격(802.1AS 기본 logSyncInterval -3)
# 상태 블록의 gmOffset 은 시각 오차가 아니라 "Sync 사이 offset 모형 변화량"이다. 스택 모형이 주파수 보정(ppb)을
# 2^-32 단위로 써서 1e9/2^32(23 %)만 반영하므로 매 Sync 마다 (1 - 1e9/2^32) x ppb x 간격 이 남는다(보드 발진기 오차에 비례,
# 다섯 보드 실측 비 0.09 - 0.11). 실제 RTC 는 OFS_CH 로 ppb 그대로 보정되고 위상은 매 Sync 절대값으로 맞춘다.
GMOFS_PER_PPB = (1 - 1e9 / 2 ** 32) * SYNC_INTERVAL_S   # ns / ppb (= 0.0959)


def rate_ppb(b):
    """RTC 주파수 보정(OFS_CH, RTC +0x2C, 부호 있음)을 ppb 로. INCR(+0x1C) = ns x 2^24."""
    inc, ch = b.rd(RTC + 0x1C), b.rd(RTC + 0x2C)
    ch = ch - (1 << 32) if ch & (1 << 31) else ch
    return ch * 1e9 / inc if inc else 0.0


def sync(b, seconds, offset_limit=OFFSET_LIMIT_NS):
    """gPTP 동기 판정. 메일박스 이미지면 상태 블록으로 판정한다:
    SlavePort(role 9)·asCapable 1 인 포트가 있고, 남는 offset(gmOffset - 주파수 보정 몫, GMOFS_PER_PPB 설명)이 offset_limit 이하이고,
    펌웨어가 상태 블록을 계속 갱신할 때 synced. 상태 블록은 시작과 끝에 한 번씩 읽어 두 번 다 맞아야 한다.
    gmOffset 그대로 비교하면 발진기 오차가 약 5 ppm 넘는 정상 보드도 "미동기"가 된다(예: +409 ns = 4.0 ppm).
    servo_updates·servo_active(RTC offset 이 seconds 동안 초당 100 번 이상 바뀜)는 참고 값이다. 부하 중에는 기준 근처에서 흔들린다(보드 실측)."""
    st0 = status_block(b) if not b.dry and has_mailbox(b) else None
    ppb0 = rate_ppb(b) if st0 is not None else None
    changes, prev = 0, read_offset(b)
    first = prev
    t_end = time.monotonic() + seconds
    while time.monotonic() < t_end and not b.dry:
        cur = read_offset(b)
        if cur != prev:
            changes += 1
            prev = cur
        time.sleep(0.0005)
    raw, ns = prev
    sec = ofs_sec(raw >> 32, raw & 0xFFFFFFFF)
    r = {"synced": None, "seconds": seconds}
    if st0 is not None:
        st1 = status_block(b)
        ppb1 = rate_ppb(b)

        def resid(st, ppb):
            if "resid" in st:                                 # v3.4 펌웨어(형식 2): 실제 Sync 간격으로 계산한 값
                return st["resid"]["offset_ns"]
            return round(st["gptp"]["offset_ns"] - GMOFS_PER_PPB * ppb, 1)

        def ok(st, ppb):
            g = st["gptp"]
            slaves = [p for p in ("port1", "port2") if g[p]["role"] == SLAVE_PORT and g[p]["as_capable"] == 1]
            return slaves, abs(resid(st, ppb)) <= offset_limit
        s0, o0 = ok(st0, ppb0)
        s1, o1 = ok(st1, ppb1)
        alive = st1["updates"] != st0["updates"]
        if "resid" in st1:                                    # 남는 offset 계산도 진행 중이어야 함(Sync 를 받고 있음)
            alive = alive and st1["resid"]["count"] != st0["resid"]["count"]
        r.update({"synced": bool(s0 and s1 and o0 and o1 and alive), "slave_port": s1,
                  "residual_source": "firmware(형식 2)" if "resid" in st1 else f"tsnctl({GMOFS_PER_PPB:.4f} x rate_ppb, Sync {SYNC_INTERVAL_S} s 가정)",
                  "offset_ns": [st0["gptp"]["offset_ns"], st1["gptp"]["offset_ns"]],
                  "rate_ppb": [round(ppb0, 1), round(ppb1, 1)],
                  "offset_residual_ns": [resid(st0, ppb0), resid(st1, ppb1)], "offset_limit_ns": offset_limit,
                  "status_alive": alive,
                  "criterion": f"SlavePort·asCapable 포트 있음, |offset - {GMOFS_PER_PPB:.4f} x rate_ppb| <= {offset_limit} ns, 상태 블록 갱신 중(시작·끝 두 번)"})
    else:
        r["note"] = "dry-run" if b.dry else "메일박스가 없는 이미지라 synced 를 판정하지 못합니다. servo_active 만 참고하십시오."
    r.update({"servo_updates": changes, "servo_active": changes >= seconds * 100,
              "rtc_offset": f"{sec} s + {ns & 0x3FFFFFFF} ns", "first_vs_last_changed": first != prev,
              "servo_note": "servo_active 는 참고 값입니다(RTC offset 갱신 횟수 간접 판정)."})
    return r


# ---- 링크(PHY MDIO) ---------------------------------------------------------------------------
MDIO_BASE = {"eth1": 0x20000, "eth2": 0x40000}     # 포트 MAC 블록. CFG +0x20, CTRL +0x24, RD +0x28
PHY_ID1 = 0x0022                                   # KSZ9031 PHY ID 1


def mdio_read(b, base, pa, reg, tries=20000):
    """MDIO 한 번 읽기. 시작을 쓴 직후에는 ready 가 아직 이전 전송의 1 로 보일 수 있다(보드 실측: 이전 결과를 돌려받음).
    한 프레임(MDC 약 2.5 MHz x 64 비트 = 약 26 us)보다 길게 쉰 뒤 ready 를 본다."""
    b.wr(base + 0x24, (0x2 << 30) | (pa << 24) | (reg << 16) | (1 << 8))
    time.sleep(0.0002)
    for _ in range(tries):
        if b.rd(base + 0x24) & 1:
            return b.rd(base + 0x28) & 0xFFFF
    raise SystemExit(f"MDIO 응답 없음(0x{base:05X})")


def link_via_driver(b, port):
    """Serialize PHY reads with the driver's carrier polling."""
    dev_port = "1" if port == "eth1" else "2"
    if not b.bdf:
        return None
    root = os.path.join("/sys/bus/pci/devices", b.bdf, "net")
    if not os.path.isdir(root):
        return None
    iface = None
    for name in os.listdir(root):
        try:
            with open(os.path.join(root, name, "dev_port"), encoding="ascii") as f:
                if f.read().strip() == dev_port:
                    iface = name
                    break
        except FileNotFoundError:
            continue
    if iface is None:
        return None
    get_phy, get_reg = 0x8947, 0x8948   # SIOCGMIIPHY, SIOCGMIIREG
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        def read(command, addr, reg):
            request = bytearray(struct.pack("=16s4H", iface.encode(), addr, reg, 0, 0).ljust(40, b"\0"))
            fcntl.ioctl(s.fileno(), command, request, True)
            phy_addr, _, _, value = struct.unpack_from("=4H", request, 16)
            return phy_addr, value
        try:
            addr, bmsr = read(get_phy, 0, 1)
        except OSError as exc:
            if exc.errno in (errno.EOPNOTSUPP, errno.ENOTTY):
                return None
            return {"port": port, "link": "unknown", "error": str(exc)}
        try:
            _, ctl = read(get_reg, addr, 0x1F)
        except OSError as exc:
            return {"port": port, "link": "unknown", "error": str(exc)}
    return describe_phy(port, addr, bmsr, ctl)


def describe_phy(port, pa, bmsr, ctl):
    speed = 1000 if ctl & 0x40 else 100 if ctl & 0x20 else 10 if ctl & 0x10 else None
    up = bool(bmsr & 0x4)
    return {"port": port, "link": "up" if up else "down", "speed_mbps": speed if up else None,
            "duplex": ("full" if ctl & 0x8 else "half") if up else None, "autoneg_done": bool(bmsr & 0x20),
            "phy_addr": pa, "bmsr": f"0x{bmsr:04X}", "phyctl": f"0x{ctl:04X}"}


def link(b, port):
    """Read PHY link data through XDMA, with a fallback for older drivers."""
    base = MDIO_BASE[port]
    if b.dry:
        return {"port": port, "dry_run": True}
    link_data = link_via_driver(b, port)
    if link_data is not None:
        return link_data
    if not (b.rd(base + 0x20) & (1 << 8)):
        b.wr(base + 0x20, (1 << 8) | 0x18)             # MDIO 켜기, MDC 분주
        time.sleep(0.01)
    pa = next((a for a in range(32) if mdio_read(b, base, a, 2) == PHY_ID1), None)
    if pa is None:
        return {"port": port, "phy": "not found"}
    mdio_read(b, base, pa, 1)                          # BMSR 링크 비트는 끊김을 한 번 붙잡아 두므로 두 번 읽는다
    bmsr = mdio_read(b, base, pa, 1)
    ctl = mdio_read(b, base, pa, 0x1F)                 # KSZ9031 PHY Control: [6]1000 [5]100 [4]10 [3]전이중
    return describe_phy(port, pa, bmsr, ctl)


# ---- 메일박스·상태 블록(메일박스가 있는 FPGA 이미지) -------------------------------------------------------------
MBX = 0x80000                       # 공유 BRAM(호스트 BAR 창)
STATUS = MBX + 0x100
STATUS_MAGIC = 0x53455354           # 'SEST'


def has_mailbox(b):
    """메일박스가 있는 이미지인지 읽기만으로 확인한다. 이전 이미지에서는 이 주소가 다른 영역이라 쓰면 안 된다."""
    return b.rd(STATUS) == STATUS_MAGIC


def need_mailbox(b):
    if not b.dry and not has_mailbox(b):
        raise SystemExit("이 FPGA 이미지에는 PCIe 메일박스가 없습니다(상태 블록 표식 없음). 메일박스가 있는 FPGA 이미지(v3 이상)가 필요합니다.")


def rd_stable(b, off, tries=100):
    """MB 가 쓰는 워드를 두 번 같게 읽힐 때까지 읽는다. 두 포트 클럭이 달라 쓰는 순간 읽으면
    섞인 값이 나올 수 있다(BRAM 읽기·쓰기 충돌, 독립 클럭 시뮬레이션에서 확인)."""
    v = b.rd(off)
    for _ in range(tries):
        w = b.rd(off)
        if w == v:
            return v
        v = w
    raise SystemExit(f"메일박스 값이 계속 바뀝니다(0x{off:05X}).")


def status_block(b, tries=50):
    """상태 블록을 seqlock 으로 읽는다(앞뒤 seq 가 같고 짝수일 때만, seq 는 두 번 같게 읽은 값)."""
    need_mailbox(b)
    for _ in range(tries):
        s1 = rd_stable(b, STATUS + 0x08)
        w = [b.rd(STATUS + 4 * i) for i in range(0x68 // 4)]   # 형식 2(v3.4) 는 +0x60 까지, 형식 3(v3.5) 는 +0x64 기능 비트
        s2 = rd_stable(b, STATUS + 0x08)
        if s1 == s2 and not (s1 & 1):
            break
        time.sleep(0.001)
    else:
        raise SystemExit("상태 블록을 일관되게 읽지 못했습니다.")

    def mac(lo, hi):
        return ":".join("%02x" % ((lo >> s) & 0xFF) for s in (0, 8, 16, 24)) + ":" + \
            ":".join("%02x" % ((hi >> s) & 0xFF) for s in (0, 8))

    def sgn(v):
        return v - (1 << 32) if v & (1 << 31) else v
    p0, p1 = w[0x20 // 4], w[0x24 // 4]
    return {"format": w[1], "updates": w[3],
            "gptp": {"role": w[4], "as_capable": w[5], "offset_ns": sgn(w[6]), "pdelay_ns": w[7],
                     "port1": {"role": p0 & 0xFF, "as_capable": (p0 >> 8) & 1, "pdelay_ns": p0 >> 16},
                     "port2": {"role": p1 & 0xFF, "as_capable": (p1 >> 8) & 1, "pdelay_ns": p1 >> 16},
                     "rx_frames": w[10], "tx_done": w[11]},
            "clock_mac": mac(w[12], w[13]), "port1_mac": mac(w[14], w[15]), "port2_mac": mac(w[16], w[17]),
            "dna_valid": bool(w[19] >> 31), "dna": f"{w[19] & 0x1FFFFFF:07X}{w[18]:08X}",
            "fw_id": f"0x{w[20]:08X}"} | ({"resid": {"offset_ns": sgn(w[21]), "sync_dt_us": w[22], "rate_ppb": sgn(w[23]), "count": w[24]}}
                                          if w[1] >= 2 else {}) | ({"features": w[25]} if w[1] >= 3 else {})


# ---- CLI ------------------------------------------------------------------------------------------
def parse_window(text):
    g, d = text.split(":")
    return int(g, 0), int(d)


def main():
    p = argparse.ArgumentParser(prog="tsnctl", description="TSN HAT / TSN NIC PCIe 제어(예제용 축소판)")
    p.add_argument("--dev", default=None, help="보드 장치(/dev/xdmaN_user). 보드가 둘 이상이면 --dev 또는 --iface 필수")
    p.add_argument("--iface", default=None, help="보드 netdev 이름(예 eth1·ens4d1)으로 보드를 고름")
    p.add_argument("--dry-run", action="store_true", help="보드에 쓰지 않고 쓰기 순서만 출력")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    lk = sub.add_parser("link", help="포트 링크 상태·속도(PHY)")
    lk.add_argument("--port", default="all", choices=("all", "eth1", "eth2"))
    sub.add_parser("status", help="gPTP 상태·보드 MAC·DNA(메일박스 이미지)")
    s = sub.add_parser("sync")
    s.add_argument("--seconds", type=float, default=2.0)
    s.add_argument("--offset-limit", type=int, default=OFFSET_LIMIT_NS, help="synced 판정: 남는 offset(gmOffset - 주파수 보정 몫) 범위(ns)")
    t = sub.add_parser("tas")
    tsub = t.add_subparsers(dest="op", required=True)
    ts = tsub.add_parser("set")
    ts.add_argument("--port", default="eth1")
    ts.add_argument("--cycle", type=int, required=True, help="cycle us")
    ts.add_argument("--window", action="append", type=parse_window, required=True,
                    help="<게이트 마스크>:<길이 us>. 비트 n = 큐 n 열림. 예 0x02:500")
    ts.add_argument("--allow-ptp-close", action="store_true", help="gPTP 큐(0·3)를 닫는 창 허용")
    for op in ("off", "status"):
        x = tsub.add_parser(op)
        x.add_argument("--port", default="eth1")
    cb = sub.add_parser("cbs")
    csub = cb.add_subparsers(dest="op", required=True)
    cs = csub.add_parser("set")
    cs.add_argument("--port", default="eth1")
    cs.add_argument("--queue", type=int, default=1)
    cs.add_argument("--rate", type=float, required=True, help="L2 Mbps(이더넷 헤더~FCS)")
    cs.add_argument("--allow-ptp-queue", action="store_true", help="gPTP 큐(0·3)에 CBS 허용")
    co = csub.add_parser("off")
    co.add_argument("--port", default="eth1")
    g = co.add_mutually_exclusive_group(required=True)
    g.add_argument("--queue", type=int)
    g.add_argument("--all", action="store_true")
    gb = sub.add_parser("guardband")
    gb.add_argument("--port", default="eth1")
    gb.add_argument("--queue", type=int, required=True)
    gb.add_argument("--us", type=float, required=True)
    a = p.parse_args()

    b = Bar(a.dev, a.dry_run, a.iface)
    if a.cmd == "status":
        r = status_block(b)
    elif a.cmd == "info":
        r = info(b)
    elif a.cmd == "link":
        ports = ("eth1", "eth2") if a.port == "all" else (a.port,)
        r = [link(b, x) for x in ports]
    elif a.cmd == "sync":
        r = sync(b, a.seconds, a.offset_limit)
    elif a.cmd == "tas":
        if a.op == "set":
            r = tas_set(b, a.port, a.cycle, a.window, a.allow_ptp_close)
        elif a.op == "off":
            tas_off(b, a.port)
            r = {"port": a.port, "tas": "off"}
        else:
            r = tas_status(b, a.port)
    elif a.cmd == "cbs":
        if a.op == "set":
            r = cbs_set(b, a.port, a.queue, a.rate, a.allow_ptp_queue)
        else:
            qs = range(8) if a.all else [a.queue]
            for q in qs:
                cbs_off(b, a.port, q)
            r = {"port": a.port, "cbs_off_queues": list(qs)}
    elif a.cmd == "guardband":
        r = guardband(b, a.port, a.queue, a.us)
    print(json.dumps(r, ensure_ascii=False, indent=1))
    if a.dry_run:
        print("\n".join(b.log))


if __name__ == "__main__":
    main()
