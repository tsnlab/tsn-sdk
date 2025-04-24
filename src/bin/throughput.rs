use std::thread;
use std::time::Duration;
use std::time::Instant;

use clap::{arg, crate_authors, crate_version, Command};
use num_format::{Locale, ToFormattedString};
use signal_hook::{consts::SIGINT, iterator::Signals};

use pnet::datalink::{self, NetworkInterface};
use pnet::packet::ethernet::{EtherType, EthernetPacket, MutableEthernetPacket};
use pnet::util::MacAddr;
use pnet_macros::packet;
use pnet_macros_support::types::u32be;
use pnet_packet::Packet;
use pnet_packet::PrimitiveValues;

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

struct Statistics {
    pkt_count: usize,
    total_bytes: usize,
    last_id: u32,
    duration: usize,
    warmup: usize,
    warming_up: bool,
}

static mut STATS: Statistics = Statistics {
    pkt_count: 0,
    total_bytes: 0,
    last_id: 0,
    duration: 0,
    warmup: 0,
    warming_up: false,
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
        .arg(arg!(target: -t --target <target> "Target MAC address").required(true))
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

            do_client(iface, target, size, duration, warmup)
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

        let eth_pkt: EthernetPacket = EthernetPacket::new(&packet).unwrap();
        if eth_pkt.get_ethertype() != EtherType(ETHERTYPE_PERF) {
            continue;
        }

        let perf_pkt: PerfPacket = PerfPacket::new(eth_pkt.payload()).unwrap();

        match perf_pkt.get_op() {
            PerfOpFieldValues::ReqStart => {
                println!("Received ReqStart");

                if unsafe { TEST_RUNNING } {
                    println!("Already running");
                    continue;
                }

                let req_start: PerfStartReqPacket =
                    PerfStartReqPacket::new(perf_pkt.payload()).unwrap();
                let duration: Duration = Duration::from_secs(req_start.get_duration().into());
                let warmup: Duration = Duration::from_secs(req_start.get_warmup().into());

                unsafe {
                    STATS.duration = duration.as_secs() as usize;
                    STATS.warmup = warmup.as_secs() as usize;
                    STATS.pkt_count = 0;
                    STATS.total_bytes = 0;
                    STATS.last_id = 0;
                    STATS.warming_up = if STATS.warmup > 0 { true } else { false };
                    TEST_RUNNING = true;
                }

                // Make thread for statistics
                thread::spawn(stats_worker);

                let mut perf_buffer = vec![0; 8];
                let mut eth_buffer = vec![0; 14 + 8];

                let mut perf_pkt = MutablePerfPacket::new(&mut perf_buffer).unwrap();
                perf_pkt.set_id(perf_pkt.get_id());
                perf_pkt.set_op(PerfOpFieldValues::ResStart);

                let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
                eth_pkt.set_destination(eth_pkt.get_source());
                eth_pkt.set_source(my_mac);
                eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

                eth_pkt.set_payload(perf_pkt.packet());
                if let Err(e) = sock.send(eth_pkt.packet()) {
                    eprintln!("Failed to send packet: {}", e)
                }
            }
            PerfOpFieldValues::Data => {
                unsafe {
                    if !STATS.warming_up {
                        STATS.last_id = perf_pkt.get_id();
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
                perf_pkt.set_id(perf_pkt.get_id());
                perf_pkt.set_op(PerfOpFieldValues::ResEnd);

                let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
                eth_pkt.set_destination(eth_pkt.get_source());
                eth_pkt.set_source(my_mac);
                eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

                eth_pkt.set_payload(perf_pkt.packet());
                if let Err(e) = sock.send(eth_pkt.packet()) {
                    eprintln!("Failed to send packet: {}", e)
                }

                // Print statistics
                unsafe {
                    println!(
                        "{} packets, {} bytes {} bps",
                        STATS.pkt_count,
                        STATS.total_bytes,
                        STATS.total_bytes * 8 / STATS.duration
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

fn do_client(iface_name: String, target: String, size: usize, duration: usize, warmup: usize) {
    let interface_name_match = |iface: &NetworkInterface| iface.name == iface_name;
    let interfaces = datalink::interfaces();
    let interface = interfaces.into_iter().find(interface_name_match).unwrap();
    let my_mac = interface.mac.unwrap();

    let target: MacAddr = target.parse().expect("Invalid MAC address");

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

    let mut response_received = false;

    for _ in 0..3 {
        if let Err(e) = sock.send(eth_pkt.packet()) {
            eprintln!("Failed to send packet: {}", e)
        }

        match wait_for_response(&mut sock, PerfOpFieldValues::ResStart) {
            Err(_) => eprintln!("No response, retrying..."),
            Ok(_) => {
                response_received = true;
                break;
            },
        }
    }

    if !response_received {
        eprintln!("Server is not responding");
        return;
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

    // Send data
    println!("Sending data");
    let mut perf_buffer = vec![0; 8 + size];
    let mut eth_buffer = vec![0; 14 + 8 + size];
    let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
    eth_pkt.set_destination(target);
    eth_pkt.set_source(my_mac);
    eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));

    let now = Instant::now();
    let mut last_id = 0;
    loop {
        let mut perf_pkt = MutablePerfPacket::new(&mut perf_buffer).unwrap();
        perf_pkt.set_id(last_id); // TODO: Randomize
        perf_pkt.set_op(PerfOpFieldValues::Data);

        eth_pkt.set_payload(perf_pkt.packet());
        if sock.send(eth_pkt.packet()).is_err() {}

        last_id += 1;

        if now.elapsed().as_secs() > (duration + warmup) as u64 || !unsafe { RUNNING } {
            break;
        }
    }

    // Request end
    println!("Requesting end");
    let mut perf_buffer = vec![0; 8];
    let mut eth_buffer = vec![0; 14 + 8];

    let mut perf_pkt = MutablePerfPacket::new(&mut perf_buffer).unwrap();
    perf_pkt.set_id(0xdeadbeef); // TODO: Randomize
    perf_pkt.set_op(PerfOpFieldValues::ReqEnd);

    let mut eth_pkt = MutableEthernetPacket::new(&mut eth_buffer).unwrap();
    eth_pkt.set_destination(target);
    eth_pkt.set_source(my_mac);
    eth_pkt.set_ethertype(EtherType(ETHERTYPE_PERF));
    eth_pkt.set_payload(perf_pkt.packet());

    for _ in 0..3 {
        if let Err(e) = sock.send(eth_pkt.packet()) {
            eprintln!("Failed to send packet: {}", e);
        }

        match wait_for_response(&mut sock, PerfOpFieldValues::ResEnd) {
            Ok(_) => break,
            Err(_) => eprintln!("No response, retrying..."),
        }
    }

    println!("Closing socket...");
    if let Err(e) = sock.close() {
        eprintln!("Failed to close socket: {}", e)
    }
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
    let stats = unsafe { &mut STATS };
    let mut last_id = 0;
    let mut last_bytes = 0;
    let mut last_packets = 0;
    let start_time = Instant::now();
    let mut last_time = start_time;

    const SECOND: Duration = Duration::from_secs(1);

    if stats.warming_up {
        println!("Warming up");
        thread::sleep(Duration::from_secs(stats.warmup as u64));
        println!("Finished warmup");
        stats.warming_up = false;
    }

    while unsafe { TEST_RUNNING } {
        let elapsed = last_time.elapsed();
        if elapsed < SECOND {
            thread::sleep(SECOND - elapsed);
        }

        last_time = Instant::now();

        let id = stats.last_id;
        let bytes = stats.total_bytes;
        let bits = (bytes - last_bytes) * 8;
        let total_packets = stats.pkt_count;
        let packets = total_packets - last_packets;
        let loss_rate = 1.0 - packets as f64 / (id - last_id) as f64;

        let lap = start_time.elapsed().as_secs() - stats.warmup as u64;

        if lap > stats.duration as u64 {
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
