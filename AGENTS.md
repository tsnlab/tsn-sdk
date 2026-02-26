# AGENTS.md

## HW Specs

### System Clock Register Read (REG_SYS_CLOCK)
- Reading the high 32-bit register (0x0058) freezes the low 32-bit register (0x005c) until it is also read.
- This means `read64()` (high then low) is atomic by HW design; no software workaround is needed for tearing.

### TX Timestamp Registers
- TX timestamp registers likely do NOT support the freeze-on-high-read behavior.
- This means `read64()` on TX timestamp registers is susceptible to tearing when the low 32 bits overflow.
- Tearing produces ~4.3s error (2^32 ticks * 8ns/tick).

## Known Anomalies

### 1. Sysclock Carry Delay (HW Bug)
- **Symptom**: When the low 32 bits of the system clock overflow (0xFFFFFFFF → 0x00000000), the carry to the high 32 bits may be delayed.
- **Effect**: `read64()` returns a value ~4.3s (2^32 * 8ns) behind the actual time. This causes `ptp4l` to detect `clockcheck: clock jumped forward/backward`, leading to SYNCHRONIZATION_FAULT and eventually MASTER state via ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES.
- **Detection**: The rms value in ptp4l logs shows ~4,300,000,000 ns, exactly matching 2^32 ticks * 8ns.
- **Fix**: `_alinx_adjust_sysclock()` compares the raw read against `last_sysclock`. If the raw value is behind by 0x80000000–0x1_80000000 (one carry unit), it adds `1ULL << 32`.
- **Two sub-cases**:
  - Case 1: Same high part, low wrapped back. `raw=0x0900819a36, ref=0x09ffa226b1`
  - Case 2: Reference already corrected to high+1. `raw=0x09008fef67, ref=0x0a00819a36`

### 2. TX Timestamp Not Updating (HW Bug)
- **Symptom**: TX timestamp registers return stale values (either all zeros or a very old timestamp) that never get updated after packet transmission.
- **Effect**: `do_tx_work()` polls TX timestamp registers but the value never changes. After TX_TSTAMP_MAX_RETRY (500) attempts, it times out. `ptp4l` reports "timed out while polling for tx timestamp", fails to send peer delay response, and enters FAULTY state. Both ports cycle through FAULTY → LISTENING → FAULTY loop.
- **Observed on**: testbed-06, both ports (enp1s0d1, enp1s0d2), reproducible from boot.
- **Root cause**: Unknown. FPGA may not be latching TX timestamps. Needs HW/FPGA investigation.
- **SW mitigation**: `do_tx_work()` validates that the TX timestamp falls within `[tx_work_start_after, now]` before accepting it, preventing stale timestamps from being reported to `ptp4l`.

### 3. Double Precision Float in PTP (Non-issue)
- **Investigated**: Whether `double` precision in `alinx_get_timestamp()` (`ticks_scale * sys_count`) could cause the ~4.3s error.
- **Conclusion**: Not possible. At boot+400s, `sys_clock` ≈ 5×10^10. `double` has 52-bit mantissa (precision up to ~4.5×10^15), so the error at this scale is sub-nanosecond. The 4.3s error requires `sys_clock` > 2^52 (~52 days uptime) to be caused by float precision loss.

### 4. `last_sysclock` Race Condition (Potential Issue)
- **Description**: `alinx_get_sys_clock_by_xdev()` reads and writes `xdev->last_sysclock` without any lock. This function is called from multiple contexts: `ptp_gettimex` (with `ptp_data->lock`), `do_tx_work` (workqueue), `xdma_netdev_start_xmit` (TX path), and `xdma_isr` (interrupt context).
- **Risk**: Concurrent access could corrupt `last_sysclock`, causing `_alinx_adjust_sysclock()` to make incorrect corrections.
- **Status**: Not yet confirmed as a cause of observed issues. Needs investigation.

### 5. systemd-journald Sequence Number Rotation
- **Symptom**: `systemd-journald` logs "Journal file uses a different sequence number ID, rotating" early in boot.
- **Relation to PTP**: This is NOT the cause of PTP issues. Both are symptoms that appear during boot, but they are independent. The journald message is informational and benign.
