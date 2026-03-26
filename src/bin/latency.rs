// #[packet] causes 'unexpected cfg' warnings, since pnet does not support cfg(clippy)
// It does not cause any issues, so we can ignore it
#![allow(unexpected_cfgs)]

use std::collections::HashMap;
use std::option::Option;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use std::vec::Vec;

use num_derive::FromPrimitive;
use num_traits::FromPrimitive;
use rand::Rng;
use signal_hook::{consts::SIGINT, iterator::Signals};

use clap::{arg, crate_authors, crate_version, value_parser, Command};

use pnet_macros::packet;
use pnet_macros_support::types::u32be;
use pnet_packet::{MutablePacket, Packet};

use pnet::datalink::{self, NetworkInterface};
use pnet::packet::ethernet::{EtherType, EthernetPacket, MutableEthernetPacket};
use pnet::util::MacAddr;
use tsn::time::tsn_time_sleep_until;

extern crate socket as soc;

const ETHERTYPE_PERF: u16 = 0x1337;
// const ETH_P_PERF: u16 = libc::ETH_P_ALL as u16; // FIXME: use ETHERTYPE_PERF
const ETH_P_PERF: u16 = ETHERTYPE_PERF;
const TIMEOUT_SEC: u64 = 1;

static mut RUNNING: bool = false;

/// Packet format for Perf tool
#[packet]
pub struct Perf {
    id: u32be,
    op: u8,
    tv_sec: u32be,
    tv_nsec: u32be,
    #[payload]
    payload: Vec<u8>,
}

#[derive(FromPrimitive)]
enum PerfOp {
    //RTT mode
    Ping = 0,
    Pong = 1,
    //One Way mode
    Tx = 2,
    Sync = 3,
}

enum TstampMode {
    Hw,
    Sw,
}

struct ServerArgs {
    interface: String,
    vlan_id: u16,
    vlan_pri: u32,
    tstamp: TstampMode,
}

struct ClientArgs {
    interface: String,
    target: MacAddr,
    vlan_id: u16,
    vlan_pri: u32,
    size: usize,
    count: usize,
    interval: u64,
    jitter: u64,
    oneway: bool,
    precise: bool,
    tstamp: TstampMode,
}

fn main() {
    let server_command = Command::new("server")
        .about("Server mode")
        .short_flag('s')
        .arg(
            arg!(-i --interface <interface> "Interface to use")
                .value_parser(value_parser!(String))
                .required(true),
        )
        .arg(
            arg!(--vlanid <id> "VLAN ID (1-4094)")
                .value_parser(value_parser!(u16).range(1..=4094))
                .required(true),
        )
        .arg(
            arg!(--pcp <prio> "VLAN priority (PCP, 0-7)")
                .value_parser(value_parser!(u32).range(0..=7))
                .default_value("0")
                .required(false),
        )
        .arg(
            arg!(--tstamp <mode> "Timestamp mode (hw, sw)")
                .value_parser(["hw", "sw"])
                .default_value("hw")
                .required(false),
        );

    let client_command = Command::new("client")
        .about("Client mode")
        .short_flag('c')
        .arg(
            arg!(-i --interface <interface> "Interface to use")
                .value_parser(value_parser!(String))
                .required(true),
        )
        .arg(
            arg!(-t --target <target> "Target MAC address")
                .value_parser(value_parser!(MacAddr))
                .required(true),
        )
        .arg(arg!(-'1' - -oneway).required(false))
        .arg(
            arg!(-s --size <size>)
                .value_parser(value_parser!(usize))
                .default_value("64")
                .required(false),
        )
        .arg(
            arg!(-c --count <count> "How many send packets")
                .value_parser(value_parser!(usize))
                .default_value("100")
                .required(false),
        )
        .arg(
            arg!(-I --interval <interval> "Interval between test packets (nanoseconds)")
                .value_parser(value_parser!(u64))
                .default_value("700000")
                .required(false),
        )
        .arg(
            arg!(-j --jitter <jitter> "Jitter for interval")
                .value_parser(value_parser!(u64))
                .default_value("0")
                .required(false),
        )
        .arg(arg!(-p --precise "Precise mode").long_help(
            "TX packets would go on every X.000000000s. Interval and Jitter will be ignored.",
        ))
        .arg(
            arg!(--vlanid <id> "VLAN ID (1-4094)")
                .value_parser(value_parser!(u16).range(1..=4094))
                .required(true),
        )
        .arg(
            arg!(--pcp <prio> "VLAN priority (PCP, 0-7)")
                .value_parser(value_parser!(u32).range(0..=7))
                .default_value("0")
                .required(false),
        )
        .arg(
            arg!(--tstamp <mode> "Timestamp mode (hw, sw)")
                .value_parser(["hw", "sw"])
                .default_value("hw")
                .required(false),
        );

    let matched_command = Command::new("latency")
        .author(crate_authors!())
        .version(crate_version!())
        .subcommand_required(true)
        .arg_required_else_help(true)
        .subcommand(server_command)
        .subcommand(client_command)
        .get_matches();

    match matched_command.subcommand() {
        Some(("server", sub_matches)) => {
            let server_args = ServerArgs {
                interface: sub_matches
                    .get_one::<String>("interface")
                    .unwrap()
                    .to_string(),
                vlan_id: *sub_matches.get_one::<u16>("vlanid").unwrap(),
                vlan_pri: *sub_matches.get_one::<u32>("pcp").unwrap(),
                tstamp: match sub_matches.get_one::<String>("tstamp").map(|s| s.as_str()) {
                    Some("sw") => TstampMode::Sw,
                    _ => TstampMode::Hw,
                },
            };

            do_server(server_args)
        }
        Some(("client", sub_matches)) => {
            let client_args = ClientArgs {
                interface: sub_matches
                    .get_one::<String>("interface")
                    .unwrap()
                    .to_string(),
                target: *sub_matches.get_one("target").unwrap(),
                vlan_id: *sub_matches.get_one::<u16>("vlanid").unwrap(),
                vlan_pri: *sub_matches.get_one::<u32>("pcp").unwrap(),
                size: *sub_matches.get_one("size").unwrap(),
                count: *sub_matches.get_one("count").unwrap(),
                interval: *sub_matches.get_one("interval").unwrap(),
                jitter: *sub_matches.get_one("jitter").unwrap(),
                oneway: sub_matches.is_present("oneway"),
                precise: sub_matches.is_present("precise"),
                tstamp: match sub_matches.get_one::<String>("tstamp").map(|s| s.as_str()) {
                    Some("sw") => TstampMode::Sw,
                    _ => TstampMode::Hw,
                },
            };

            do_client(client_args)
        }
        _ => unreachable!(),
    }
}

fn do_server(args: ServerArgs) {
    let interface_name_match = |iface: &NetworkInterface| iface.name == args.interface;
    let interfaces = datalink::interfaces();
    let interface = interfaces.into_iter().find(interface_name_match).unwrap();
    let my_mac = interface.mac.unwrap();

    let mut sock = match tsn::sock_open(&args.interface, args.vlan_id, args.vlan_pri, ETH_P_PERF) {
        Ok(sock) => sock,
        Err(e) => panic!("Failed to open TSN socket: {}", e),
    };

    if let Err(e) = sock.set_timeout(Duration::from_secs(TIMEOUT_SEC)) {
        panic!("Failed to set timeout: {}", e)
    }

    unsafe {
        RUNNING = true;
    }
    // Handle signal handler
    let mut signals = Signals::new([SIGINT]).unwrap();
    thread::spawn(move || {
        for _ in signals.forever() {
            unsafe {
                RUNNING = false;
            }
        }
    });

    let mut packet = [0u8; 1514];
    let mut iov: libc::iovec = libc::iovec {
        iov_base: packet.as_mut_ptr() as *mut libc::c_void,
        iov_len: packet.len(),
    };
    if matches!(args.tstamp, TstampMode::Hw) {
        match sock.enable_timestamps(Some(&mut iov)) {
            Ok(()) => {
                eprintln!("Socket RX timestamp enabled (HW)");
            }
            Err(e) => {
                eprintln!("Failed to set sock timestamp: {}", e);
            }
        };
    } else {
        eprintln!("Socket RX timestamp disabled (SW mode)");
    }
    // TX and SYNC may arrive out of order on the wire; pair by id when both sides are seen.
    let mut pending_tx_server_rx: HashMap<u32, SystemTime> = HashMap::new();
    let mut pending_sync_client_tx: HashMap<u32, SystemTime> = HashMap::new();
    let mut last_tx_id: u32 = 0;
    let mut last_timestamp: SystemTime = SystemTime::now();
    while unsafe { RUNNING } {
        // TODO: Cleanup this code
        let (rx_timestamp, mut eth_pkt) = match recv_perf_packet(&sock, &mut packet) {
            Some(value) => value,
            None => continue,
        };
        let mut perf_pkt = MutablePerfPacket::new(eth_pkt.payload_mut()).unwrap();

        match PerfOp::from_u8(perf_pkt.get_op()) {
            Some(PerfOp::Tx) => {
                let tx_id = perf_pkt.get_id();
                if let Some(client_tx) = pending_sync_client_tx.remove(&tx_id) {
                    print_latency(tx_id as usize, rx_timestamp, client_tx);
                } else {
                    if last_tx_id != 0 {
                        pending_tx_server_rx.insert(last_tx_id, last_timestamp);
                    }
                    last_tx_id = tx_id;
                    last_timestamp = rx_timestamp;
                }
            }
            Some(PerfOp::Sync) => {
                let sync_id = perf_pkt.get_id();
                let client_tx = UNIX_EPOCH
                    + Duration::new(perf_pkt.get_tv_sec().into(), perf_pkt.get_tv_nsec());
                if sync_id == last_tx_id {
                    print_latency(sync_id as usize, last_timestamp, client_tx);
                    last_tx_id = 0;
                } else if let Some(server_rx_tx) = pending_tx_server_rx.remove(&sync_id) {
                    print_latency(sync_id as usize, server_rx_tx, client_tx);
                } else {
                    pending_sync_client_tx.insert(sync_id, client_tx);
                }
            }
            Some(PerfOp::Ping) => {
                perf_pkt.set_op(PerfOp::Pong as u8);
                eth_pkt.set_destination(eth_pkt.get_source());
                eth_pkt.set_source(my_mac);
                if sock.send(eth_pkt.packet()).is_err() {
                    eprintln!("Failed to send packet");
                };
            }
            _ => {}
        }
    }

    if sock.close().is_err() {
        eprintln!("Failed to close socket");
    }
}

fn do_client(args: ClientArgs) {
    let interface_name_match = |iface: &NetworkInterface| iface.name == args.interface;
    let interfaces = datalink::interfaces();
    let interface = interfaces
        .into_iter()
        .find(interface_name_match)
        .unwrap_or_else(|| {
            eprintln!("Interface not found: {}", args.interface);
            std::process::exit(1);
        });
    let my_mac = interface.mac.expect("Failed to get MAC address");

    if args.precise {
        tsn::time::tsn_time_analyze();
    }

    let mut sock = match tsn::sock_open(&args.interface, args.vlan_id, args.vlan_pri, ETH_P_PERF) {
        Ok(sock) => sock,
        Err(e) => panic!("Failed to open TSN socket: {}", e),
    };

    if !args.oneway {
        if let Err(e) = sock.set_timeout(Duration::from_secs(TIMEOUT_SEC)) {
            panic!("Failed to set timeout: {}", e)
        }
    }

    unsafe {
        RUNNING = true;
    }
    // Handle signal handler
    let mut signals = Signals::new([SIGINT]).unwrap();
    thread::spawn(move || {
        for _ in signals.forever() {
            unsafe {
                RUNNING = false;
            }
        }
    });
    let mut tx_perf_buff = vec![0u8; args.size - 14];
    let mut tx_eth_buff = vec![0u8; args.size];

    let mut perf_pkt = MutablePerfPacket::new(&mut tx_perf_buff).unwrap();

    let mut eth_pkt = MutableEthernetPacket::new(&mut tx_eth_buff).unwrap();

    eth_pkt.set_destination(args.target);
    eth_pkt.set_source(my_mac);
    eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

    let mut rx_eth_buff = [0u8; 1514];
    let mut iov: libc::iovec = libc::iovec {
        iov_base: rx_eth_buff.as_mut_ptr() as *mut libc::c_void,
        iov_len: rx_eth_buff.len(),
    };
    let is_tx_ts_enabled = if matches!(args.tstamp, TstampMode::Sw) {
        eprintln!("Socket timestamps disabled (SW mode)");
        false
    } else if sock
        .enable_timestamps(if args.oneway { None } else { Some(&mut iov) })
        .is_err()
    {
        eprintln!("Failed to enable timestamps");
        false
    } else {
        eprintln!("Socket TX/RX timestamp enabled (HW)");
        true
    };
    let mut tx_ts_failures: u32 = 0;
    let mut use_sw_tx_fallback = false;
    const TX_TS_FALLBACK_THRESHOLD: u32 = 3;
    let mut timestamps: HashMap<u32 /* id */, SystemTime /* ts */> = HashMap::new();

    for ping_id in 1..=args.count {
        perf_pkt.set_id(ping_id as u32);
        let now;
        if args.oneway {
            perf_pkt.set_op(PerfOp::Tx as u8);
        } else {
            perf_pkt.set_op(PerfOp::Ping as u8);
        }
        eth_pkt.set_payload(perf_pkt.packet());
        if args.precise {
            now = SystemTime::now();
            let duration = now.duration_since(UNIX_EPOCH).unwrap();
            tsn_time_sleep_until(&Duration::new(duration.as_secs() + 1, 0))
                .expect("Failed to sleep");
        }

        if let Err(e) = sock.send(eth_pkt.packet()) {
            eprintln!("Failed to send packet: {}", e);
            continue;
        }
        let tx_timestamp = if is_tx_ts_enabled && !use_sw_tx_fallback {
            match sock.get_tx_timestamp() {
                Ok(ts) => {
                    tx_ts_failures = 0;
                    UNIX_EPOCH + Duration::new(ts.tv_sec as u64, ts.tv_nsec as u32)
                }
                Err(e) => {
                    tx_ts_failures += 1;
                    if tx_ts_failures >= TX_TS_FALLBACK_THRESHOLD {
                        use_sw_tx_fallback = true;
                        eprintln!(
                            "Failed to get TX timestamp {} times consecutively, falling back to SW timestamps",
                            tx_ts_failures
                        );
                    } else {
                        eprintln!(
                            "Failed to get TX timestamp: {} (skipping packet {})",
                            e, ping_id
                        );
                    }
                    if use_sw_tx_fallback {
                        SystemTime::now()
                    } else {
                        continue;
                    }
                }
            }
        } else {
            SystemTime::now()
        };
        if args.oneway {
            perf_pkt.set_tv_sec(tx_timestamp.duration_since(UNIX_EPOCH).unwrap().as_secs() as u32);
            perf_pkt.set_tv_nsec(
                tx_timestamp
                    .duration_since(UNIX_EPOCH)
                    .unwrap()
                    .subsec_nanos(),
            );
            perf_pkt.set_op(PerfOp::Sync as u8);

            eth_pkt.set_payload(perf_pkt.packet());
            if let Err(e) = sock.send(eth_pkt.packet()) {
                eprintln!("Failed to send packet: {}", e);
                continue;
            }

            // Must consume packet's timestamp (skip if already in SW fallback to avoid poll timeout)
            if is_tx_ts_enabled && !use_sw_tx_fallback {
                let _ = sock.get_tx_timestamp();
            }
        } else {
            timestamps.insert(ping_id as u32, tx_timestamp);
            let (rx_timestamp, rx_eth_pkt) = match recv_perf_packet(&sock, &mut rx_eth_buff) {
                Some(value) => value,
                None => continue,
            };

            let pong_pkt = PerfPacket::new(rx_eth_pkt.payload()).unwrap();
            let pong_id = pong_pkt.get_id() as usize;

            let tx_timestamp = match timestamps.remove(&(pong_id as u32)) {
                Some(ts) => ts,
                None => {
                    eprintln!("ERROR: Ping ID not found: {}", pong_id);
                    continue;
                }
            };

            print_latency(pong_id, rx_timestamp, tx_timestamp);
        }

        if !args.precise {
            let jitter = match args.jitter {
                0 => 0,
                _ => rand::thread_rng().gen_range(0..args.jitter),
            };
            let sleep_duration = Duration::from_nanos(args.interval + jitter);

            thread::sleep(sleep_duration);
        }
        if unsafe { !RUNNING } {
            break;
        }
    }

    // Wait for possible remaining packets

    let wait_start = Instant::now();
    while !timestamps.is_empty() && wait_start.elapsed().as_secs() < TIMEOUT_SEC {
        let (rx_timestamp, rx_eth_pkt) = match recv_perf_packet(&sock, &mut rx_eth_buff) {
            Some(value) => value,
            None => continue,
        };

        let pong_pkt = PerfPacket::new(rx_eth_pkt.payload()).unwrap();
        let pong_id = pong_pkt.get_id() as usize;

        let tx_timestamp = match timestamps.remove(&(pong_id as u32)) {
            Some(ts) => ts,
            None => {
                eprintln!("ERROR: Ping ID not found: {}", pong_id);
                continue;
            }
        };

        print_latency(pong_id, rx_timestamp, tx_timestamp);
    }

    if sock.close().is_err() {
        eprintln!("Failed to close socket");
    }
}

fn recv_perf_packet<'a>(
    sock: &tsn::TsnSocket,
    packet: &'a mut [u8; 1514],
) -> Option<(SystemTime, MutableEthernetPacket<'a>)> {
    let start = Instant::now();
    while start.elapsed().as_secs() < TIMEOUT_SEC {
        let mut rx_timestamp;
        let recv_bytes = {
            if sock.rx_timestamp_enabled {
                const CONTROL_SIZE: usize = 1024;
                let mut control = [0u8; CONTROL_SIZE];
                let mut iov = libc::iovec {
                    iov_base: packet.as_mut_ptr() as *mut libc::c_void,
                    iov_len: packet.len(),
                };
                let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
                msg.msg_name = std::ptr::null_mut();
                msg.msg_namelen = 0;
                msg.msg_iov = &mut iov;
                msg.msg_iovlen = 1;
                msg.msg_control = control.as_mut_ptr() as *mut libc::c_void;
                msg.msg_controllen = control.len();
                msg.msg_flags = 0;

                let res = unsafe { libc::recvmsg(sock.fd, &mut msg, 0) };
                rx_timestamp = SystemTime::now(); // Fallback to SW timestamp

                if res == -1 {
                    continue;
                } else if res == 0 {
                    continue;
                }

                if msg.msg_flags & libc::MSG_CTRUNC != 0 {
                    eprintln!("RX control data truncated; falling back to SW timestamp");
                } else if let Some(ts) = parse_rx_timestamp(&msg) {
                    rx_timestamp = ts;
                } else {
                    eprintln!("Failed to parse RX HW timestamp; falling back to SW timestamp");
                }

                res as usize
            } else {
                match sock.recv(packet) {
                    Ok(size) => {
                        rx_timestamp = SystemTime::now();
                        size as usize
                    }
                    Err(_) => {
                        continue;
                    }
                }
            }
        };

        let bytes = &packet[..recv_bytes];
        let eth = EthernetPacket::new(bytes).unwrap();
        if eth.get_ethertype() != EtherType(ETHERTYPE_PERF) {
            continue;
        }

        let eth_pkt = MutableEthernetPacket::new(&mut packet[..recv_bytes]).unwrap();
        return Some((rx_timestamp, eth_pkt));
    }

    None
}

fn parse_rx_timestamp(msg: &libc::msghdr) -> Option<SystemTime> {
    let mut cmsg = unsafe { libc::CMSG_FIRSTHDR(msg as *const _ as *mut libc::msghdr) };
    while !cmsg.is_null() {
        let cmsg_level = unsafe { (*cmsg).cmsg_level };
        let cmsg_type = unsafe { (*cmsg).cmsg_type };

        if cmsg_level == libc::SOL_SOCKET && cmsg_type == libc::SO_TIMESTAMPING {
            let ts = unsafe { *(libc::CMSG_DATA(cmsg) as *const [libc::timespec; 3]) };
            let selected = if ts[2].tv_sec != 0 || ts[2].tv_nsec != 0 {
                Some(ts[2])
            } else if ts[1].tv_sec != 0 || ts[1].tv_nsec != 0 {
                Some(ts[1])
            } else if ts[0].tv_sec != 0 || ts[0].tv_nsec != 0 {
                eprintln!("SW RX timestamp used");
                Some(ts[0])
            } else {
                None
            };

            if let Some(ts) = selected {
                if ts.tv_sec >= 0 && ts.tv_nsec >= 0 {
                    return Some(UNIX_EPOCH + Duration::new(ts.tv_sec as u64, ts.tv_nsec as u32));
                }
            }
            return None;
        }

        cmsg = unsafe { libc::CMSG_NXTHDR(msg as *const _ as *mut libc::msghdr, cmsg) };
    }
    None
}

fn print_latency(id: usize, rx_timestamp: SystemTime, tx_timestamp: SystemTime) {
    // elapsed could be negative for some reason
    let tx_ns = tx_timestamp.duration_since(UNIX_EPOCH).unwrap().as_nanos();
    let rx_ns = rx_timestamp.duration_since(UNIX_EPOCH).unwrap().as_nanos();
    let elapsed_ns = rx_ns as i128 - tx_ns as i128;
    println!(
        "{}: {}.{:09} -> {}.{:09} = {} ns",
        id,
        tx_ns / 1_000_000_000,
        tx_ns % 1_000_000_000,
        rx_ns / 1_000_000_000,
        rx_ns % 1_000_000_000,
        elapsed_ns
    );
}
