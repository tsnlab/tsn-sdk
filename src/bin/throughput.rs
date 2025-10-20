// #[packet] causes 'unexpected cfg' warnings, since pnet does not support cfg(clippy)
// It does not cause any issues, so we can ignore it
#![allow(unexpected_cfgs)]

use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;
use std::time::Instant;

use clap::{arg, crate_authors, crate_version, Command};
use crossterm::event::{self, Event, KeyCode, KeyEvent};
use crossterm::terminal::{enable_raw_mode, disable_raw_mode};
use num_format::{Locale, ToFormattedString};
use signal_hook::{consts::SIGINT, iterator::Signals};

use pnet::datalink::{self, NetworkInterface};
use pnet::packet::ethernet::{EtherType, EthernetPacket, MutableEthernetPacket};
use pnet::util::MacAddr;
use pnet_macros::packet;
use pnet_macros_support::types::u32be;
use pnet_packet::Packet;
use pnet_packet::PrimitiveValues;

const NS_IN_SEC: u64 = 1_000_000_000;

const VLAN_ID_PERF: u16 = 10;
const VLAN_PRI_PERF: u32 = 3;
const ETHERTYPE_PERF: u16 = 0x1337;
const ETH_P_PERF: u16 = libc::ETH_P_ALL as u16; // FIXME: use ETHERTYPE_PERF

static mut RUNNING: bool = false;
static mut TEST_RUNNING: bool = false;

#[derive(PartialEq, Eq, PartialOrd, Ord, Clone, Copy, Debug, Hash)]
pub struct PerfOpField(pub u8);

impl PerfOpField {
    pub fn new(field_val: u8) -> PerfOpField {
        PerfOpField(field_val)
    }
}

impl PrimitiveValues for PerfOpField {
    type T = (u8,);
    fn to_primitive_values(&self) -> (u8,) {
        (self.0,)
    }
}

#[allow(non_snake_case)]
#[allow(non_upper_case_globals)]
pub mod PerfOpFieldValues {
    use super::PerfOpField;

    pub const ReqStart: PerfOpField = PerfOpField(0x00);
    pub const ReqEnd: PerfOpField = PerfOpField(0x01);
    pub const ResStart: PerfOpField = PerfOpField(0x20);
    pub const ResEnd: PerfOpField = PerfOpField(0x21);
    pub const Data: PerfOpField = PerfOpField(0x30);
    pub const ReqResult: PerfOpField = PerfOpField(0x40);
    pub const ResResult: PerfOpField = PerfOpField(0x41);
}

/// Packet format for Perf tool
#[packet]
pub struct Perf {
    id: u32be,
    #[construct_with(u8)]
    op: PerfOpField,
    #[payload]
    payload: Vec<u8>,
}

#[packet]
pub struct PerfStartReq {
    duration: u32be,
    warmup: u32be,
    #[payload]
    payload: Vec<u8>,
}

#[derive(PartialEq, Eq)]
enum WarmupState {
    None = 0,
    Ready = 1,
    WarmingUp = 2,
    Finished = 3,
}

struct Statistics {
    pkt_count: usize,
    total_bytes: usize,
    last_id: u32,
    duration: usize,
    warmup: usize,
    warmup_state: WarmupState,
}

static mut STATS: Statistics = Statistics {
    pkt_count: 0,
    total_bytes: 0,
    last_id: 0,
    duration: 0,
    warmup: 0,
    warmup_state: WarmupState::None,
};

unsafe impl Send for Statistics {}

fn main() {
    let server_command = Command::new("server")
        .about("Server mode")
        .short_flag('s')
        .arg(arg!(interface: -i --interface <interface> "interface to use").required(true));

    let client_command = Command::new("client")
        .about("Client mode")
        .short_flag('c')
        .arg(arg!(interface: -i --interface <interface> "interface to use").required(true))
        .arg(arg!(target: -t --target <target> "Target MAC address").required(false).default_value("FF:FF:FF:FF:FF:FF"))
        .arg(
            arg!(size: -p --size <size> "packet size")
                .required(false)
                .default_value("1400"),
        )
        .arg(
            arg!(duration: -d --duration <duration>)
                .required(false)
                .default_value("10"),
        )
        .arg(
            arg!(warmup: -w --warmup <warmup>)
                .required(false)
                .default_value("0"),
        )
        .arg(
            arg!(bitrate: -b --bitrate <bitrate>)
                .required(false)
                .default_value("1000000"),  // 1 Mbps
        );

    let matched_command = Command::new("throughput")
        .author(crate_authors!())
        .version(crate_version!())
        .subcommand_required(true)
        .arg_required_else_help(true)
        .subcommand(server_command)
        .subcommand(client_command)
        .get_matches();

    match matched_command.subcommand().unwrap() {
        ("server", server_matches) => {
            let iface = server_matches.value_of("interface").unwrap().to_string();
            do_server(iface)
        }
        ("client", client_matches) => {
            let iface = client_matches.value_of("interface").unwrap().to_string();
            let target = client_matches.value_of("target").unwrap().to_string();
            let size: usize = client_matches.value_of("size").unwrap().parse().unwrap();
            let duration: usize = client_matches
                .value_of("duration")
                .unwrap()
                .parse()
                .unwrap();
            let warmup: usize = client_matches
                .value_of("warmup")
                .unwrap()
                .parse()
                .unwrap();
            let bitrate: usize = client_matches
                .value_of("bitrate")
                .unwrap()
                .parse()
                .unwrap();

            do_client(iface, target, size, duration, warmup, bitrate)
        }
        _ => panic!("Invalid command"),
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

    if let Err(e) = sock.set_timeout(Duration::from_secs(1)) {
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

    while unsafe { RUNNING } {
        let mut packet = [0u8; 1514];
        let packet_size;

        match sock.recv(&mut packet) {
            Ok(n) => packet_size = n as usize,
            Err(_) => continue,
        };

        let recv_eth_pkt: EthernetPacket = EthernetPacket::new(&packet).unwrap();
        if recv_eth_pkt.get_ethertype() != EtherType(ETHERTYPE_PERF) {
            continue;
        }

        let recv_perf_pkt: PerfPacket = PerfPacket::new(recv_eth_pkt.payload()).unwrap();

        match recv_perf_pkt.get_op() {
            PerfOpFieldValues::ReqStart => {
                println!("Received ReqStart");

                if unsafe { TEST_RUNNING } {
                    println!("Already running");
                    continue;
                }

                let req_start: PerfStartReqPacket =
                    PerfStartReqPacket::new(recv_perf_pkt.payload()).unwrap();
                let duration: Duration = Duration::from_secs(req_start.get_duration().into());
                let warmup: Duration = Duration::from_secs(req_start.get_warmup().into());

                unsafe {
                    STATS.duration = duration.as_secs() as usize;
                    STATS.warmup = warmup.as_secs() as usize;
                    STATS.pkt_count = 0;
                    STATS.total_bytes = 0;
                    STATS.last_id = 0;
                    STATS.warmup_state = if STATS.warmup > 0 { WarmupState::Ready } else { WarmupState::None };
                    TEST_RUNNING = true;
                }

                // Make thread for statistics
                thread::spawn(stats_worker);

                let mut perf_buffer = vec![0; 8];
                let mut eth_buffer = vec![0; 14 + 8];

                let mut perf_pkt = MutablePerfPacket::new(&mut perf_buffer).unwrap();
                perf_pkt.set_id(recv_perf_pkt.get_id());
                perf_pkt.set_op(PerfOpFieldValues::ResStart);

                let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
                eth_pkt.set_destination(recv_eth_pkt.get_source());
                eth_pkt.set_source(my_mac);
                eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

                eth_pkt.set_payload(perf_pkt.packet());
                if let Err(e) = sock.send(eth_pkt.packet()) {
                    eprintln!("Failed to send packet: {}", e)
                }
            }
            PerfOpFieldValues::Data => {
                unsafe {
                    STATS.last_id = recv_perf_pkt.get_id();
                    if STATS.warmup_state == WarmupState::Finished || STATS.warmup_state == WarmupState::None {
                        STATS.pkt_count += 1;
                        STATS.total_bytes += packet_size + 4/* hidden VLAN tag */;
                    }
                }
            }
            PerfOpFieldValues::ReqEnd => {
                println!("Received ReqEnd");

                unsafe { TEST_RUNNING = false }

                let mut perf_buffer = vec![0; 8];
                let mut eth_buffer = vec![0; 14 + 8];

                let mut perf_pkt = MutablePerfPacket::new(&mut perf_buffer).unwrap();
                perf_pkt.set_id(recv_perf_pkt.get_id());
                perf_pkt.set_op(PerfOpFieldValues::ResEnd);

                let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
                eth_pkt.set_destination(recv_eth_pkt.get_source());
                eth_pkt.set_source(my_mac);
                eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

                eth_pkt.set_payload(perf_pkt.packet());
                if let Err(e) = sock.send(eth_pkt.packet()) {
                    eprintln!("Failed to send packet: {}", e)
                }

                // Print statistics
                unsafe {
                    let pkt_count = STATS.pkt_count;
                    let total_bytes = STATS.total_bytes;
                    let duration = STATS.duration;
                    println!(
                        "{} packets, {} bytes {} bps",
                        pkt_count,
                        total_bytes,
                        total_bytes * 8 / duration
                    );
                }
            }
            _ => {}
        }
    }

    println!("Closing socket...");
    if let Err(e) = sock.close() {
        eprintln!("Failed to close socket: {}", e);
    }
}

fn do_client(iface_name: String, target: String, size: usize, duration: usize, warmup: usize, initial_bitrate: usize) {
    let interface_name_match = |iface: &NetworkInterface| iface.name == iface_name;
    let interfaces = datalink::interfaces();
    let interface = interfaces.into_iter().find(interface_name_match).unwrap();
    let my_mac = interface.mac.unwrap();

    let target: MacAddr = target.parse().expect("Invalid MAC address");

    // Create shared bitrate variable
    let bitrate = Arc::new(Mutex::new(initial_bitrate));

    let mut sock = match tsn::sock_open(&iface_name, VLAN_ID_PERF, VLAN_PRI_PERF, ETH_P_PERF) {
        Ok(sock) => sock,
        Err(e) => panic!("Failed to open TSN socket: {}", e),
    };

    if let Err(e) = sock.set_timeout(Duration::from_secs(1)) {
        panic!("Failed to set timeout: {}", e)
    }

    // Request start
    println!("Requesting start");
    let mut req_start_buffer = vec![0; 8];
    let mut perf_buffer = vec![0; 8 + 8];
    let mut eth_buffer = vec![0; 14 + 8 + 8];

    let mut perf_req_start_pkt = MutablePerfStartReqPacket::new(&mut req_start_buffer).unwrap();
    perf_req_start_pkt.set_duration(duration.try_into().unwrap());
    perf_req_start_pkt.set_warmup(warmup.try_into().unwrap());

    let mut perf_pkt = MutablePerfPacket::new(&mut perf_buffer).unwrap();
    perf_pkt.set_id(0xdeadbeef); // TODO: Randomize
    perf_pkt.set_op(PerfOpFieldValues::ReqStart);
    perf_pkt.set_payload(perf_req_start_pkt.packet());

    let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
    eth_pkt.set_destination(target);
    eth_pkt.set_source(my_mac);
    eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));
    eth_pkt.set_payload(perf_pkt.packet());

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

    // Start keyboard input handler
    enable_raw_mode().expect("Failed to enable raw mode");
    let bitrate_for_keyboard = Arc::clone(&bitrate);
    thread::spawn(move || {
        loop {
            if let Ok(Event::Key(KeyEvent { code, .. })) = event::read() {
                match code {
                    KeyCode::Up => {
                        let mut current_bitrate = bitrate_for_keyboard.lock().unwrap();
                        *current_bitrate += 1_000_000; // 1 Mbps increase
                    }
                    KeyCode::Down => {
                        let mut current_bitrate = bitrate_for_keyboard.lock().unwrap();
                        if *current_bitrate > 100_000 { // Prevent going below 100 kbps
                            *current_bitrate -= 1_000_000; // 1 Mbps decrease
                        }
                    }
                    KeyCode::Left => {
                        let mut current_bitrate = bitrate_for_keyboard.lock().unwrap();
                        if *current_bitrate > 100_000 { // Prevent going below 100 kbps
                            *current_bitrate -= 100_000; // 100 kbps decrease
                        }
                    }
                    KeyCode::Right => {
                        let mut current_bitrate = bitrate_for_keyboard.lock().unwrap();
                        *current_bitrate += 100_000; // 100 kbps increase
                    }
                    KeyCode::Char('q') | KeyCode::Char('Q') => {
                        unsafe {
                            RUNNING = false;
                        }
                        break;
                    }
                    _ => {}
                }
            }
        }
    });

    // Send data
    disable_raw_mode().expect("Failed to disable raw mode");
    println!("Sending data");
    enable_raw_mode().expect("Failed to enable raw mode");
    let mut perf_buffer = vec![0; 8 + size];
    let mut eth_buffer = vec![0; 14 + 8 + size];
    let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
    eth_pkt.set_destination(target);
    eth_pkt.set_source(my_mac);
    eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

    let now = Instant::now();
    let mut last_id = 0;

    // Calculate packet bits (Ethernet header + Perf header + payload + VLAN tag)
    let packet_bits = (14 + 8 + size + 4) * 8;
    let mut last_send_time = Instant::now();

    // Statistics for actual throughput
    let mut bytes_sent = 0;
    let mut last_stats_time = Instant::now();

    loop {
        let current_time = Instant::now();
        let mut elapsed_ns = current_time.duration_since(last_send_time).as_nanos() as u64;

        // Get current bitrate and calculate interval
        let current_bitrate = *bitrate.lock().unwrap();
        let interval_sec = packet_bits as f64 / current_bitrate as f64;
        let interval_ns = (interval_sec * NS_IN_SEC as f64) as u64;

        // Wait for the interval to pass
        while elapsed_ns < interval_ns {
            let current_time = Instant::now();
            elapsed_ns = current_time.duration_since(last_send_time).as_nanos() as u64;
        }

        // Check if we should stop
        if now.elapsed().as_secs() >= (duration + warmup) as u64 || !unsafe { RUNNING } {
            break;
        }

        last_send_time = Instant::now();

        let mut perf_pkt = MutablePerfPacket::new(&mut perf_buffer).unwrap();
        perf_pkt.set_id(last_id); // TODO: Randomize
        perf_pkt.set_op(PerfOpFieldValues::Data);

        eth_pkt.set_payload(perf_pkt.packet());
        if sock.send(eth_pkt.packet()).is_ok() {
            bytes_sent += packet_bits / 8; // Convert bits to bytes
        }

        last_id += 1;

        // Print statistics every second
        if current_time.duration_since(last_stats_time).as_secs() >= 1 {
            let elapsed_sec = current_time.duration_since(last_stats_time).as_secs() as f64;
            let actual_bps = (bytes_sent * 8) as f64 / elapsed_sec;
            disable_raw_mode().expect("Failed to disable raw mode");
            println!("Actual throughput: {:.0} bps ({:.2} Mbps)",
                   actual_bps, actual_bps / 1_000_000.0);
            enable_raw_mode().expect("Failed to enable raw mode");

            // Reset counters
            bytes_sent = 0;
            last_stats_time = current_time;
        }
    }

    println!("Closing socket...");
    if let Err(e) = sock.close() {
        eprintln!("Failed to close socket: {}", e)
    }

    // Disable raw mode
    disable_raw_mode().expect("Failed to disable raw mode");
}

fn wait_for_response(sock: &mut tsn::TsnSocket, op: PerfOpField) -> Result<(), ()> {
    let timeout = Duration::from_millis(1000);
    let now = Instant::now();
    loop {
        if now.elapsed() > timeout {
            return Err(());
        }
        let mut packet = [0; 1514];
        if let Err(e) = sock.recv(&mut packet) {
            eprintln!("Failed to receive packet: {}", e);
            continue;
        }

        let eth_pkt: EthernetPacket = EthernetPacket::new(&packet).unwrap();
        if eth_pkt.get_ethertype() != EtherType(ETHERTYPE_PERF) {
            continue;
        }

        let perf_pkt: PerfPacket = PerfPacket::new(eth_pkt.payload()).unwrap();
        if perf_pkt.get_op() == op {
            return Ok(());
        }
    }
}

fn stats_worker() {
    let mut last_id: u32;
    let mut last_bytes = 0;
    let mut last_packets = 0;

    const SECOND: Duration = Duration::from_secs(1);

    unsafe {
        if STATS.warmup_state == WarmupState::Ready {
            STATS.warmup_state = WarmupState::WarmingUp;
            println!("Warming up");
            thread::sleep(Duration::from_secs(STATS.warmup as u64));
            println!("Finished warmup");
            STATS.warmup_state = WarmupState::Finished;
        }
        last_id = STATS.last_id;
    }

    let start_time = Instant::now();
    let mut last_time = start_time;

    while unsafe { TEST_RUNNING } {
        let elapsed = last_time.elapsed();
        if elapsed < SECOND {
            thread::sleep(SECOND - elapsed);
        }

        last_time = Instant::now();

        let (id, bytes, total_packets, duration) = unsafe {
            (STATS.last_id, STATS.total_bytes, STATS.pkt_count, STATS.duration)
        };
        let bits = (bytes - last_bytes) * 8;
        let packets = total_packets - last_packets;
        let loss_rate = 1.0 - packets as f64 / (id - last_id) as f64;

        let lap = start_time.elapsed().as_secs();

        if lap > duration as u64 {
            unsafe {
                TEST_RUNNING = false;
            }
            break;
        }
        println!(
            "{0}s: \
            {1} pps {2} bps, \
            loss: {3:.2}%",
            lap,
            packets.to_formatted_string(&Locale::en),
            bits.to_formatted_string(&Locale::en),
            loss_rate * 100.0,
        );

        last_id = id;
        last_bytes = bytes;
        last_packets = total_packets;
    }
}
