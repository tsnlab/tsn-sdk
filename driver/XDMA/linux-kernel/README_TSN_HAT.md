# Raspberry Pi 5용 TSN HAT 드라이버

이 드라이버는 Raspberry Pi 5에 장착한 TSN Lab TSN HAT R2용 호스트 드라이버이다.

이 드라이버는 Xilinx XDMA 레퍼런스 드라이버(GPLv2/BSD)를 기반으로 한다. 라이선스는 `LICENSE`와 `COPYING`을 참조하십시오.

TSN HAT이 다음 TSN 기능을 처리한다.

- 802.1AS 시간 동기
- 802.1Qbv 시간 인지 셰이퍼(TAS, Time-Aware Shaper)
- 802.1Qav 크레딧 기반 셰이퍼(CBS, Credit-Based Shaper)
- 802.1CB 프레임 복제·제거(FRER, Frame Replication and Elimination for Reliability)

드라이버는 Raspberry Pi와 TSN HAT 사이에서 프레임을 전달한다. 드라이버는 네트워크 인터페이스 2개를 만든다.

TSN 기능은 TSN HAT의 USB-C UART로 설정한다. 4절을 참조하십시오.

## 1. 검증 환경

| 항목 | 값 |
|---|---|
| 보드 | Raspberry Pi 5 + TSN HAT R2 (PCIe FFC 케이블) |
| 운영체제 | Raspberry Pi OS (Debian 13 trixie, 64비트) |
| 커널 | `6.12.47+rpt-rpi-2712` |

커널 버전이 다르면 그 버전의 커널 헤더로 드라이버를 다시 빌드하십시오.

미리 빌드한 `xdma.ko`는 커널 버전이 같을 때만 적재된다.

## 2. Raspberry Pi 설정

1. `/boot/firmware/config.txt`를 여십시오.
2. 다음 줄을 추가하십시오.

   ```
   dtparam=pciex1
   ```

3. Raspberry Pi를 재부팅하십시오.
4. Raspberry Pi가 TSN HAT을 인식하는지 확인하십시오.

   ```
   lspci -nn | grep 10ee
   ```

   출력에 다음 장치가 있어야 한다.

   ```
   0001:01:00.0 Ethernet controller [0200]: Xilinx Corporation Device [10ee:7024]
   ```

장치가 보이지 않으면 다음 절차를 수행하십시오.

1. Raspberry Pi의 전원을 분리하십시오.
2. FFC 케이블의 방향을 확인하십시오.
3. FFC 케이블이 양쪽 커넥터에 끝까지 들어갔는지 확인하십시오.
4. 전원을 다시 연결하십시오.

TSN HAT은 Raspberry Pi에서 전원을 받는다.

## 3. 드라이버 빌드와 적재

1. 빌드 도구와 커널 헤더를 설치하십시오.

   ```
   sudo apt install -y build-essential linux-headers-$(uname -r)
   ```

2. 소스를 받으십시오.

   ```
   git clone https://github.com/tsnlab/tsn-sdk.git
   cd tsn-sdk/driver/XDMA/linux-kernel/xdma
   ```

3. 드라이버를 빌드하십시오.

   ```
   make
   ```

4. 드라이버를 적재하십시오.

   ```
   sudo insmod xdma.ko

   기본값이 권장값이다. 파라미터는 아래 표를 참조하십시오.
   ```

5. 인터페이스 `eth1`과 `eth2`가 생겼는지 확인하십시오. `eth0`은 Raspberry Pi 내장 포트이다.
6. IP 주소를 설정하고 인터페이스를 켜십시오. 다음은 예시이다.

   ```
   sudo ip addr add 192.168.100.10/24 dev eth1
   sudo ip link set eth1 up
   ```

부팅할 때마다 드라이버를 적재하려면 `xdma.ko`를 고정 위치에 두십시오. 그 다음 systemd 서비스로 4단계와 6단계를 실행하십시오.

### 모듈 파라미터

| 파라미터 | 권장값 | 기능 |
|---|---|---|
| `se_mode` | 1 | TSN HAT 데이터 경로를 사용한다. 셰이핑, 송신 타임스탬프, FRER는 TSN HAT이 처리한다. 0으로 바꾸지 마십시오. |
| `rx_poll_mode` | 0 | 인터럽트로 수신한다. 값이 1이면 폴링으로 수신하고 처리량이 크게 떨어진다. |
| `pmac_rx_enable` | 1 | 프레임 선점(802.3br) 수신 경로를 사용한다. |
| `tx_prefetch` | 1 | 송신 디스크립터를 미리 읽는다. 작은 프레임의 처리량이 올라간다. |
| `se_tx_dest_port` | 0 | 송신 포트를 선택한다. 0 = 기본 포트(`eth1`). 0x80 = 두 포트로 송신(FRER 이중 경로). |

## 4. TSN 기능 설정 (USB-C UART)

1. TSN HAT의 USB-C 포트를 PC 또는 Raspberry Pi에 연결하십시오. 시리얼 포트 2개가 생긴다. Raspberry Pi에서는 `/dev/ttyUSB0`과 `/dev/ttyUSB1`이다.
2. 두 번째 포트(`/dev/ttyUSB1`)로 제어하십시오. 포트를 115200 baud, 데이터 8비트, 패리티 없음, 정지 1비트로 설정하십시오.

   ```
   sudo apt install -y picocom
   sudo picocom -b 115200 /dev/ttyUSB1
   ```

3. `help`를 입력하십시오. 명령 목록이 나온다.

| 명령 | 기능 |
|---|---|
| `ver` | 펌웨어와 코어 버전을 표시한다. |
| `ptp` | 802.1AS 상태(role, offset_ns, asCapable, pdelay_ns)를 표시한다. |
| `tas set <cycle_us> <gate_hex> <dur_us> ...` | 802.1Qbv 게이트 제어 목록을 설정한다. |
| `tas on` / `tas off` / `tas status` | TAS를 시작한다 / 정지한다 / 상태를 표시한다. |
| `cbs on <tc> <slope_hex>` / `cbs off <tc>` / `cbs status` | 802.1Qav CBS를 설정한다 / 정지한다 / 상태를 표시한다. |
| `frer ...` | 802.1CB 스트림 복제·제거를 설정한다. 문법은 `help`로 확인하십시오. |
| `cnt` | `eth1` 큐별 송신 초과(overrun) 카운터를 표시한다. |

### TAS 예시

이 예시는 주기 1 ms를 사용한다. 앞 500 us 동안 데이터 큐가 열린다. 뒤 500 us 동안 데이터 큐가 닫힌다.

```
tas set 1000 0x0a 500 0x08 500
tas on
```

- `gate_hex`의 비트 i가 큐 i를 연다.
- Raspberry Pi에서 보낸 프레임은 큐 1을 사용한다.
- 802.1AS(gPTP) 프레임은 큐 3을 사용한다.
- 따라서 `0x0a`는 데이터 큐와 gPTP 큐를 연다. `0x08`은 gPTP 큐만 연다.
- 큐 3(비트 3)은 항상 열어 두십시오. 큐 3이 닫히면 시간 동기가 끊긴다.
- `dur_us` 값의 합은 `cycle_us`와 같아야 한다.
- 동작 중에 스케줄을 바꾸려면 새 `tas set`을 보내십시오. 그 다음 `tas on`을 다시 보내십시오.
- TAS와 CBS 명령은 `eth1`에 적용된다.

## 5. 알려진 제한

- Raspberry Pi에서 보낸 모든 프레임은 큐 1 하나로 들어간다. 이 버전은 트래픽 종류별로 다른 큐를 사용할 수 없다.
- 스위치 없이 TSN HAT 두 대를 직접 연결한 구성의 802.1AS 동기는 공식 검증 전이다. `ptp` 명령으로 동기 상태를 확인하십시오.
- 이 문서는 Raspberry Pi 5 구성만 다룬다. x86 PC용 PCIe NIC 구성 안내는 별도로 제공한다.
- 이전 TSNv3 드라이버는 `tsnv3` 브랜치에 보존한다.
