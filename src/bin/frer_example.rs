//! FRER (802.1CB) Example
//!
//! This example demonstrates how to configure FRER for frame replication
//! and duplicate elimination.
//!
//! Usage:
//!   frer_example <interface> <mode> <src_mac> <dst_mac>
//!
//! Modes:
//!   talker   - Enable R-TAG insertion (sequence generation)
//!   listener - Enable duplicate elimination (sequence recovery)
//!   both     - Enable both talker and listener
//!   stats    - Show statistics for a stream
//!   disable  - Disable FRER globally
//!
//! Examples:
//!   # Configure as talker
//!   frer_example eth0 talker 00:11:22:33:44:55 AA:BB:CC:DD:EE:FF
//!
//!   # Configure as listener
//!   frer_example eth0 listener 00:11:22:33:44:55 AA:BB:CC:DD:EE:FF
//!
//!   # Get statistics
//!   frer_example eth0 stats 00:11:22:33:44:55 AA:BB:CC:DD:EE:FF

use std::env;
use std::process::exit;

// Import from the tsn_sdk crate
use tsn_sdk::frer::{format_mac, parse_mac, FrerController, FrerStreamConfig};

fn print_usage(program: &str) {
    eprintln!("FRER (IEEE 802.1CB) Configuration Tool\n");
    eprintln!(
        "Usage: {} <interface> <mode> [src_mac] [dst_mac]\n",
        program
    );
    eprintln!("Modes:");
    eprintln!("  talker   - Enable R-TAG insertion (sequence generation)");
    eprintln!("  listener - Enable duplicate elimination (sequence recovery)");
    eprintln!("  both     - Enable both talker and listener");
    eprintln!("  stats    - Show statistics for a stream");
    eprintln!("  delete   - Delete a stream");
    eprintln!("  enable   - Enable FRER globally");
    eprintln!("  disable  - Disable FRER globally\n");
    eprintln!("Examples:");
    eprintln!(
        "  {} eth0 talker 00:11:22:33:44:55 AA:BB:CC:DD:EE:FF",
        program
    );
    eprintln!(
        "  {} eth0 listener 00:11:22:33:44:55 AA:BB:CC:DD:EE:FF",
        program
    );
    eprintln!(
        "  {} eth0 stats 00:11:22:33:44:55 AA:BB:CC:DD:EE:FF",
        program
    );
    eprintln!("  {} eth0 enable", program);
}

fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 3 {
        print_usage(&args[0]);
        exit(1);
    }

    let ifname = &args[1];
    let mode = &args[2];

    // Create FRER controller
    let frer = match FrerController::new(ifname) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Failed to create FRER controller: {}", e);
            exit(1);
        }
    };

    match mode.as_str() {
        "enable" => match frer.set_enabled(true) {
            Ok(_) => println!("FRER enabled on {}", ifname),
            Err(e) => {
                eprintln!("Failed to enable FRER: {}", e);
                exit(1);
            }
        },

        "disable" => match frer.set_enabled(false) {
            Ok(_) => println!("FRER disabled on {}", ifname),
            Err(e) => {
                eprintln!("Failed to disable FRER: {}", e);
                exit(1);
            }
        },

        "talker" | "listener" | "both" | "stats" | "delete" => {
            if args.len() < 5 {
                eprintln!("Error: src_mac and dst_mac required for mode '{}'", mode);
                print_usage(&args[0]);
                exit(1);
            }

            let smac = match parse_mac(&args[3]) {
                Ok(m) => m,
                Err(e) => {
                    eprintln!("Invalid source MAC: {}", e);
                    exit(1);
                }
            };

            let dmac = match parse_mac(&args[4]) {
                Ok(m) => m,
                Err(e) => {
                    eprintln!("Invalid destination MAC: {}", e);
                    exit(1);
                }
            };

            match mode.as_str() {
                "talker" => {
                    let config = FrerStreamConfig::new(smac, dmac).as_talker();
                    match frer.add_stream(&config) {
                        Ok(_) => {
                            println!("Added FRER stream as Talker:");
                            println!("  SMAC: {}", format_mac(&smac));
                            println!("  DMAC: {}", format_mac(&dmac));

                            // Enable FRER
                            if let Err(e) = frer.set_enabled(true) {
                                eprintln!("Warning: Failed to enable FRER: {}", e);
                            }
                        }
                        Err(e) => {
                            eprintln!("Failed to add stream: {}", e);
                            exit(1);
                        }
                    }
                }

                "listener" => {
                    let config = FrerStreamConfig::new(smac, dmac).as_listener();
                    match frer.add_stream(&config) {
                        Ok(_) => {
                            println!("Added FRER stream as Listener:");
                            println!("  SMAC: {}", format_mac(&smac));
                            println!("  DMAC: {}", format_mac(&dmac));

                            // Enable FRER
                            if let Err(e) = frer.set_enabled(true) {
                                eprintln!("Warning: Failed to enable FRER: {}", e);
                            }
                        }
                        Err(e) => {
                            eprintln!("Failed to add stream: {}", e);
                            exit(1);
                        }
                    }
                }

                "both" => {
                    let config = FrerStreamConfig::new(smac, dmac).as_both();
                    match frer.add_stream(&config) {
                        Ok(_) => {
                            println!("Added FRER stream as Talker + Listener:");
                            println!("  SMAC: {}", format_mac(&smac));
                            println!("  DMAC: {}", format_mac(&dmac));

                            // Enable FRER
                            if let Err(e) = frer.set_enabled(true) {
                                eprintln!("Warning: Failed to enable FRER: {}", e);
                            }
                        }
                        Err(e) => {
                            eprintln!("Failed to add stream: {}", e);
                            exit(1);
                        }
                    }
                }

                "stats" => match frer.get_stats(smac, dmac) {
                    Ok(stats) => {
                        println!("FRER Stream Statistics:");
                        println!("  SMAC: {}", format_mac(&stats.id.smac));
                        println!("  DMAC: {}", format_mac(&stats.id.dmac));
                        println!("  Received:       {}", stats.received_count);
                        println!(
                            "  Eliminated:     {} (duplicates dropped)",
                            stats.eliminated_count
                        );
                        println!("  Out-of-order:   {} (accepted)", stats.out_of_order_count);
                        println!(
                            "  Out-of-window:  {} (dropped, too old)",
                            stats.out_of_window_count
                        );
                        println!("  Recv Seq:       {}", stats.recv_seq);
                        println!("  Next Seq:       {}", stats.next_seq);
                    }
                    Err(e) => {
                        eprintln!("Failed to get stats: {}", e);
                        exit(1);
                    }
                },

                "delete" => {
                    let id = tsn_sdk::frer::FrerStreamId { smac, dmac };
                    match frer.del_stream(&id) {
                        Ok(_) => {
                            println!("Deleted FRER stream:");
                            println!("  SMAC: {}", format_mac(&smac));
                            println!("  DMAC: {}", format_mac(&dmac));
                        }
                        Err(e) => {
                            eprintln!("Failed to delete stream: {}", e);
                            exit(1);
                        }
                    }
                }

                _ => unreachable!(),
            }
        }

        _ => {
            eprintln!("Unknown mode: {}", mode);
            print_usage(&args[0]);
            exit(1);
        }
    }
}
