#!/usr/bin/env python3
"""tas_cbs_demo.py - TAS 와 CBS 를 따로, 그리고 함께 켜고 수신 대역을 잰다.

송신 보드(TSN HAT)에서 root 로 실행한다. 수신 쪽에서는 먼저 `iperf3 -s` 를 실행한다.
트래픽 = iperf3 UDP(기본 1472 B payload = 1518 B 이더넷 프레임), 송신 보드 eth1 -> 수신 장치.
수신 대역은 iperf3 서버가 실제로 받은 패킷 수로 계산한다(송신 쪽 숫자가 아니다).

사용:
  sudo python3 tas_cbs_demo.py --server 192.0.2.20
  sudo python3 tas_cbs_demo.py --server 192.0.2.20 --cycle 1000 --open 500 --cbs 300 --cbs 600 --time 10

결과 표의 기대값은 아래 조건에서의 근사값이다(합격 기준이 아니다).
  조건: 회선 1 Gb/s, 큐 1 에 회선 속도 이상 UDP 를 계속 보냄, payload 한 크기, 측정 --time 초.
  - TAS 만: 창 안에서 시작할 수 있는 프레임 수 = 올림((열림 시간 - guard band + 0.45 us) / 프레임 송출 시간)
    0.45 us 는 TSN Lab 보드에서 잰 보정값이다.
  - CBS 만: 설정한 L2 대역
  - TAS + CBS: 아래 두 모델 값을 함께 보여 준다. 측정값이 어느 쪽에 가까운지로 보드 동작을 확인한다.
      모델 A = CBS 대역 x 열림 비율(게이트가 닫힌 동안 크레딧이 쌓이지 않음. TSN Lab 보드는 이쪽)
        guard band 는 모델 A 에 넣지 않는다(guard band 12.3 us 실측 149.8 / 299.6 Mbps 가 넣지 않은 150 / 300 에 맞음)
      모델 B = min(CBS 대역, TAS 만일 때 대역)(닫힌 동안에도 크레딧이 쌓임)
    tsnctl 의 cbs --rate 는 게이트가 열린 동안의 대역이다. 주기 전체 대역 B 를 원하면 B / 열림 비율(1000 이하)을 준다.
첫 쓰기 전에 읽기만으로 보드를 확인한다(write_preflight). 하나라도 맞지 않거나 확인하지 못하면 아무것도 쓰지 않고 멈춘다:
  - 고른 보드 장치의 PCIe 주소를 알 수 있음(보드가 여러 장이면 --dev 또는 --iface 로 고름)
  - 코어 레지스터가 응답함, 메일박스 상태 블록이 있음, 상태 블록 형식 3 이상
  - 펌웨어 ID 가 이 예제를 확인한 값(SUPPORTED_FW)임. 펌웨어 ID 는 펌웨어 호환 확인용이며 FPGA 이미지를 구별하지 않는다
  - 드라이버 se_mode = 1
시작할 때 링크 속도·큐 1 guard band·TAS 상태를 읽어 결과에 남긴다. 링크가 1 Gb/s 가 아니면 멈춘다.
끝나면 TAS·CBS 를 끄고 큐 1 guard band 를 시작 전 값으로 되돌린다.
"""
import argparse
import json
import math
import os
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

LINE_BPS = 1e9
HDR = 8 + 20 + 14 + 4     # UDP + IPv4 + 이더넷 헤더 + FCS
WIRE_EXTRA = 20           # 프리앰블 8 + IFG 12
GATE_OPEN_SLACK_US = 0.45 # 창이 닫히기 0.45 us 전까지 시작한 프레임은 나간다(측정값)
DATA_QUEUE = 1            # 호스트가 보내는 프레임의 하드웨어 큐
SUPPORTED_FW = {0x20261035: "v3.5"}   # 이 예제를 확인한 펌웨어 ID(호환 확인용, FPGA 이미지 식별 아님)
MIN_STATUS_FORMAT = 3


def write_preflight(b):
    """첫 쓰기 전 확인(읽기만). 통과하면 확인 내용을, 아니면 이유를 알리고 멈춘다(그때까지 보드 쓰기 0)."""
    rec = {"dev": b.dev, "pci": b.bdf}
    errs = []
    if not b.bdf:
        errs.append("보드 장치의 PCIe 주소를 확인하지 못했습니다(--dev 또는 --iface 로 보드를 고르십시오)")
    core = b.rd(0x2000)
    rec["core_version_raw"] = f"0x{core:08X}"
    if core in (0, 0xFFFFFFFF):
        errs.append("코어 레지스터가 응답하지 않습니다")
    if not tsnctl.has_mailbox(b):
        errs.append("메일박스 상태 블록이 없습니다(이 예제가 지원하지 않는 FPGA 이미지)")
    else:
        st = tsnctl.status_block(b)
        rec["status_format"], rec["fw_id"] = st["format"], st["fw_id"]
        if st["format"] < MIN_STATUS_FORMAT:
            errs.append(f"상태 블록 형식 {st['format']} (필요 {MIN_STATUS_FORMAT} 이상)")
        if int(st["fw_id"], 16) not in SUPPORTED_FW:
            errs.append(f"펌웨어 ID {st['fw_id']} 는 이 예제가 확인한 값이 아닙니다("
                        + ", ".join(f"0x{k:08X}" for k in SUPPORTED_FW) + ")")
    se = tsnctl.driver_params().get("se_mode")
    rec["se_mode"] = se
    if se != "1":
        errs.append(f"드라이버 se_mode 가 1 이 아닙니다({se!r})")
    if errs:
        sys.exit("보드 확인에 실패해 아무것도 설정하지 않고 멈춥니다:\n  - " + "\n  - ".join(errs))
    return rec


def run_iperf(server, payload, seconds, port):
    cmd = ["iperf3", "-c", server, "-p", str(port), "-u", "-l", str(payload), "-b", "1000M",
           "-t", str(seconds), "-J", "--get-server-output"]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 30)
    if out.returncode != 0:
        raise RuntimeError(f"iperf3 실패: {out.stdout[-400:]} {out.stderr[-400:]}")
    j = json.loads(out.stdout)
    # 수신(서버) 쪽 집계. 서버가 -J 로 실행되면 server_output_json, 아니면 end.sum_received 를 쓴다.
    # iperf3 3.9(Ubuntu 22.04 기본)는 sum_received 가 없고 end.sum 에 수신 쪽 lost_packets 를 싣는다.
    if "server_output_json" in j:
        srv = j["server_output_json"]["end"]["sum"]
        rx_pkts = round(srv["bytes"] / payload)
    elif "sum_received" in j["end"]:
        srv = j["end"]["sum_received"]
        rx_pkts = round(srv["bytes"] / payload)
    else:
        srv = j["end"]["sum"]
        rx_pkts = srv["packets"] - srv["lost_packets"]
    secs = srv["seconds"]
    frame = payload + HDR
    return {"rx_pkts": rx_pkts, "seconds": round(secs, 3),
            "l2_mbps": round(rx_pkts * frame * 8 / secs / 1e6, 1),
            "wire_mbps": round(rx_pkts * (frame + WIRE_EXTRA) * 8 / secs / 1e6, 1),
            "lost": srv["lost_packets"]}


def tas_only_l2(cycle_us, open_us, payload, guard_us=0.0):
    frame = payload + HDR
    t_frame_us = (frame + WIRE_EXTRA) * 8 / LINE_BPS * 1e6
    n = math.ceil(max(0.0, open_us - guard_us + GATE_OPEN_SLACK_US) / t_frame_us)
    return n * frame * 8 / (cycle_us * 1e-6) / 1e6


def main():
    p = argparse.ArgumentParser(description="TAS·CBS 동시 동작 확인")
    p.add_argument("--server", required=True, help="수신 장치 IP(iperf3 -s 실행 중)")
    p.add_argument("--port", type=int, default=5201)
    p.add_argument("--payload", type=int, default=1472, help="UDP payload 바이트(1472 이하)")
    p.add_argument("--cycle", type=int, default=1000, help="TAS cycle us")
    p.add_argument("--open", type=int, default=500, help="큐 1(호스트 트래픽) 열림 시간 us")
    p.add_argument("--cbs", type=float, action="append", help="CBS L2 Mbps(여러 번 지정 가능). 기본 300, 600")
    p.add_argument("--time", type=int, default=10, help="측정 시간 s")
    p.add_argument("--port-name", default="eth1", help="TAS·CBS 를 걸 보드 포트(eth1 또는 eth2)")
    p.add_argument("--guardband", type=float, default=0.0, help="큐 1 guard band us(기본 0). 끝나면 시작 전 값으로 되돌린다")
    p.add_argument("--json", help="결과를 이 파일에 JSON 으로 저장")
    p.add_argument("--dev", help="보드 장치(/dev/xdmaN_user). 한 PC 에 보드가 여러 장이면 --dev 또는 --iface 로 고른다")
    p.add_argument("--iface", help="보드 netdev 이름(예 eth1·ens4d1)으로 보드를 고른다")
    a = p.parse_args()
    cbs_list = a.cbs or [300.0, 600.0]
    if not 0 < a.open < a.cycle:
        sys.exit("--open 은 0 보다 크고 --cycle 보다 작아야 합니다.")

    b = tsnctl.Bar(a.dev, iface=a.iface)
    pre = write_preflight(b)
    print(f"보드 확인: {pre['dev']} (PCIe {pre['pci']}), 펌웨어 ID {pre['fw_id']}, se_mode {pre['se_mode']}")
    sy = tsnctl.sync(b, 1.0)
    ok = sy["synced"] if sy.get("synced") is not None else sy["servo_active"]   # 이전 이미지(메일박스 없음)는 servo_active
    print(f"gPTP 동기: {'예' if ok else '아니오'} (slave 포트 {sy.get('slave_port')}, offset {sy.get('offset_ns')} ns)")
    if not ok:
        print("경고: gPTP 동기가 확인되지 않습니다. TAS 시각 기준이 맞지 않을 수 있습니다.")
    port = a.port_name
    lk = tsnctl.link(b, port)
    st0 = tsnctl.tas_status(b, port, samples=1)
    gb0_us = st0["guardband_ticks"][DATA_QUEUE] * tsnctl.SYSTICK_NS / 1000
    cond = {"port": port, "queue": DATA_QUEUE, "link": lk, "guardband_us_before": gb0_us, "guardband_us": a.guardband,
            "tas_enabled_before": st0["enabled"], "cbs_enabled_before": st0["cbs_enabled"],
            "cycle_us": a.cycle, "open_us": a.open, "payload": a.payload, "time_s": a.time}
    print(f"조건: 포트 {port} 링크 {lk.get('link')} {lk.get('speed_mbps')} Mbps, 큐 {DATA_QUEUE} guard band "
          f"{gb0_us:g} -> {a.guardband:g} us, cycle {a.cycle} us, 열림 {a.open} us, payload {a.payload} B")
    if not lk.get("dry_run") and lk.get("speed_mbps") != 1000:
        sys.exit("링크가 1 Gb/s 가 아닙니다. 기대값 모델은 1 Gb/s 기준이라 멈춥니다.")

    # 창 1 = 큐 1 열림, 창 2 = 큐 1 닫힘(gPTP 큐 0·3 은 도구가 항상 연다)
    windows = [(0x02, a.open), (0x00, a.cycle - a.open)]
    open_ratio = a.open / a.cycle
    tas_l2 = tas_only_l2(a.cycle, a.open, a.payload, a.guardband)
    rows = []

    def measure(name, expect, note=""):
        r = run_iperf(a.server, a.payload, a.time, a.port)
        r.update(case=name, expect_l2_mbps=expect, note=note)
        rows.append(r)
        exp = "-" if expect is None else f"{expect:.1f}"
        dif = "" if not expect else f" ({(r['l2_mbps'] - expect) / expect * 100:+.1f} %)"
        print(f"{name:<26} 수신 L2 {r['l2_mbps']:7.1f} Mbps  (wire {r['wire_mbps']:7.1f})  근사 기대 {exp:>7}{dif}  {note}")

    def all_off():
        tsnctl.tas_off(b, port)
        for q in range(8):
            tsnctl.cbs_off(b, port, q)

    try:
        all_off()
        tsnctl.guardband(b, port, DATA_QUEUE, a.guardband)
        measure("기준(TAS·CBS 끔)", None)

        tsnctl.tas_set(b, port, a.cycle, windows)
        measure(f"TAS 만(열림 {a.open}/{a.cycle} us)", round(tas_l2, 1))
        tsnctl.tas_off(b, port)

        for cbs in cbs_list:
            tsnctl.cbs_set(b, port, 1, cbs)
            measure(f"CBS 만({cbs:g} Mbps)", cbs)
            tsnctl.tas_set(b, port, a.cycle, windows)
            ma, mb = cbs * open_ratio, min(cbs, tas_l2)
            measure(f"TAS+CBS({cbs:g} Mbps)", None, f"모델 A {ma:.1f} / 모델 B {mb:.1f}")
            tsnctl.tas_off(b, port)
            tsnctl.cbs_off(b, port, 1)
    finally:
        all_off()
        tsnctl.guardband(b, port, DATA_QUEUE, gb0_us)
        print(f"정리: TAS 끔, CBS 끔, 큐 {DATA_QUEUE} guard band {gb0_us:g} us 로 되돌림")
        print("참고: TAS·CBS 를 켠 회차의 iperf3 lost 는 셰이퍼가 줄인 대역보다 많이 보내 생기는 몫입니다(모델). 판정은 수신 L2 대역으로 합니다.")

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"cycle_us": a.cycle, "open_us": a.open, "payload": a.payload, "rows": rows, "conditions": cond,
                       "sync": sy, "preflight": pre, "path": "PCIe BAR(tsnctl)", "time": time.strftime("%Y-%m-%d %H:%M:%S")}, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
