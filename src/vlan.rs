use crate::{cbs::CbsConfig, config::Config, tas::TasConfig};
use itertools::Itertools;
use std::{collections::HashMap, io::Error};

fn run_cmd(input: &str) -> Result<i32, String> {
    eprintln!("{}", input);
    let split = input.split(char::is_whitespace);
    let cmd = split.clone().next().unwrap();
    let cmd = std::process::Command::new(cmd)
        .args(split.skip(1))
        .spawn()
        .unwrap();
    let output = cmd.wait_with_output().unwrap();
    if output.status.success() {
        Ok(0)
    } else {
        Err(Error::from_raw_os_error(output.status.code().unwrap()).to_string())
    }
}

pub fn setup_tas(ifname: &str, config: &TasConfig) -> Result<i32, String> {
    let handle = 100;
    let num_tc = config.num_tc;
    let mut priomap = String::new();
    let mut queues = String::new();
    let base_time = config.base_time;
    let txtime_delay = config.txtime_delay;
    let mut sched_entries = String::new();
    for key in config.tc_map.keys().sorted() {
        priomap.push_str(&format!(" {}", config.tc_map.get(key).unwrap()));
    }
    for queue in &config.queues {
        queues.push_str(&format!(" {}", queue));
    }
    for entry in &config.sched_entries {
        sched_entries.push_str(&format!(" sched-entry {}", entry));
    }
    let cmd = format!(
        "tc qdisc replace dev {} parent root handle {} taprio num_tc {} map{} \
         queues{} base-time {}{} flags 0x2 txtime-delay {}",
        ifname, handle, num_tc, priomap, queues, base_time, sched_entries, txtime_delay
    );
    run_cmd(&cmd)?;
    // TSN NIC does not support ETF for now
    /*
    let cmd = format!(
        "tc qdisc replace dev {} parent {}:1 etf clockid \
         CLOCK_TAI delta {} offload skip_sock_check",
        ifname, handle, txtime_delay
    );
    run_cmd(&cmd)?;
    */
    Ok(0)
}

pub fn setup_cbs(ifname: &str, config: &CbsConfig) -> Result<i32, String> {
    let root_handle = 100;
    let num_tc = config.num_tc;
    let mut priomap = String::new();
    let mut queues = String::new();
    for key in config.tc_map.keys().sorted() {
        priomap.push_str(&format!(" {}", config.tc_map.get(key).unwrap()));
    }
    for s in &config.queues {
        queues.push_str(&format!("{} ", s));
    }
    let cmd = format!(
        "tc qdisc add dev {} parent root handle {} mqprio \
         num_tc {} map{} queues {}hw 0",
        ifname, root_handle, num_tc, priomap, queues
    );
    run_cmd(&cmd)?;
    for (qid, val) in &config.children {
        let handle = qid * 1111;

        let idleslope = val.idleslope;
        let sendslope = val.sendslope;
        let hicredit = val.hicredit;
        let locredit = val.locredit;
        let cmd = format!(
            "tc qdisc replace dev {} parent {}:{} handle {} \
             cbs idleslope {} sendslope {} hicredit {} locredit {} offload 1",
            ifname, root_handle, qid, handle, idleslope, sendslope, hicredit, locredit
        );
        run_cmd(&cmd)?;
    }
    Ok(0)
}

pub fn create_vlan(config: &Config, ifname: &str, vlan_id: u16) -> Result<i32, String> {
    let name = get_vlan_name(ifname, vlan_id);

    if config.tas.is_some() && config.cbs.is_some() {
        eprintln!("Does not support both TAS and CBS");
        return Err("Does not support both TAS and CBS".to_string());
    }

    let mut cmd = String::new();
    if let Some(qos_map) = config.egress_qos_map.get(&(vlan_id as i64)) {
        if !qos_map.is_empty() {
            cmd.push_str(&format!(
                "ip link add link {} name {} type vlan id {} egress-qos-map",
                ifname, name, vlan_id
            ));
            for (prio, pri) in qos_map {
                cmd.push_str(&format!(" {}:{}", pri, prio));
            }
        } else {
            cmd.push_str(&format!(
                "ip link add link {} name {} type vlan id {}",
                ifname, name, vlan_id
            ));
        }
    } else {
        cmd.push_str(&format!(
            "ip link add link {} name {} type vlan id {}",
            ifname, name, vlan_id
        ));
    }
    run_cmd(&cmd)?;
    let cmd = format!("ip link set up {}", name);
    run_cmd(&cmd)?;
    if let Some(tas) = &config.tas {
        setup_tas(ifname, tas)?;
    }
    if let Some(cbs) = &config.cbs {
        setup_cbs(ifname, cbs)?;
    }
    Ok(0)
}

pub fn delete_vlan(ifname: &str, vlanid: u16) -> Result<i32, String> {
    let name = get_vlan_name(ifname, vlanid);
    let cmd = format!("ip link del {}", name);
    run_cmd(&cmd)?;
    let cmd = format!("tc qdisc delete dev {} root", ifname);
    run_cmd(&cmd)?;
    Ok(0)
}

pub fn get_vlan_name(ifname: &str, vlanid: u16) -> String {
    if ifname.len() > 10 {
        format!("{}.{}", &ifname[..10], vlanid)
    } else {
        format!("{}.{}", &ifname, vlanid)
    }
}
