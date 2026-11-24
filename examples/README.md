# TSN application examples

Example programs for building TSN applications with a TSN HAT (on a Raspberry Pi 5) or a TSN NIC (in a PC).
All board settings are made over PCIe. No USB/UART cable is needed.

| Folder | Contents |
|---|---|
| [`tas_cbs/`](tas_cbs/README.md) | Enable TAS (time-aware shaper) and CBS (credit-based shaper) together, then check bandwidth and arrival position within the TAS cycle |
| [`gstreamer/`](gstreamer/README.md) | Send and receive a video stream with GStreamer `udpsink`/`udpsrc`, measure receive quality, and measure sender-application to receiver-MAC latency (L1) |
| [`sensor/`](sensor/README.md) | Send and receive sensor data (USB serial, vendor SDK output, simulated LiDAR) and measure one-way latency |
| `common/tsnctl.py` | Board configuration and status tool over PCIe (reduced version for these examples, see below) |

## Requirements

- Hardware: two TSN boards (TSN HAT + Raspberry Pi 5, or TSN NIC in a PC), connected through a 1 GbE switch.
  For time-synchronized examples, the switch must support IEEE 802.1AS (see "gPTP time synchronization" below).
- FPGA image: checked with FPGA image v3.5 (`"fw_id": "0x20261035"` in `tsnctl.py status`). `tas_cbs_demo.py` accepts only this fw_id.
- Operating system: Raspberry Pi OS or Ubuntu 24.04 LTS (64-bit) on the Raspberry Pi 5. Build the driver against the headers for the running kernel.
- Driver: the XDMA driver in this repository (`driver/XDMA/linux-kernel`), loaded with `tsn_mode=1` (see "Installation" below and [`README_TSN_HAT.md`](../driver/XDMA/linux-kernel/README_TSN_HAT.md)).
- Python 3.9 or later (`python3 --version`). The Python shipped with Raspberry Pi OS is sufficient.
  The Python programs use only the standard library.
- Root privileges (`sudo`) for most programs (PCIe register access, raw packet capture, hardware timestamps).

### Board configuration tool (`tsnctl.py`)

`common/tsnctl.py` configures and reads the board over PCIe. It is a reduced version of the TSN Lab board tool and contains only what these examples use.
`tas_cbs/tas_cbs_demo.py`, `gstreamer/rtp_l1_tag.py` and `sensor/sensor_tx.py` import it, and `gstreamer/rtp_stats.py` uses it for L1 latency measurement.
The example READMEs call it as `../common/tsnctl.py` from inside each example folder. Run it as root on the host where the board is installed.

| Command | Purpose |
|---|---|
| `info` | Core version, driver parameters, current board time |
| `link [--port all\|eth1\|eth2]` | Link state and speed of the board TSN ports (PHY read over MDIO) |
| `status` | gPTP status (role, asCapable, offset, pdelay), board MAC addresses, board DNA and firmware ID (FPGA images with a mailbox) |
| `sync [--seconds 2] [--offset-limit 500]` | gPTP synchronization check (`synced`); `servo_active` is for reference |
| `tas set --port eth1 --cycle <us> --window <mask>:<us> ...` | Apply a TAS gate control list (starts at the next second boundary + 2 s) |
| `tas off --port eth1` / `tas status --port eth1` | Turn TAS off / show TAS, CBS and guard band status |
| `cbs set --port eth1 --queue 1 --rate <L2 Mbps>` / `cbs off --port eth1 --queue 1` or `--all` | Set or clear CBS bandwidth |
| `guardband --port eth1 --queue 1 --us <us>` | Per-queue guard band (default 0; not cleared by `tas off` or `cbs off`, reset with `--us 0`) |

- Common options: `--dev /dev/xdmaN_user` or `--iface <netdev>` to select a board when a host has more than one, `--dry-run` to print the register writes without touching the board.
- Output is JSON. Messages and command-line help are in English.
- Not included in this version: FPGA image (flash) update, encrypted image handling, firmware command passthrough, frame counters, and FRER, Qci and FPE configuration.

The other programs (`gst_send.sh`, `gst_recv.sh`, `rtp_stats.py` without L1, `sensor_rx.py`, `rx_window_check.py`) do not use it.

## Installation

Run the following on both hosts. All commands assume the repository is cloned in your home directory (`~/tsn-sdk`); if you clone it elsewhere, replace `~/tsn-sdk` with the actual location.

1. Install the packages. `git`, `pciutils` (`lspci`), `iproute2` (`ip`) and `kmod` (`insmod`, `lsmod`) may already be installed on your image.

   ```sh
   sudo apt-get update
   sudo apt-get install -y git pciutils iproute2 kmod build-essential "linux-headers-$(uname -r)" python3
   test -f "/lib/modules/$(uname -r)/build/Makefile"
   ```

   Each example README lists its additional packages (GStreamer, iperf3).
   If the running kernel's header package is missing, follow the
   [kernel header guidance](../driver/XDMA/linux-kernel/README_TSN_HAT.md#3-드라이버-빌드와-적재).

2. On Raspberry Pi 5, check for the board before editing the boot configuration.

   ```sh
   lspci -nn -d 10ee:
   ```

   If the output includes `10ee:7021` or `10ee:7024`, continue to step 3. Ubuntu 24.04 can already detect the board without `dtparam=pciex1`.

   If neither ID appears, add `dtparam=pciex1` under the final `[all]` section of `/boot/firmware/config.txt`.
   Add the `[all]` header at the end if the file ends in another section.

   ```ini
   [all]
   dtparam=pciex1
   ```

   Save the file and reboot. Run `lspci -nn -d 10ee:` again and confirm one of the two IDs before building the driver.

3. Clone the repository, build the driver and load it.

   ```sh
   cd ~
   git clone https://github.com/tsnlab/tsn-sdk.git
   cd ~/tsn-sdk/driver/XDMA/linux-kernel/xdma
   make
   sudo insmod xdma.ko tsn_mode=1
   ```

   A manual `insmod` load lasts until reboot. After each reboot, rerun `sudo insmod xdma.ko tsn_mode=1` from `~/tsn-sdk/driver/XDMA/linux-kernel/xdma` before doing step 4.
   If you installed the driver with the SDK's [DKMS procedure](../driver/XDMA/linux-kernel/README_TSN_HAT.md#3-드라이버-빌드와-적재), it loads automatically at boot. Proceed to step 4 to verify it.

4. Check the driver. `tsn_mode` must be `1`, and the `dmesg` command must print a line.

   ```sh
   lsmod | grep xdma
   cat /sys/module/xdma/parameters/tsn_mode
   sudo dmesg | grep "netdev MAC from board DNA"
   ```

5. Find the Linux name for TSN port 1 using the
   [interface discovery step](../driver/XDMA/linux-kernel/README_TSN_HAT.md#3-드라이버-빌드와-적재).
   Substitute that name for `eth1` in the examples. Give the interfaces
   addresses on the same subnet. The sample uses `192.0.2.10` and `192.0.2.20`.

   ```sh
   iface=enp1s0d1  # use the name you found
   sudo ip addr add 192.0.2.10/24 dev "$iface"
   sudo ip link set "$iface" up
   ip link show dev "$iface"
   ```

## Working directory

- Clone this repository at the same location on both hosts (`~/tsn-sdk` in these READMEs).
- Run the commands of each example **inside that example folder**: `cd ~/tsn-sdk/examples/<example>` (for example `cd ~/tsn-sdk/examples/tas_cbs`). The board tool is then `../common/tsnctl.py`.
- Open a new terminal for each sender/receiver command and change to that folder first.

## Hardware setup

- Do not power a TSN HAT without its heat sink. Attach the heat sink to the FPGA with the power disconnected.
- The TSN HAT is powered by the Raspberry Pi 5. Connect the PCIe FFC cable as described in section 2 of the driver README.
- Plug the network cable into TSN port 1 of the board. The host interface `eth1` (on a TSN NIC, a name such as `ens4d1`) uses this port.
  `sudo python3 ../common/tsnctl.py link` shows `"link": "up"` and `"speed_mbps": 1000` for `eth1` when the cable is in the right port.
- Connect both boards to the same switch VLAN and give `eth1` an IP address in the same subnet on both hosts.
  The `192.0.2.x` addresses in the examples are documentation addresses; replace them with your own.

## gPTP time synchronization

- TAS window position, `rx_window_check.py`, sensor one-way latency and video L1 latency are only meaningful when both boards are synchronized by gPTP.
- The network needs one gPTP Grandmaster (GM). The boards synchronize to the GM through a switch that supports IEEE 802.1AS (time-aware relay).
  A switch without 802.1AS support does not forward gPTP frames (destination 01-80-C2-00-00-0E).
- A configuration with a TSN HAT acting as GM has not been tested.
- On both boards, `sudo python3 ../common/tsnctl.py sync` must report `"synced": true`.
  If `synced` is `null`, the FPGA image is older and has no mailbox (the PCIe BAR area used to exchange commands with the board firmware).
  In that case only check that `"servo_active": true`.

## Tested configurations

- Two TSN HATs through a 1 GbE 802.1AS switch, same VLAN.
- One TSN HAT and one TSN NIC, same configuration.
- The "expected results" in each README are reference values measured by TSN Lab over short runs (10 to 30 s) with a single frame size.
  They are not acceptance criteria. Values can differ with other FPGA image or driver option combinations.

## Notes

- When running an iperf3 server on a TSN HAT, keep it off CPU0: `taskset -c 1-3 iperf3 -s`.
  On the Raspberry Pi, board receive interrupts are handled on CPU0. With the iperf3 thread on CPU0, receive throughput drops to about 966 Mbps (wire rate).
- Every frame sent by the board host goes to hardware queue 1 of the board. These examples only add a VLAN PCP tag.
  Queue classification by PCP is done by an external TSN switch, which needs a PCP-to-queue mapping (see section 6 of `gstreamer/README.md`).
- `tas_cbs/tas_cbs_demo.py` checks the board, firmware and driver mode before writing anything, and stops without writing if they do not match.
  Running `tsnctl.py` directly to change TAS, CBS or guard band does not perform this check. Check `fw_id` in `status` and the driver `tsn_mode` first.
- The sensor example uses packet header version 3. By default the receiver does not accept packets from older senders (header versions 1 and 2). See section 2 of `sensor/README.md`. Use the same version on sender and receiver.
- Board queues with a lower number are sent first (queue 0 has the highest priority). This is the opposite of the IEEE 802.1Q traffic class numbering convention.
- Run only one receive measurement program (`rtp_stats.py`, `rx_window_check.py`, `sensor_rx.py`) per interface at a time.
  Each program restores the hardware receive timestamp setting when it exits.
- Program messages and command-line help are in English. Each example README documents its options, JSON fields, and exit codes.
