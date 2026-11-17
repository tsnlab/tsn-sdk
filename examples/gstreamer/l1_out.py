#!/usr/bin/env python3
"""l1_out.py - L1 세 도구(rtp_l1_tag·rtp_stats·l1_finalize)의 결과 파일 배타 예약·게시. 표준 라이브러리만.

규칙(같은 로컬 파일 시스템 안):
  1. 시작에 '<경로>.lock' 을 O_CREAT|O_EXCL 로 만든다(원자적 배타 생성). 이미 있으면 다른 실행이 예약 중이거나 죽은 실행이 남긴 예약이다.
     -> 시작하지 않는다(자동 탈취 없음). 결과 파일 '<경로>' 가 이미 있어도 시작하지 않는다(예약 뒤 한 번 더 확인).
     예약 내용(JSON)을 쓰다가 오류·Ctrl+C 가 나면, 만든 쪽(생성자)이 방금 만든 그 파일(같은 장치·inode)일 때만 지우고 예외를 다시 올린다.
  2. 내용은 실행별 임시 파일 '<경로>.<토큰>.tmp' 에 쓴다(토큰 = pid + 무작위 64 비트, 실행마다 다름).
  3. 게시 = 예약 파일이 내 것인지 확인한 뒤 os.link(임시, 경로). link 는 목적 파일이 있으면 실패하므로 먼저 완료된 결과를 덮지 않는다.
     성공하면 내 임시 파일을 지우고, 마지막에 내 예약을 지운다.
  4. 성공·처리한 실패·Ctrl+C: 자기 기록(미완이면 complete=false)을 게시한 뒤 자기 임시 파일과 자기 예약을 지운다.
     게시 자체가 실패하면 임시 파일은 남기지 않고 예약만 지운다(남의 파일·남의 예약은 건드리지 않는다).
  5. "내 예약" = 예약 파일 내용이 내 토큰을 담은 객체이고, 그 파일이 내가 만든 파일(같은 장치·inode)인 것. 아니면 게시·삭제 0.
  6. 예약 해제는 지우기 전에 예약 파일을 내 이름의 해제 파일('<경로>.lock.<토큰>.rel')로 원자적으로 옮기고(rename), 옮긴 파일이 내 것인지
     다시 확인한 뒤에만 지운다. 옮긴 파일이 남의 것이면 지우지 않고 원래 이름으로 되돌린다(link, 이미 새 예약이 있으면 해제 파일로 남김).
     그래서 확인과 지우기 사이에 예약이 바뀌어도 남의 예약 내용을 지우지 않는다. 되돌리는 동안 잠깐 예약 이름이 비는 창은 남는다.
  7. 실행 중 복구 배제 계약: 예약을 가진 실행이 끝나기 전에는 사람·다른 도구가 '<경로>.lock' 을 지우거나 바꾸지 않는다.
     정규 실행끼리는 1 번 규칙으로 서로 예약을 빼앗지 않는다. 이 계약을 어긴 외부 개입에 대해서는 '확인 뒤 게시(link)' 사이의 창을 막지 못한다.
     link 는 기존 결과를 덮지 않으므로 그 경우에도 기존 결과 덮어쓰기는 없다. 원자성을 보장하는 파일 잠금(flock 등)은 쓰지 않는다.
  8. 강제 종료(SIGKILL·전원)로 남은 예약은 자동으로 지우지 않는다. 그 실행이 끝났음을 사람이 확인한 뒤 '<경로>.lock' 을 직접 지우고
     다른 이름으로 다시 시작한다(README 복구 절차). 하드 전원 차단에서 무손실은 보장하지 않는다.
  9. 해제 결과: release() 는 self.release_status = {released, code, errno, note, leftover, retry} 를 남긴다.
     code = released·not_held·not_mine·rename_failed·claim_remove_failed·foreign_restored·foreign_kept_new_reservation·foreign_restore_failed·exception.
     해제는 한 번만 시도한다(시도 전에 held=false). 같은 객체에서 다시 부르면 파일을 건드리지 않고 처음 release_status 를 그대로 둔다
     (처음 상태가 없을 때만 not_held). 남은 파일(leftover)은 자동 재시도·강제 삭제하지 않고,
     세 도구가 종료 표준출력(rtp_stats·rtp_l1_tag) 또는 표준오류 진단 한 줄(l1_finalize)에 그대로 낸다 -> README 복구 절차로 사람이 정리.
     생성자에서 첫 fstat 이 실패하면(신원 확인 전) 빈 '<경로>.lock' 이 남을 수 있다. 신원을 모르므로 지우지 않고 예외에 진단을 붙인다.
     fdopen·내용 쓰기 실패는 신원 확인 뒤라 생성자가 정리한다(규칙 1).
 10. 해제 중 예외: release() 는 단계마다 지금 예약이 있을 수 있는 경로와 그 소유 확인 상태를 적어 둔다
     (mine = 내 토큰·같은 inode 로 확인됨, foreign = 남의 것으로 확인됨, unknown = 확인 전·확인 못 함).
     처리하지 않은 예외(KeyboardInterrupt·RuntimeError 등)로 끝나면 release_status = code exception 이고,
     candidates = [{path, owner, exists}](exists = 예외 뒤 os.path.exists), leftover = 그중 지금 있는 경로, stage = 예외가 난 단계,
     owner_certain = 남은 경로의 소유가 모두 확인됐는지. 소유를 모르면 unknown(불확실)으로 적고 지우지 않는다(자동 삭제 범위를 넓히지 않음).
     예외는 그대로 다시 올린다. 측정 기록(게시된 JSON)은 해제 결과로 다시 쓰지 않는다(해제 결과는 표준출력·표준오류에만).
     생성자 정리 실패: fdopen·쓰기 실패 뒤 내 lock 정리(stat·remove)나 fd 닫기가 실패하면, 처음 예외를 그대로 올리고
     그 예외에 후속 실패와 남은 경로를 note 로 붙인다. 정리 실패를 자동 재시도하지 않는다.
"""
import errno
import json
import os
import time


class Busy(Exception):
    """예약을 얻지 못함(다른 실행 예약 또는 결과 이미 있음)."""


def _ident(st):
    return (st.st_dev, st.st_ino)


RETRY_POLICY = "자동 재시도 없음(한 번 시도, held=false). 남은 파일은 실행이 끝났음을 확인한 뒤 README 복구 절차로 사람이 정리"


def _status(released, code, err=None, note=None, leftover=()):
    return {"released": released, "code": code, "errno": getattr(err, "errno", None) if err is not None else None,
            "error": f"{type(err).__name__}: {err}" if err is not None else None, "note": note, "leftover": list(leftover),
            "retry": RETRY_POLICY}


def release_exception_status(out, e):
    """release() 자체가 예외로 끝났을 때의 상태. release() 가 적어 둔 단계·후보 경로·소유 확인 상태를 쓴다.
    후보 경로마다 지금 있는지만 본다(읽기만, 지우지 않음). 추적 기록이 없으면 예약 경로 하나를 소유 불명으로 적는다."""
    tr = getattr(out, "release_track", None) or {"stage": "unknown", "candidates": [(out.lock, "unknown")]}
    cands = []
    for p, own in tr["candidates"]:
        try:
            ex = os.path.exists(p)
        except BaseException:             # noqa: BLE001 - 진단만, 확인 못 함
            ex = None
        cands.append({"path": p, "owner": own, "exists": ex})
    left = [c["path"] for c in cands if c["exists"] is not False]
    certain = all(c["owner"] != "unknown" for c in cands if c["exists"] is not False)
    note = f"해제 중 예외(단계 {tr['stage']}): " + (", ".join(f"{c['path']}({c['owner']}, 있음={c['exists']})" for c in cands) or "후보 없음")
    if not certain:
        note += ". 소유 확인 안 됨(불확실): 실행이 끝났음을 확인하고 내용을 본 뒤 README 복구 절차로 정리"
    st = _status(False, "exception", e, note, left)
    st.update(stage=tr["stage"], candidates=cands, owner_certain=certain)
    return st


class Out:
    def __init__(self, path, owner):
        self.path = path
        self.lock = path + ".lock"
        self.token = f"{os.getpid()}-{os.urandom(8).hex()}"
        self.tmp = f"{path}.{self.token}.tmp"
        self.held = False
        self.ident = None
        self.release_note = None
        self.release_status = None
        self.release_track = None         # 해제 단계·후보 경로·소유 확인 상태
        if os.path.exists(path):
            raise Busy(f"{path} 가 이미 있습니다. 회차마다 새 경로를 쓰십시오(이전 기록은 지우지 않음).")
        try:
            fd = os.open(self.lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            raise Busy(f"{self.lock} 예약이 있습니다(다른 실행 중이거나 강제 종료로 남은 예약). 자동으로 지우지 않습니다.") from None
        made = None
        try:                              # 여기서 난 실패의 정리 책임은 생성자에 있다
            try:
                made = _ident(os.fstat(fd))
            except BaseException as e:    # 신원 확인 전 실패 = 빈 예약이 남음(지우지 않음), 진단을 붙임
                if hasattr(e, "add_note"):
                    e.add_note(f"{self.lock} 가 남았을 수 있음(첫 fstat 실패, 신원 확인 전이라 자동 삭제 안 함). "
                               "실행이 끝났음을 확인한 뒤 README 복구 절차로 지우십시오.")
                raise
            f = os.fdopen(fd, "w", encoding="utf-8")
            fd = None
            with f:
                json.dump({"token": self.token, "pid": os.getpid(), "owner": owner, "time_ns": time.time_ns()}, f)
        except BaseException as e0:       # 처리 가능한 오류·Ctrl+C: 내가 만든 바로 그 파일만 지우고 처음 예외를 다시 올림
            def note(msg):                # 정리 실패는 처음 예외에 덧붙임(처음 예외는 바꾸지 않음)
                if hasattr(e0, "add_note"):
                    e0.add_note(msg)
            if fd is not None:
                try:
                    os.close(fd)
                except OSError as ce:
                    note(f"생성 정리: fd 닫기 실패 {type(ce).__name__}(errno {ce.errno}): {ce}")
            if made is not None:
                try:
                    if _ident(os.stat(self.lock)) == made:
                        os.remove(self.lock)
                    else:
                        note(f"생성 정리: {self.lock} 가 내가 만든 파일이 아니어서 지우지 않음")
                except FileNotFoundError:
                    pass
                except OSError as ce:
                    note(f"생성 정리: {self.lock} 정리 실패 {type(ce).__name__}(errno {ce.errno}): {ce}. "
                         f"{self.lock} 가 남았을 수 있음(내가 만든 빈 예약, 자동 재시도 없음). 실행이 끝났음을 확인한 뒤 README 복구 절차로 지우십시오.")
            raise
        self.ident = made
        self.held = True
        if os.path.exists(path):          # 예약 사이에 다른 실행이 결과를 냈으면 내 예약만 풀고 거절
            self.release()
            raise Busy(f"{path} 가 이미 있습니다.")

    def _owned(self, p):
        """p 가 내 토큰을 담은 객체이고 내가 만든 파일(같은 장치·inode)인지. 읽지 못하거나 형식이 다르면 False."""
        try:
            with open(p, encoding="utf-8") as f:
                ident = _ident(os.fstat(f.fileno()))
                d = json.load(f)
        except (OSError, ValueError):
            return False
        return isinstance(d, dict) and d.get("token") == self.token and (self.ident is None or ident == self.ident)

    def mine(self):
        """예약 파일이 내 것일 때만 True(내용 토큰 + 파일 동일성). 아니면 False = 내 것으로 보지 않음(지우지 않음)."""
        return self._owned(self.lock)

    def publish(self, text):
        """내 예약일 때만 게시. 성공 True. 실패하면 이유 문자열. 확인 뒤 link 사이의 외부 교체는 막지 못한다(규칙 7)."""
        if not self.held or not self.mine():
            return "예약이 내 것이 아님"
        try:
            with open(self.tmp, "x", encoding="utf-8") as f:
                f.write(text)
            os.link(self.tmp, self.path)
        except OSError as e:
            return f"게시 실패 {type(e).__name__}: {e}"
        finally:
            try:
                os.remove(self.tmp)
            except OSError:
                pass
        return True

    def release(self):
        """내 예약만 지운다. 원자적 이름 바꾸기 뒤 다시 확인(규칙 6). 결과는 self.release_note·release_status 에 남긴다.
        같은 객체에서 다시 부르면 파일을 건드리지 않고 처음 release_status 를 그대로 둔다(규칙 9). 처리하지 않은 예외는 규칙 10."""
        if not self.held:
            if self.release_status is None:
                self.release_status = _status(False, "not_held", note="예약을 갖고 있지 않음(해제할 것 없음)")
            return
        self.held = False                 # 해제는 한 번만 시도(RETRY_POLICY)
        try:
            self._release()
        except BaseException as e:        # 지금 후보 경로·소유 확인 상태로 진단을 남기고 그대로 다시 올림
            self.release_status = release_exception_status(self, e)
            raise

    def _track(self, stage, *cands):
        self.release_track = {"stage": stage, "candidates": list(cands)}

    def _release(self):
        lock, claim = self.lock, f"{self.lock}.{self.token}.rel"
        self._track("check_mine", (lock, "unknown"))
        if not self.mine():
            self.release_note = "예약이 내 것이 아니어서 지우지 않음"
            self.release_status = _status(False, "not_mine", note=self.release_note)
            return
        self._track("rename", (lock, "mine"), (claim, "mine"))      # 이름 바꾸기 도중: 둘 중 하나에 내 예약
        try:
            os.rename(self.lock, claim)
        except OSError as e:
            self.release_note = f"예약 해제 이름 바꾸기 실패 {type(e).__name__}"
            self.release_status = _status(False, "rename_failed", e, f"{self.lock} 가 남음(내 예약)", [self.lock])
            return
        self._track("check_claim", (claim, "unknown"))
        if self._owned(claim):
            self._track("remove_claim", (claim, "mine"))
            try:
                os.remove(claim)
            except OSError as e:
                self.release_note = f"해제 파일 삭제 실패 {type(e).__name__}: {claim} 남음"
                self.release_status = _status(False, "claim_remove_failed", e, self.release_note, [claim])
                return
            self.release_status = _status(True, "released")
            return
        self._track("restore_foreign", (claim, "foreign"), (lock, "unknown"))
        try:                              # 옮기고 보니 남의 예약: 지우지 않고 원래 이름으로 되돌림(같은 inode)
            os.link(claim, self.lock)
        except OSError as e:              # errno 로 구분(EEXIST 만 '원래 이름에 새 예약 있음')
            if isinstance(e, FileExistsError) or e.errno == errno.EEXIST:
                self.release_note = f"확인 뒤 예약이 바뀜: 남의 예약을 {claim} 로 보존(원래 이름에 새 예약 있음)"
                code = "foreign_kept_new_reservation"
            else:
                self.release_note = (f"확인 뒤 예약이 바뀜: 남의 예약을 원래 이름으로 되돌리지 못함({type(e).__name__} errno {e.errno}). "
                                     f"{claim} 로 보존, 원래 이름은 비어 있을 수 있음")
                code = "foreign_restore_failed"
            self.release_status = _status(False, code, e, self.release_note, [claim])
            return
        self._track("remove_foreign_claim", (claim, "foreign"), (lock, "foreign"))
        try:
            os.remove(claim)
            self.release_note = "확인 뒤 예약이 바뀜: 남의 예약을 되돌림"
            self.release_status = _status(False, "foreign_restored", note=self.release_note)
        except OSError as e:
            self.release_note = f"확인 뒤 예약이 바뀜: 남의 예약을 되돌렸으나 {claim} 삭제 실패"
            self.release_status = _status(False, "foreign_restored", e, self.release_note, [claim])
