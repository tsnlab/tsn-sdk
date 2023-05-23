use std::collections::HashMap;
use std::io::Error;
use std::mem;
use std::option::Option;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use std::vec::Vec;

use nix::sys::socket::{cmsghdr, msghdr};
use rand::Rng;
use signal_hook::{consts::SIGINT, iterator::Signals};

use clap::{arg, crate_authors, crate_version, Command};

use pnet_macros::packet;
use pnet_macros_support::types::u32be;
use pnet_packet::{Packet, MutablePacket};

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

enum PerfOp {
    //RTT mode
    Ping = 0,
    Pong = 1,
    //One Way mode
    Tx = 2,
    Sync = 3,
}

fn main() {
    let server_command = Command::new("server")
        .about("Server mode")
        .short_flag('s')
        .arg(arg!(-i --interface <interface> "Interface to use").required(true))
        .arg(arg!(-'1' - -oneway).required(false));

    let client_command = Command::new("client")
        .about("Client mode")
        .short_flag('c')
        .arg(arg!(-i --interface <interface> "Interface to use").required(true))
        .arg(arg!(-t --target <target> "Target MAC address").required(true))
        .arg(arg!(-'1' - -oneway).required(false))
        .arg(arg!(-s --size <size>).default_value("64").required(false))
        .arg(
            arg!(-c --count <count> "How many send packets")
                .default_value("100")
                .required(false),
        )
        .arg(arg!(-p --precise "Precise mode"));

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
            let oneway = sub_matches.is_present("oneway");

            do_server(iface, oneway)
        }
        Some(("client", sub_matches)) => {
            let iface = sub_matches.value_of("interface").unwrap().to_string();
            let target = sub_matches.value_of("target").unwrap().to_string();
            let oneway: bool = sub_matches.is_present("oneway");
            let size: usize = sub_matches.value_of("size").unwrap().parse().unwrap();
            let count: usize = sub_matches.value_of("count").unwrap().parse().unwrap();
            let precise = sub_matches.is_present("precise");

            do_client(iface, target, size, count, oneway, precise)
        }
        _ => unreachable!(),
    }
}

fn do_server(iface_name: String, oneway: bool) {
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
    let msg: Option<msghdr> = match oneway {
        true => match enable_rx_timestamp(&sock, &mut iov) {
            Ok(msg) => {
                println!("Set sock timestamp");
                Some(msg)
            }
            Err(e) => {
                eprintln!("Failed to set sock timestamp: {}", e);
                None
            }
        },
        false => None,
    };
    let mut rx_map: HashMap<usize, SystemTime> = HashMap::new();
    while unsafe { RUNNING } {
        // TODO: Cleanup this code
        let recv_bytes = {
            match (oneway, msg) {
                (true, Some(mut msg)) => {
                    let res = unsafe { libc::recvmsg(sock.fd, &mut msg, 0) };
                    if res == -1 {
                        continue;
                    } else if res == 0 {
                        eprintln!("????");
                        continue;
                    }
                    res
                }
                _ => match sock.recv(&mut packet) {
                    Ok(size) => size,
                    Err(_) => {
                        continue;
                    }
                },
            }
        };
        // Match packet size
        let mut rx_packet = packet.split_at(recv_bytes as usize).0.to_owned();
        let mut eth_pkt = MutableEthernetPacket::new(&mut rx_packet).unwrap();
        if eth_pkt.get_ethertype() != EtherType(ETHERTYPE_PERF) {
            continue;
        }
        let mut perf_pkt = MutablePerfPacket::new(eth_pkt.payload_mut()).unwrap();
        if oneway {
            if perf_pkt.get_op() == PerfOp::Tx as u8 {
                // Get rx timestamp
                let rx_timestamp = match msg {
                    Some(msg) => get_timestamp(msg),
                    None => SystemTime::now(),
                };
                rx_map.insert(perf_pkt.get_id() as usize, rx_timestamp);
            } else if perf_pkt.get_op() == PerfOp::Sync as u8 {
                let rx_timestamp = rx_map.get(&(perf_pkt.get_id() as usize));
                if rx_timestamp.is_none() {
                    continue;
                }
                let rx_timestamp = rx_timestamp.unwrap();
                let tx_sec = perf_pkt.get_tv_sec();
                let tx_nsec = perf_pkt.get_tv_nsec();
                if tx_sec == 0 && tx_nsec == 0 {
                    continue;
                }
                let tx_timestamp = UNIX_EPOCH + Duration::new(tx_sec.into(), tx_nsec);
                let elapsed = rx_timestamp.duration_since(tx_timestamp).unwrap();
                let elapsed_ns = elapsed.as_nanos();
                println!(
                    "{}: {}.{:09} -> {}.{:09} = {} ns",
                    perf_pkt.get_id(),
                    tx_timestamp.duration_since(UNIX_EPOCH).unwrap().as_secs(),
                    tx_timestamp
                        .duration_since(UNIX_EPOCH)
                        .unwrap()
                        .subsec_nanos(),
                    rx_timestamp.duration_since(UNIX_EPOCH).unwrap().as_secs(),
                    rx_timestamp
                        .duration_since(UNIX_EPOCH)
                        .unwrap()
                        .subsec_nanos(),
                    elapsed_ns
                );
            }
        } else {
            println!("recv_bytes: {}", recv_bytes);
            perf_pkt.set_op(PerfOp::Pong as u8);
            eth_pkt.set_destination(eth_pkt.get_source());
            eth_pkt.set_source(my_mac);
            if sock.send(eth_pkt.packet()).is_err() {
                eprintln!("Failed to send packet");
            };
        }
    }

    if sock.close().is_err() {
        eprintln!("Failed to close socket");
    }
}

fn do_client(
    iface_name: String,
    target: String,
    size: usize,
    count: usize,
    oneway: bool,
    precise: bool,
) {
    let target: MacAddr = target.parse().unwrap();
    let interface_name_match = |iface: &NetworkInterface| iface.name == iface_name;
    let interfaces = datalink::interfaces();
    let interface = interfaces.into_iter().find(interface_name_match).unwrap();
    let my_mac = interface.mac.unwrap();

    if precise {
        tsn::time::tsn_time_analyze();
    }

    let mut sock = match tsn::sock_open(&iface_name, VLAN_ID_PERF, VLAN_PRI_PERF, ETH_P_PERF) {
        Ok(sock) => sock,
        Err(e) => panic!("Failed to open TSN socket: {}", e),
    };

    if !oneway {
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

    let mut tx_perf_buff = vec![0u8; size - 14];
    let mut tx_eth_buff = vec![0u8; size];

    let mut perf_pkt = MutablePerfPacket::new(&mut tx_perf_buff).unwrap();

    let mut eth_pkt = MutableEthernetPacket::new(&mut tx_eth_buff).unwrap();
    let mut tx_map: HashMap<usize, SystemTime> = HashMap::new();

    eth_pkt.set_destination(target);
    eth_pkt.set_source(my_mac);
    eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

    let mut rx_eth_buff = [0u8; 1514];
    let mut iov: libc::iovec = libc::iovec {
        iov_base: rx_eth_buff.as_mut_ptr() as *mut libc::c_void,
        iov_len: rx_eth_buff.len(),
    };
    let msg: Option<msghdr> = match oneway {
        true => None,
        false => match enable_rx_timestamp(&sock, &mut iov) {
            Ok(msg) => {
                eprintln!("Set sock timestamp");
                Some(msg)
            }
            Err(e) => {
                eprintln!("Failed to set sock timestamp: {}", e);
                None
            }
        },
    };

    // Loop over count
    for i in 0..count {
        perf_pkt.set_id(i as u32);
        let mut now;
        if precise {
            now = SystemTime::now();
            let duration = now.duration_since(UNIX_EPOCH).unwrap();
            tsn_time_sleep_until(&Duration::new(duration.as_secs() + 1, 0))
                .expect("Failed to sleep");
        }
        now = SystemTime::now();
        match oneway {
            true => perf_pkt.set_op(PerfOp::Tx as u8),
            false => perf_pkt.set_op(PerfOp::Ping as u8),
        };
        perf_pkt.set_tv_sec(now.duration_since(UNIX_EPOCH).unwrap().as_secs() as u32);
        perf_pkt.set_tv_nsec(now.duration_since(UNIX_EPOCH).unwrap().subsec_nanos());

        eth_pkt.set_payload(perf_pkt.packet());

        if let Err(e) = sock.send(eth_pkt.packet()) {
            eprintln!("Failed to send packet: {}", e);
            continue;
        }
        let tx_timestamp = get_tx_timestamp(sock.fd);
        tx_map.insert(i, tx_timestamp);
        if oneway {
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
        } else {
            let retry_start = Instant::now();
            let mut rx_timestamp;
            while retry_start.elapsed().as_secs() < TIMEOUT_SEC {
                match msg {
                    Some(mut msg) => {
                        if unsafe { libc::recvmsg(sock.fd, &mut msg, 0) } < 0 {
                            continue;
                        }
                        rx_timestamp = get_timestamp(msg).duration_since(UNIX_EPOCH).unwrap();
                    }
                    None => {
                        if sock.recv(&mut rx_eth_buff).is_err() {
                            continue;
                        }
                        rx_timestamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
                    }
                }

                let rx_eth_pkt = EthernetPacket::new(&rx_eth_buff).unwrap();
                if rx_eth_pkt.get_ethertype() != EtherType(ETHERTYPE_PERF) {
                    continue;
                }

                let perf_pkt = PerfPacket::new(rx_eth_pkt.payload()).unwrap();
                let id = perf_pkt.get_id() as usize;
                let tx_timestamp = tx_map.get(&id);
                if None == tx_timestamp {
                    continue;
                }
                let tx_timestamp = tx_timestamp.unwrap().duration_since(UNIX_EPOCH).unwrap();
                let elapsed = rx_timestamp.checked_sub(tx_timestamp).unwrap();
                println!(
                    "{}: {}.{:09} s",
                    id,
                    elapsed.as_secs(),
                    elapsed.subsec_nanos()
                );

                break;
            }
        }

        if !precise {
            let sleep_duration = Duration::from_millis(700)
                + Duration::from_nanos(rand::thread_rng().gen_range(0..10_000_000));

            thread::sleep(sleep_duration);
        }
        if unsafe { !RUNNING } {
            break;
        }
    }

    if sock.close().is_err() {
        eprintln!("Failed to close socket");
    }
}

fn enable_rx_timestamp(sock: &tsn::TsnSocket, iov: &mut libc::iovec) -> Result<msghdr, String> {
    // return Err("Not implemented yet".to_string());
    const CONTROLSIZE: usize = 1024;
    let mut control: [libc::c_char; CONTROLSIZE] = [0; CONTROLSIZE];

    let msg = msghdr {
        msg_iov: iov,
        msg_iovlen: 1,
        msg_control: control.as_mut_ptr() as *mut libc::c_void,
        msg_controllen: CONTROLSIZE,
        msg_flags: 0,
        msg_name: std::ptr::null_mut::<libc::c_void>(),
        msg_namelen: 0,
    };

    let sockflags: u32 = libc::SOF_TIMESTAMPING_RX_HARDWARE
        | libc::SOF_TIMESTAMPING_RAW_HARDWARE
        | libc::SOF_TIMESTAMPING_SOFTWARE;

    let res = unsafe {
        libc::setsockopt(
            sock.fd,
            libc::SOL_SOCKET,
            libc::SO_TIMESTAMPNS,
            &sockflags as *const u32 as *const libc::c_void,
            mem::size_of_val(&sockflags) as u32,
        )
    };

    if res < 0 {
        Err(format!(
            "Cannot set socket timestamp: {}",
            Error::last_os_error()
        ))
    } else {
        Ok(msg)
    }
}

fn get_tx_timestamp(fd: i32) -> SystemTime {
    SystemTime::now()
}

fn get_timestamp(msg: msghdr) -> SystemTime {
    // return Err("Not implemented yet".to_string());

    let mut tend: libc::timespec = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let mut cmsg: *mut cmsghdr;

    let mut cmsg_level;
    let mut cmsg_type;
    unsafe {
        cmsg = libc::CMSG_FIRSTHDR(&msg);
    }
    while !cmsg.is_null() {
        unsafe {
            cmsg_level = (*cmsg).cmsg_level;
            cmsg_type = (*cmsg).cmsg_type;
            if cmsg_level != libc::SOL_SOCKET {
                cmsg = libc::CMSG_NXTHDR(&msg, cmsg);
                continue;
            }
        }
        if libc::SO_TIMESTAMPNS == cmsg_type {
            unsafe {
                libc::memcpy(
                    &mut tend as *mut _ as *mut libc::c_void,
                    libc::CMSG_DATA(cmsg) as *const libc::c_void,
                    mem::size_of_val(&tend),
                );
            }
            let time = UNIX_EPOCH + Duration::new(tend.tv_sec as u64, tend.tv_nsec as u32);
            return time;
        }
    }
    SystemTime::now()
}
