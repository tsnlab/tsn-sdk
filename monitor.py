#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import threading
import queue
import time
import sys
from pathlib import Path
from typing import Optional, Deque, Tuple
from collections import deque

import tkinter as tk
from tkinter import ttk

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
from matplotlib.animation import FuncAnimation
from matplotlib.ticker import PercentFormatter

try:
    import paramiko
except ImportError:
    paramiko = None

READ_TIMEOUT = 3.0
WINDOW_SECONDS = 60.0
Y_MIN, Y_MAX = 0.0, 100.0
Y_MARGIN = 2.0

# ---------- 파일 읽기 함수 ----------
def read_local(path: Path) -> Optional[float]:
    try:
        txt = path.read_text(encoding="utf-8").strip()
        return float(txt)
    except Exception:
        return None

def read_remote(host: str, user: str, path: str, port: int = 22,
                password: Optional[str] = None, keyfile: Optional[str] = None,
                timeout: float = READ_TIMEOUT) -> Optional[float]:
    if paramiko is None:
        return None
    try:
        client = paramiko.SSHClient()
        client.load_system_host_keys()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        if keyfile:
            pkey = paramiko.RSAKey.from_private_key_file(keyfile)
            client.connect(hostname=host, port=port, username=user, pkey=pkey, timeout=timeout)
        else:
            client.connect(hostname=host, port=port, username=user, password=password, timeout=timeout)
        stdin, stdout, stderr = client.exec_command(f"cat {path}", timeout=timeout)
        out = stdout.read().decode().strip()
        client.close()
        return float(out)
    except Exception:
        return None

# ---------- 메인 앱 ----------
class App(tk.Tk):
    def __init__(self, args):
        super().__init__()
        self.title("Traffic Monitor")
        self.geometry("960x600")
        self.args = args
        self.interval = max(0.1, float(args.interval))
        self.color = args.color.lower()

        # 상단 UI
        top = ttk.Frame(self)
        top.pack(side=tk.TOP, fill=tk.X, padx=8, pady=6)
        self.path_var = tk.StringVar(value=args.path)
        ttk.Label(top, text="File:").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.path_var, width=56).pack(side=tk.LEFT, padx=6)
        self.status_var = tk.StringVar(value="Waiting…")
        ttk.Label(top, textvariable=self.status_var).pack(side=tk.RIGHT)

        # 그래프 설정
        self.fig = Figure(figsize=(5, 4), dpi=100)
        self.ax = self.fig.add_subplot(111)
        self.ax.set_xlabel("Time (s)")
        self.ax.set_ylabel("Traffic (%)")
        self.ax.grid(True)
        (self.line,) = self.ax.plot([], [], lw=2, color=self.color)

        self.ax.set_ylim(Y_MIN - Y_MARGIN, Y_MAX + Y_MARGIN)
        self.ax.yaxis.set_major_formatter(PercentFormatter(xmax=100.0))

        self.t0 = time.time()
        self.ts: Deque[float] = deque()
        self.vals: Deque[float] = deque()

        canvas = FigureCanvasTkAgg(self.fig, master=self)
        canvas.draw()
        canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        NavigationToolbar2Tk(canvas, self)

        # 워커 스레드
        self.running = True
        self.q: "queue.Queue[Tuple[float, float]]" = queue.Queue(maxsize=32)
        threading.Thread(target=self._poll_worker, daemon=True).start()

        self.ani = FuncAnimation(self.fig, self._on_update,
                                 interval=int(self.interval * 1000),
                                 blit=False)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _poll_worker(self):
        next_tick = time.time()
        while self.running:
            t = time.time() - self.t0
            if self.args.ssh_host:
                v = read_remote(
                    host=self.args.ssh_host,
                    user=self.args.ssh_user,
                    path=self.args.path,
                    port=self.args.ssh_port,
                    password=self.args.ssh_pass,
                    keyfile=self.args.ssh_key
                )
            else:
                v = read_local(Path(self.args.path))

            if v is not None:
                percent = v * 100.0
                try:
                    self.q.put((t, percent), timeout=0.2)
                    self._set_status(f"OK  last={percent:.2f}%")
                except queue.Full:
                    self._set_status("Queue full (skipped)")
            else:
                self._set_status("Read failed (retrying)")

            next_tick += self.interval
            while self.running and time.time() < next_tick:
                time.sleep(0.05)

    def _set_status(self, text: str):
        self.after(0, lambda: self.status_var.set(text))

    def _on_update(self, _frame):
        updated = False
        now_rel = time.time() - self.t0
        cutoff = now_rel - WINDOW_SECONDS

        try:
            while True:
                t, p = self.q.get_nowait()
                self.ts.append(t)
                self.vals.append(p)
                updated = True
        except queue.Empty:
            pass

        while self.ts and self.ts[0] < cutoff:
            self.ts.popleft()
            self.vals.popleft()

        if updated and self.ts:
            self.line.set_data(self.ts, self.vals)
            tmax = max(self.ts[-1], WINDOW_SECONDS)
            self.ax.set_xlim(tmax - WINDOW_SECONDS, tmax)
            self.ax.set_ylim(Y_MIN - Y_MARGIN, Y_MAX + Y_MARGIN)
        return (self.line,)

    def _on_close(self):
        self.running = False
        self.after(200, self.destroy)

# ---------- 인자 처리 ----------
def parse_args():
    p = argparse.ArgumentParser(description="최근 60초 표시, 값×100 → %, Y축 0~100%(+여백), 색상 지정 가능")
    p.add_argument("--path", required=True, help="float 한 줄이 들어있는 파일 경로")
    p.add_argument("--interval", "-i", type=float, default=1.0, help="폴링 주기(초). 기본 1.0")
    p.add_argument("--color", choices=["blue", "green", "red", "yellow", "black"],
                   default="blue", help="그래프 색상 (기본: blue)")

    p.add_argument("--ssh-host", help="원격 호스트/IP")
    p.add_argument("--ssh-port", type=int, default=22)
    p.add_argument("--ssh-user", help="SSH 사용자")
    p.add_argument("--ssh-pass", help="SSH 비밀번호")
    p.add_argument("--ssh-key", help="SSH 개인키 경로")
    args = p.parse_args()

    if args.ssh_host and not args.ssh_user:
        print("--ssh-user 가 필요합니다.", file=sys.stderr)
        sys.exit(2)
    if args.ssh_host and (args.ssh_pass is None and args.ssh_key is None):
        print("--ssh-pass 또는 --ssh-key 중 하나를 지정하세요.", file=sys.stderr)
        sys.exit(2)
    return args

if __name__ == "__main__":
    args = parse_args()
    app = App(args)
    app.mainloop()
