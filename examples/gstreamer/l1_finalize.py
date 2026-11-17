#!/usr/bin/env python3
"""l1_finalize.py - 같은 회차의 송신 기록(rtp_l1_tag.py --json)과 수신 기록(rtp_stats.py --l1-ext-id --json)을 대조해 L1 을 판정한다.

장비에 접근하지 않는다(파일만 읽음). 송신·수신이 모두 끝난 뒤 한쪽 보드나 PC 에서 실행한다.
판정 이름: 송신 앱 -> 수신 MAC 지연(송신 SW·커널·DMA·큐잉 포함). 순수 MAC 사이 네트워크 지연·화면 출력 지연이 아니다.

l1_judgement 가 "valid_manual_gm" 이 되려면 아래가 모두 맞아야 한다. 하나라도 어긋나거나 근거가 없으면 "not_judged" 와 이유를 낸다.
  "valid_manual_gm" = 이 회차 기록이 아래 조건을 만족하고, 두 보드의 GM 동일성은 사람이 따로 확인한 수동 근거에 조건부라는 뜻이다.
  자동 동기 검증이 아니며, 관측 표본의 통계일 뿐 전체 흐름의 합격(최소 표본 수·허용 누락률·지연 문턱)이 아니다.
  같은 회차: 두 기록이 끝까지 쓰였음(complete), 형식 L1v1, 같은 확장 ID, 같은 acq_id, 수신 기록에 송신 run 이 있음,
            그 run 의 SSRC 가 송신이 태그한 SSRC 안에 있음, SSRC 별 수신 수 <= 송신 태그 시도 수,
            수신 시각 구간이 [송신 첫 태그, 송신 마지막 태그 + --l1-max-us] 안에 있음,
            수신 원소의 송신 시각(first_tx_ns·last_tx_ns)이 [송신 첫 태그, 송신 마지막 태그] 안에 있음
            (run 은 32 비트 난수라 우연히 같을 수 있으므로 다른 실행의 원소를 시각 구간으로도 거른다. 인증은 아니다)
  장치 연결(각 호스트 안): 송신 기록의 시각 보드 PCI = --iface PCI, 시작·끝 상태를 읽은 장치 PCI = --iface PCI.
            수신 기록의 시작·끝 상태 장치 PCI = 캡처 --iface PCI. 두 호스트의 PCI 가 서로 같을 필요는 없다.
  측정: 그 run 의 유효 표본 1 개 이상(없으면 no_samples)
  시간축: 두 보드 모두 시작·끝 상태 블록이 있고, 각 시점에 Slave 포트·asCapable 1·남는 offset 절댓값 500 ns 이하,
          시작 -> 끝 사이 상태 갱신 수(updates)와 남는 offset 계수(resid.count)가 늘었음(멈춘 상태가 아님),
          수신 드라이버 rx_ts_gptp = 1(하드웨어 수신 시각이 gPTP 시각), 송신 시각 출처 = 보드 gPTP
  GM 동일성(수동 근거, --gm-evidence, 형식 L1-GM-manual-v1): 상태 블록에는 GM 식별자가 없다. 아래를 모두 갖춘 수동 확인 기록이 필요하다.
          acq_id·run(이 회차), captured_at_utc(송신 기록 시작·끝 ±--gm-max-skew-s 안), checked_by(확인한 사람),
          source.path·source.sha256(원자료 파일, 이 근거 파일 기준 상대 경로 가능, 실제 파일 SHA-256 이 같아야 함),
          sender·receiver 각각 board_pci·iface·gm_identity·domain(정수)·profile·timescale.
          sender/receiver 의 board_pci·iface 는 두 기록의 보드와 같아야 하고, gm_identity·domain·profile·timescale 은 서로 같아야 한다.
          빠진 항목·다른 domain 은 not_judged. 이 도구는 GM 을 자동으로 조회하지 않는다.
시작·끝 점검은 측정 중간의 동기 이탈을 배제하지 않는다.
결과 파일(--out)은 l1_out.py 규칙(배타 예약 '<경로>.lock', 실행별 임시 파일, 먼저 난 결과를 덮지 않는 게시)으로 쓴다.
  경로나 예약이 이미 있으면 쓰지 않는다(종료 2). 결과에 원 RX·TX·수동 근거 파일의 SHA-256 을 남긴다.
수동 근거의 문자열 항목(checked_by, acq_id, run, captured_at_utc, source.path·sha256, 양쪽 board_pci·iface·gm_identity·profile·timescale)은
  문자열이고 앞뒤 공백을 뺀 뒤 비어 있지 않아야 한다. domain 은 bool 이 아닌 정수. 근거 전체·sender·receiver·source 가 객체가 아니면 미판정.
입구 계약: 두 기록의 complete 가 True(문자열 아님)·error 없음·stage done·수신 interrupted False, 필수 칸 자료형
  (ext_id 정수, acq_id·run 문자열, counts·tagged_per_ssrc·runs 의 음 아닌 정수, status 객체)이 맞아야 뒤 판정을 한다. 틀리면 그 이유로 미판정.
  파일 SHA-256 은 읽은 바이트 기록일 뿐 내용 인증이 아니다. 판정 중 남은 형식 예외도 미판정으로 끝내고, 자기 예약은 항상 푼다.
  상태 블록 updates·resid.count 는 bool 이 아닌 음 아닌 정수인지 먼저 보고 정수끼리만 증가를 비교한다(수동 근거의 한계가 이 검사를 대신하지 않음).
  수신 기록 최상위·l1 이 객체가 아니어도 l1_max_us 를 읽기 전에 형식을 보고, 남은 구조 예외까지 같은 경계에서 RC3 미판정 JSON 으로 끝낸다.
  송신 기록 interrupted 도 False 여야 한다(칸이 없는 예전 기록은 False 로 봄).

사용: python3 l1_finalize.py rx.json tag.json --gm-evidence gm.json [--out l1.json]
"""
import argparse
import calendar
import hashlib
import json
import math
import os
import sys
import time

SLAVE_ROLE = 9
OFFSET_LIMIT_NS = 500
GM_SCHEMA = "L1-GM-manual-v1"
GM_SIDE_KEYS = ("board_pci", "iface", "gm_identity", "domain", "profile", "timescale")
LABEL = "valid_manual_gm"


def load(path):
    try:
        with open(path, "rb") as f:
            raw = f.read()
        return json.loads(raw.decode("utf-8")), hashlib.sha256(raw).hexdigest(), None
    except (OSError, ValueError) as e:
        return None, None, f"{path}: {type(e).__name__}"


def snapshot_ok(st):
    """객체가 아닌 칸(목록·문자열 등)은 확인 안 됨으로 본다(예외 0)."""
    if not isinstance(st, dict) or not isinstance(st.get("gptp"), dict):
        return False
    ports = [st["gptp"].get(k) for k in ("port1", "port2")]
    slave = any(isinstance(p, dict) and p.get("role") == SLAVE_ROLE and p.get("as_capable") == 1 for p in ports)
    resid = st.get("resid")
    off = resid.get("offset_ns") if isinstance(resid, dict) else None
    return bool(slave and isinstance(off, int) and not isinstance(off, bool) and abs(off) <= OFFSET_LIMIT_NS)


def status_counters(st):
    """상태 블록의 (updates, resid.count). 둘 다 bool 이 아닌 음 아닌 정수일 때만, 아니면 None(문자열·bool·음수·null·누락·구조 손상)."""
    if not isinstance(st, dict) or not isinstance(st.get("resid"), dict):
        return None
    u, c = st.get("updates"), st["resid"].get("count")
    return (u, c) if _nn(u) and _nn(c) else None


def progressed(a, b):
    """시작 -> 끝 사이 상태 갱신 수와 resid.count 가 모두 늘었는지. 자료형을 먼저 확인하고 정수끼리만 비교한다."""
    ca, cb = status_counters(a), status_counters(b)
    if ca is None or cb is None:
        return False
    return cb[0] > ca[0] and cb[1] > ca[1]


def utc_epoch(s):
    try:
        return calendar.timegm(time.strptime(s, "%Y-%m-%dT%H:%M:%SZ"))
    except (TypeError, ValueError):
        return None


def sval(v):
    """문자열이고 앞뒤 공백을 뺀 뒤 비어 있지 않으면 그 값, 아니면 None(목록·객체·숫자·공백 거부)."""
    return v.strip() if isinstance(v, str) and v.strip() else None


def check_gm(gm, gm_dir, tx, l1, max_skew_s):
    """수동 GM 근거 검사. (이유 목록, 원자료 SHA-256 또는 None)."""
    if gm is None:
        return ["GM·domain 동일성 수동 근거 없음(상태 블록에 GM 식별자가 없음, --gm-evidence 필요)"], None
    if not isinstance(gm, dict):
        return ["GM 근거가 객체가 아님"], None
    why = []
    if gm.get("schema") != GM_SCHEMA:
        return [f"GM 근거 형식이 {GM_SCHEMA} 아님"], None
    if sval(gm.get("acq_id")) is None or gm.get("acq_id") != tx.get("acq_id"):
        why.append("GM 근거의 acq_id 가 문자열이 아니거나 이 회차와 다름")
    if sval(gm.get("run")) is None or gm.get("run") != tx.get("run"):
        why.append("GM 근거의 run 이 문자열이 아니거나 이 회차와 다름")
    if sval(gm.get("checked_by")) is None:
        why.append("GM 근거에 확인한 사람(checked_by, 비어 있지 않은 문자열) 없음")
    t = utc_epoch(gm.get("captured_at_utc"))
    w0, w1 = utc_epoch(tx.get("wall_start_utc")), utc_epoch(tx.get("wall_end_utc"))
    if t is None or w0 is None or w1 is None or not (w0 - max_skew_s <= t <= w1 + max_skew_s):
        why.append(f"GM 근거 채취 시각이 송신 기록 시작·끝 ±{max_skew_s:.0f} s 밖이거나 없음")
    src = gm.get("source") if isinstance(gm.get("source"), dict) else {}
    src_sha = None
    if not isinstance(gm.get("source"), dict):
        why.append("GM 근거 source 가 객체가 아님")
    if sval(src.get("path")) is None or sval(src.get("sha256")) is None:
        why.append("GM 근거 원자료(source.path·sha256, 비어 있지 않은 문자열) 없음")
    else:
        p = src["path"] if os.path.isabs(src["path"]) else os.path.join(gm_dir, src["path"])
        try:
            with open(p, "rb") as f:
                src_sha = hashlib.sha256(f.read()).hexdigest()
        except (OSError, ValueError):     # ValueError = 경로에 NUL 등
            why.append("GM 근거 원자료 파일을 읽지 못함(없음·권한·잘못된 경로)")
        if src_sha is not None and src_sha != str(src["sha256"]).lower():
            why.append("GM 근거 원자료 SHA-256 이 다름")
    sides = {}
    for name in ("sender", "receiver"):
        v = gm.get(name)
        if not isinstance(v, dict):
            why.append(f"GM 근거 {name} 가 객체가 아님")
            continue
        bad = [k for k in GM_SIDE_KEYS if k != "domain" and sval(v.get(k)) is None]
        if bad:
            why.append(f"GM 근거 {name} 의 {', '.join(bad)} 가 비어 있지 않은 문자열이 아님")
            continue
        if not isinstance(v.get("domain"), int) or isinstance(v.get("domain"), bool):
            why.append(f"GM 근거 {name}.domain 이 정수가 아님")
            continue
        sides[name] = {k: (sval(v[k]) if k != "domain" else v[k]) for k in GM_SIDE_KEYS}
    if len(sides) == 2:
        snd, rcv = sides["sender"], sides["receiver"]
        for k in ("gm_identity", "domain", "profile", "timescale"):
            if snd[k] != rcv[k]:
                why.append(f"GM 근거 {k} 가 두 보드에서 다름")
        board = tx.get("board") or {}
        if snd["board_pci"] != board.get("pci") or snd["iface"] != board.get("iface"):
            why.append("GM 근거 sender 보드가 송신 기록 보드와 다름")
        rst = l1.get("status_start") or {}
        if rcv["board_pci"] != rst.get("pci") or rcv["iface"] != l1.get("rx_iface"):
            why.append("GM 근거 receiver 보드가 수신 기록 보드와 다름")
    return why, src_sha


def _nn(v, none_ok=False):
    return (none_ok and v is None) or (isinstance(v, int) and not isinstance(v, bool) and v >= 0)


def _str(v, none_ok=False):
    return (none_ok and v is None) or (isinstance(v, str) and v != "")


def record_errors(rx, tx):
    """판정 전에 두 기록의 입구 계약(완료 상태·필수 구조·자료형)을 확인. 반환 = 이유 목록(빈 목록 = 계약 맞음).
    complete 는 True 일 때만(문자열 'false' 등은 아님), error 는 None, 수신 interrupted 는 False, stage 는 done.
    송신 오류 수(send_errors)·누락은 여기서 불합격으로 보지 않는다(정수 형식만 본다, 전체 흐름 합격 기준 아님)."""
    why = []
    if not isinstance(tx, dict):
        return ["송신 기록이 객체가 아님"]
    if not isinstance(rx, dict) or not isinstance(rx.get("l1"), dict):
        return ["수신 기록 또는 그 l1 이 객체가 아님"]
    l1 = rx["l1"]
    for name, r in (("송신", tx), ("수신", l1)):
        if r.get("complete") is not True:
            why.append(f"{name} 기록 complete 가 True 아님({r.get('complete')!r})")
        if r.get("error") is not None:
            why.append(f"{name} 기록에 error 있음({str(r.get('error'))[:80]})")
        if r.get("stage") != "done":
            why.append(f"{name} 기록 stage 가 done 아님({r.get('stage')!r})")
        if not isinstance(r.get("ext_id"), int) or isinstance(r.get("ext_id"), bool):
            why.append(f"{name} 기록 ext_id 가 정수 아님")
        if not _str(r.get("acq_id")):
            why.append(f"{name} 기록 acq_id 가 문자열 아님")
        for k in ("status_start", "status_end"):
            if r.get(k) is not None and not isinstance(r.get(k), dict):
                why.append(f"{name} 기록 {k} 가 객체가 아님")
            elif isinstance(r.get(k), dict) and status_counters(r[k]) is None:   # 상태 계수 자료형
                why.append(f"{name} 기록 {k} 의 updates·resid.count 가 음 아닌 정수 아님")
    if l1.get("interrupted") is not False:
        why.append(f"수신 기록 interrupted 가 False 아님({l1.get('interrupted')!r})")
    if tx.get("interrupted", False) is not False:       # 송신 기록에도 interrupted(없던 예전 기록은 False 로 봄)
        why.append(f"송신 기록 interrupted 가 False 아님({tx.get('interrupted')!r})")
    if not _str(tx.get("run")):
        why.append("송신 기록 run 이 문자열 아님")
    if not isinstance(tx.get("board"), dict) or not all(_str(tx["board"].get(k), True) for k in ("pci", "iface", "iface_pci")):
        why.append("송신 기록 board 가 객체가 아니거나 pci·iface·iface_pci 가 문자열 아님")
    counts = tx.get("counts")
    if not isinstance(counts, dict) or not all(_nn(counts.get(k)) for k in ("received", "sent", "tagged", "send_errors")):
        why.append("송신 기록 counts(received·sent·tagged·send_errors 음 아닌 정수) 형식 아님")
    tg = tx.get("tagged_per_ssrc")
    if not isinstance(tg, dict) or not all(isinstance(k, str) and _nn(v) for k, v in tg.items()):
        why.append("송신 기록 tagged_per_ssrc 형식 아님")
    for k in ("first_tag_gptp_ns", "last_tag_gptp_ns"):
        if not _nn(tx.get(k), True):
            why.append(f"송신 기록 {k} 가 음 아닌 정수·null 아님")
    runs = l1.get("runs")
    if not isinstance(runs, dict):
        why.append("수신 기록 runs 가 객체가 아님")
    else:
        for run, per in runs.items():
            if not isinstance(per, dict):
                why.append(f"수신 기록 run {run} 이 객체가 아님")
                continue
            for ssrc, v in per.items():
                ok = isinstance(v, dict) and all(_nn(v.get(k)) for k in ("pkts", "samples", "duplicates_skipped")) \
                    and all(_nn(v.get(k), True) for k in ("first_rx_ns", "last_rx_ns", "first_tx_ns", "last_tx_ns"))
                if not ok:
                    why.append(f"수신 기록 run {run} SSRC {ssrc} 형식 아님")
    if not _str(l1.get("rx_iface")):
        why.append("수신 기록 rx_iface 가 문자열 아님")
    return why


def finalize(rx, tx, gm, max_us, gm_dir=".", gm_max_skew_s=600.0):
    why = []
    l1 = (rx or {}).get("l1") if isinstance(rx, dict) else None
    if not rx or not l1:
        return {"l1_judgement": "not_judged", "reasons": ["수신 기록에 L1 이 없음"]}
    if not tx:
        return {"l1_judgement": "not_judged", "reasons": ["송신 기록 없음"]}
    rec_why = record_errors(rx, tx)       # 입구 계약이 틀리면 뒤 판정(형식 가정)을 하지 않는다
    if rec_why:
        return {"l1_judgement": "not_judged", "reasons": rec_why, "run": tx.get("run") if isinstance(tx, dict) else None}
    # 같은 회차
    if tx.get("complete") is not True or l1.get("complete") is not True:
        why.append("기록이 끝까지 쓰이지 않음(complete 아님)")
    if tx.get("format") != "L1v1" or l1.get("format") != "L1v1":
        why.append("형식이 L1v1 아님")
    if tx.get("ext_id") != l1.get("ext_id"):
        why.append(f"확장 ID 다름(송신 {tx.get('ext_id')}, 수신 {l1.get('ext_id')})")
    if not tx.get("acq_id") or tx.get("acq_id") != l1.get("acq_id"):
        why.append(f"acq_id 다름 또는 없음(송신 {tx.get('acq_id')!r}, 수신 {l1.get('acq_id')!r})")
    run = tx.get("run")
    runs = l1.get("runs") or {}
    per = runs.get(run) if run else None
    other_runs = {k: {s: v.get("pkts") for s, v in d.items()} for k, d in runs.items() if k != run}
    if not per:
        why.append(f"수신 기록에 송신 run {run} 이 없음")
        per = {}
    tagged = tx.get("tagged_per_ssrc") or {}
    for ssrc, v in per.items():
        if ssrc not in tagged:
            why.append(f"SSRC {ssrc} 는 송신이 태그하지 않음")
        elif v["pkts"] - v["duplicates_skipped"] > tagged[ssrc]:
            why.append(f"SSRC {ssrc} 수신 {v['pkts'] - v['duplicates_skipped']} > 송신 태그 시도 {tagged[ssrc]}")
    t0, t1 = tx.get("first_tag_gptp_ns"), tx.get("last_tag_gptp_ns")
    firsts = [v["first_rx_ns"] for v in per.values() if v.get("first_rx_ns") is not None]
    lasts = [v["last_rx_ns"] for v in per.values() if v.get("last_rx_ns") is not None]
    if firsts and (t0 is None or t1 is None or min(firsts) < t0 or max(lasts) > t1 + max_us * 1000):
        why.append("수신 시각 구간이 송신 태그 구간 밖")
    for ssrc, v in per.items():
        a, b = v.get("first_tx_ns"), v.get("last_tx_ns")
        if a is None or b is None or t0 is None or t1 is None or a < t0 or b > t1:
            why.append(f"SSRC {ssrc} 원소의 송신 시각이 이번 송신 태그 구간 밖이거나 없음(다른 실행 원소 가능)")
    # 장치 연결(각 호스트 안)
    board = tx.get("board") or {}
    if not board.get("pci") or board.get("pci") != board.get("iface_pci"):
        why.append("송신 시각 보드 PCI 와 송신 --iface PCI 가 같다는 기록 없음")
    for name, st, want, ifc in (("송신 시작", tx.get("status_start"), board.get("pci"), board.get("iface")),
                                ("송신 끝", tx.get("status_end"), board.get("pci"), board.get("iface")),
                                ("수신 시작", l1.get("status_start"), None, l1.get("rx_iface")),
                                ("수신 끝", l1.get("status_end"), None, l1.get("rx_iface"))):
        st = st if isinstance(st, dict) else {}
        if not st.get("pci") or st.get("pci") != st.get("iface_pci") or st.get("iface") != ifc or (want is not None and st.get("pci") != want):
            why.append(f"{name} 상태 장치와 시각 netdev 의 PCI 연결 확인 안 됨")
    rs0, rs1 = l1.get("status_start") or {}, l1.get("status_end") or {}
    if isinstance(rs0, dict) and isinstance(rs1, dict) and rs0.get("pci") != rs1.get("pci"):
        why.append("수신 시작·끝 상태 장치가 다름")
    # 측정
    samples = sum(v["samples"] for v in per.values())
    if samples == 0:
        why.append("no_samples: 유효 지연 표본 없음")
    # 시간축
    ts, te = tx.get("status_start"), tx.get("status_end")
    rs, re_ = l1.get("status_start"), l1.get("status_end")
    for name, st in (("송신 시작", ts), ("송신 끝", te), ("수신 시작", rs), ("수신 끝", re_)):
        if not snapshot_ok(st):
            why.append(f"{name} 동기 상태 확인 안 됨")
    if not progressed(ts, te):
        why.append("송신 보드 상태 갱신·resid count 진행 확인 안 됨")
    if not progressed(rs, re_):
        why.append("수신 보드 상태 갱신·resid count 진행 확인 안 됨")
    if l1.get("rx_ts_gptp") != "1":
        why.append(f"수신 rx_ts_gptp 가 1 이 아님({l1.get('rx_ts_gptp')!r}): 수신 시각이 gPTP 시각이라는 근거 없음")
    if "gPTP" not in str(tx.get("time_source", "")):
        why.append("송신 시각 출처가 보드 gPTP 가 아님")
    gm_why, gm_src_sha = check_gm(gm, gm_dir, tx, l1, gm_max_skew_s)
    why += gm_why
    main_ssrc = max(per, key=lambda k: per[k]["samples"]) if per else None
    counts = tx.get("counts") or {}
    denom = {"tx_counts": counts, "tx_send_errors": counts.get("send_errors"), "tx_tagged_per_ssrc": tagged,
             "tx_tagged_note": "sendto 전 태그 시도 수(성공 송신 수 아님), send_errors 와 함께 본다",
             "rx_totals": {k: rx.get(k) for k in ("pkts", "hw_ts", "sw_ts", "capture_drops", "header_rejects")},
             "rx_excluded": l1.get("excluded"), "rx_header_rejects": l1.get("header_rejects"),
             "rx_other_runs_pkts": other_runs, "rx_run_per_ssrc": per}
    return {"l1_judgement": LABEL if not why else "not_judged", "reasons": why, "run": run, "acq_id": tx.get("acq_id"),
            "name": "송신 앱 -> 수신 MAC 지연(송신 SW·커널·DMA·큐잉 포함)", "main_ssrc": main_ssrc,
            "observed": per.get(main_ssrc) if main_ssrc else None, "denominators": denom, "gm_source_sha256": gm_src_sha,
            "limits": ["시작·끝 상태만 봄(중간 동기 이탈 배제 아님)", "관측 표본 통계이며 전체 흐름 합격 기준 아님",
                       "GM 동일성은 사람이 준 수동 근거(--gm-evidence)에 조건부, 자동 조회·검증 아님",
                       "run 32 비트는 인증값이 아님(우연 일치 확률 2^-32, 송신 시각 구간으로 추가 거름)",
                       "원 RX·TX·수동 근거 파일과 함께 보존해야 함(inputs 의 SHA-256)"]}


def main():
    ap = argparse.ArgumentParser(description="L1 같은 회차 판정")
    ap.add_argument("rx_json")
    ap.add_argument("tx_json")
    ap.add_argument("--gm-evidence")
    ap.add_argument("--gm-max-skew-s", type=float, default=600.0, help="GM 근거 채취 시각이 송신 기록 시작·끝에서 벗어나도 되는 초(기본 600)")
    ap.add_argument("--l1-max-us", type=float, help="기본 = 수신 기록의 값")
    ap.add_argument("--out", help="결과 파일(이미 있으면 쓰지 않음)")
    a = ap.parse_args()
    res_out = None
    if a.out:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import l1_out
        try:
            res_out = l1_out.Out(a.out, owner={"tool": "l1_finalize"})
        except l1_out.Busy as e:
            print(str(e), file=sys.stderr)
            return 2
    try:
        return _main(a, res_out)
    except KeyboardInterrupt:        # 판정 결과를 게시하지 않고 내 예약만 푼다
        print("중단됨(Ctrl+C): 결과를 쓰지 않았습니다.", file=sys.stderr)
        return 130
    finally:
        if res_out is not None:
            try:
                res_out.release()          # 내 예약일 때만 지움(남의 예약·결과는 그대로)
            except (Exception, KeyboardInterrupt) as e:    # noqa: BLE001
                res_out.release_status = l1_out.release_exception_status(res_out, e)
            rs = res_out.release_status   # 해제 실패·남은 파일은 표준오류 진단 한 줄(측정 결과·RC 는 그대로)
            if rs and not rs.get("released") and rs.get("code") != "not_held":
                print(json.dumps({"reservation_release": rs}, ensure_ascii=False), file=sys.stderr)


def max_us_bad(v, src):
    """CLI·파일 l1_max_us 공통 검사. bool 제외 숫자이고 유한한 양수가 아니면 이유 문자열(미판정), 맞으면 None."""
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or not v > 0:
        return f"{src} l1_max_us 가 유한한 양수가 아님({v!r})"
    return None


def max_us_of(a_max_us, rx):
    """판정에 쓸 l1_max_us. 파일 구조를 먼저 확인한 뒤에만 읽는다(.get 전에 객체 검사). (값, 이유 또는 None).
    CLI 값(주어졌으면, 0 포함)과 파일 값을 같은 검사(max_us_bad)로 본다. NaN·±Infinity·0·음수·bool = 이유(RC3 미판정).
    정책: CLI 를 주지 않고 수신 기록의 값이 null·누락이면 기본 1e6 us(1 s)."""
    if a_max_us is not None:
        e = max_us_bad(a_max_us, "--l1-max-us")
        return (1e6, e) if e else (a_max_us, None)
    if not isinstance(rx, dict):
        return 1e6, None                        # 뒤의 finalize/record_errors 가 수신 기록 형식 이유를 낸다
    l1 = rx.get("l1")
    if not isinstance(l1, dict):
        return 1e6, None
    v = l1.get("l1_max_us")
    if v is None:
        return 1e6, None
    e = max_us_bad(v, "수신 기록")
    return (1e6, e) if e else (v, None)


def _main(a, res_out):
    rx, rx_sha, e1 = load(a.rx_json)
    tx, tx_sha, e2 = load(a.tx_json)
    gm, gm_sha, e3 = load(a.gm_evidence) if a.gm_evidence else (None, None, None)
    e4 = None
    try:                             # 파일 구조를 가정하는 남은 경로를 모두 같은 경계 안에 둔다(RC3 미판정 JSON)
        max_us, e4 = max_us_of(a.l1_max_us, rx)
        gm_dir = os.path.dirname(os.path.abspath(a.gm_evidence)) if a.gm_evidence else "."
        res = finalize(rx, tx, gm, max_us, gm_dir, a.gm_max_skew_s)
    except Exception as e:           # noqa: BLE001 - 입력 형식 가정이 깨진 남은 경로도 예외 대신 미판정
        res = {"l1_judgement": "not_judged", "reasons": [f"판정 중 예외 {type(e).__name__}: {e}(입력 형식 확인)"]}
    if not isinstance(res.get("reasons"), list):
        res["reasons"] = []
    if e4:
        res["reasons"].append(e4)
    res["reasons"] = [x for x in (e1, e2, e3) if x] + res["reasons"]
    if res["reasons"]:
        res["l1_judgement"] = "not_judged"
    res["inputs"] = {"rx": {"path": a.rx_json, "sha256": rx_sha}, "tx": {"path": a.tx_json, "sha256": tx_sha},
                     "gm": {"path": a.gm_evidence, "sha256": gm_sha}}
    out = json.dumps(res, ensure_ascii=False, indent=1)
    print(out)
    if res_out is not None:
        pub = res_out.publish(out)
        if pub is not True:
            print(f"결과 게시 실패: {pub}(결과 파일 없음)", file=sys.stderr)
            return 3
    return 0 if res["l1_judgement"] == LABEL else 3


if __name__ == "__main__":
    sys.exit(main())
