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

const VLAN_ID_PERF: u16 = 10;
const VLAN_PRI_PERF: u32 = 3;
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

struct ClientArgs {
    interface: String,
    target: MacAddr,
    size: usize,
    count: usize,
    interval: u64,
    jitter: u64,
    oneway: bool,
    precise: bool,
}

fn main() {
    let server_command = Command::new("server")
        .about("Server mode")
        .short_flag('s')
        .arg(arg!(-i --interface <interface> "Interface to use").required(true));

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
        ));

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
            let iface = sub_matches.value_of("interface").unwrap().to_string();

            do_server(iface)
        }
        Some(("client", sub_matches)) => {
            let interface = sub_matches
                .get_one::<String>("interface")
                .unwrap()
                .to_string();
            let target = *sub_matches.get_one("target").unwrap();
            let oneway: bool = sub_matches.is_present("oneway");
            let size: usize = *sub_matches.get_one("size").unwrap();
            let count: usize = *sub_matches.get_one("count").unwrap();
            let interval = *sub_matches.get_one("interval").unwrap();
            let jitter = *sub_matches.get_one("jitter").unwrap();
            let precise = sub_matches.is_present("precise");

            let client_args = ClientArgs {
                interface,
                target,
                size,
                count,
                interval,
                jitter,
                oneway,
                precise,
            };

            do_client(client_args)
        }
        _ => unreachable!(),
    }
}

fn do_server(iface_name: String) {
    let interface_name_match = |iface: &NetworkInterface| iface.name == iface_name;
    let interfaces = datalink::interfaces();
    let interface = interfaces.into_iter().find(interface_name_match).unwrap();
    let my_mac = interface.mac.unwrap();

    let mut sock = match tsn::sock_open(&iface_name, VLAN_ID_PERF, VLAN_PRI_PERF, ETH_P_PERF) {
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
    match sock.enable_timestamps(Some(&mut iov)) {
        Ok(()) => {
            eprintln!("Socket RX timestamp enabled");
        }
        Err(e) => {
            eprintln!("Failed to set sock timestamp: {}", e);
        }
    };
    let mut timestamps: HashMap<u32 /* id */, SystemTime /* ts */> = HashMap::new();
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
                timestamps.insert(tx_id, rx_timestamp);
            }
            Some(PerfOp::Sync) => {
                let sync_id = perf_pkt.get_id();
                let rx_timestamp = match timestamps.remove(&sync_id) {
                    Some(ts) => ts,
                    None => {
                        eprintln!("ERROR: TX ID not found: {}", sync_id);
                        continue;
                    }
                };
                let tx_timestamp = UNIX_EPOCH
                    + Duration::new(perf_pkt.get_tv_sec().into(), perf_pkt.get_tv_nsec());
                print_latency(sync_id as usize, rx_timestamp, tx_timestamp);
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

    let mut sock = match tsn::sock_open(&args.interface, VLAN_ID_PERF, VLAN_PRI_PERF, ETH_P_PERF) {
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
    let is_tx_ts_enabled = {
        if sock.enable_timestamps(if args.oneway { None } else { Some(&mut iov) }).is_err() {
            eprintln!("Failed to enable timestamps");
            false
        } else {
            eprintln!("Socket TX/RX timestamp enabled");
            true
        }
    };
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
        let mut tx_timestamp = SystemTime::now();
        if is_tx_ts_enabled {
            let msg_ts = sock.get_tx_timestamp();
            match msg_ts {
                Ok(ts) => {
                    tx_timestamp = UNIX_EPOCH + Duration::new(ts.tv_sec as u64, ts.tv_nsec as u32);
                }
                Err(e) => {
                    eprintln!("Failed to get TX timestamp: {}", e);
                }
            }
        }
        if args.oneway {
            let (tv_sec, tv_nsec) = match tx_timestamp.duration_since(UNIX_EPOCH) {
                Ok(ts) => (ts.as_secs() as u32, ts.subsec_nanos()),
                Err(err) => {
                    eprintln!("TX time before UNIX epoch: {:?}", tx_timestamp);
                    continue;
                }
            };
            perf_pkt.set_tv_sec(tv_sec);
            perf_pkt.set_tv_nsec(tv_nsec);
            perf_pkt.set_op(PerfOp::Sync as u8);

            eth_pkt.set_payload(perf_pkt.packet());
            if let Err(e) = sock.send(eth_pkt.packet()) {
                eprintln!("Failed to send packet: {}", e);
                continue;
            }

            // Must consume packet's timestamp
            if is_tx_ts_enabled {
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
            match sock.msg {
                Some(mut msg) => {
                    let res = unsafe { libc::recvmsg(sock.fd, &mut msg, 0) };
                    rx_timestamp = SystemTime::now(); // Fallback default value
                    if res == -1 {
                        continue;
                    } else if res == 0 {
                        eprintln!("????");
                        continue;
                    }
                    match sock.get_rx_timestamp() {
                        Ok(ts) => rx_timestamp = UNIX_EPOCH + Duration::new(ts.tv_sec as u64, ts.tv_nsec as u32),
                        Err(_) => {
                            eprintln!("Failed to get RX timestamp");
                        }
                    }
                    res as usize
                }
                _ => match sock.recv(packet) {
                    Ok(size) => {
                        rx_timestamp = SystemTime::now();
                        size as usize
                    }
                    Err(_) => {
                        continue;
                    }
                },
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
