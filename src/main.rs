use crate::{
    config::read_config,
    vlan::{create_vlan, delete_vlan},
};
use clap::{arg, Arg, ArgMatches, Command as ClapCommand};
mod cbs;
mod config;
mod info;
mod tas;
mod vlan;
fn main() {
    let arg_config = arg!(-c --config <config> "Config file path")
        .required(false)
        .default_value("config.yaml");

    let create_parser = ClapCommand::new("create")
        .about("Create a TSN interface")
        .arg(&arg_config)
        .arg(
            Arg::new("interface")
                .help("Interface name to create")
                .required(true),
        )
        .arg(Arg::new("vlanid").help("VLAN ID to create").required(true));
    let delete_parser = ClapCommand::new("delete")
        .about("Delete a TSN interface")
        .arg(&arg_config)
        .arg(
            Arg::new("interface")
                .help("Interface name to delete")
                .required(true),
        )
        .arg(Arg::new("vlanid").help("VLAN ID to delete").required(true));
    let info_parser = ClapCommand::new("info")
        .about("Show TSN interface information")
        .arg(&arg_config)
        .arg(
            Arg::new("interface")
                .help("Interface name to show")
                .required(false)
                .multiple_values(true),
        );
    let matched_command: ArgMatches = ClapCommand::new("tsnlib")
        .about("TSN socket manager")
        .arg_required_else_help(true)
        .subcommand(create_parser)
        .subcommand(delete_parser)
        .subcommand(info_parser)
        .get_matches();
    match matched_command.subcommand() {
        Some(("create", create_matches)) => {
            let config = read_config(create_matches.value_of("config").unwrap());
            if config.is_err() {
                return;
            }
            let interface = create_matches.value_of("interface").unwrap();
            let vlan_id = create_matches
                .value_of("vlanid")
                .unwrap()
                .parse::<u16>()
                .unwrap();
            let config = config.as_ref().unwrap().get(interface).unwrap();
            create_vlan(config, interface, vlan_id).unwrap();
        }
        Some(("delete", delete_matches)) => {
            let interface = delete_matches.value_of("interface").unwrap();
            let vlan_id = delete_matches
                .value_of("vlanid")
                .unwrap()
                .parse::<u16>()
                .unwrap();
            delete_vlan(interface, vlan_id).unwrap();
        }
        Some(("info", info_matches)) => {
            let config = read_config(info_matches.value_of("config").unwrap());
            if config.is_err() {
                return;
            }
            let config = config.as_ref().unwrap();
            if info_matches.is_present("interface") {
                let interfaces = info_matches.values_of("interface").unwrap();
                for interface in interfaces {
                    println!("{}:", interface);
                    let config = config.get(interface).unwrap();
                    info::get_info(config);
                }
            } else {
                for (interface, config) in config {
                    println!("{}:", interface);
                    info::get_info(config);
                }
            }
        }
        _ => unreachable!(),
    }
}
