use serde_yaml::{self, Value};
use std::collections::HashMap;
use std::process::Command;
use std::str;
#[derive(Clone)]
pub struct CbsChild {
    #[allow(dead_code)]
    pub prio: i64,
    pub max_frame: i64,
    pub bandwidth: i64,
}

#[derive(Clone)]
pub struct CbsCredit {
    pub sendslope: i64,
    pub idleslope: i64,
    pub hicredit: i64,
    pub locredit: i64,
}

#[derive(Clone)]
pub struct CbsConfig {
    pub tc_map: HashMap<i64, i64>,
    pub num_tc: i64,
    pub queues: Vec<String>,
    pub children: HashMap<i64, CbsCredit>,
    #[allow(dead_code)]
    pub streams: HashMap<char, Vec<CbsChild>>,
}

pub fn get_linkspeed(ifname: &str) -> Result<String, String> {
    let interface = interfaces::Interface::get_by_name(ifname);
    if interface.is_err() || interface.unwrap().is_none() {
        return Err(format!("Interface {} not found", ifname));
    }

    let output = Command::new("ethtool").arg(ifname).output();
    match output {
        Ok(output) => {
            let out = str::from_utf8(&output.stdout).unwrap();
            let pattern = regex::Regex::new(r"Speed: (?P<speed>\d+(?:|k|M|G)b[p/]?s)").unwrap();
            match pattern.captures(out) {
                Some(matched) => {
                    return Ok(matched.name("speed").unwrap().as_str().to_string());
                }
                None => {
                    return Err(format!("Speed cannot be found for {}", ifname));
                }
            }
        }
        Err(e) => Err(e.to_string()),
    }
}
pub fn to_bits(input: &Value) -> Result<i64, String> {
    if let Some(value) = input.as_str() {
        let matched =
            regex::Regex::new(r"^(?P<v>[\d_]+)\s*(?P<modifier>|k|M|G|ki|Mi|Gi)(?P<b>b|B)$")
                .unwrap()
                .captures(value)
                .unwrap();
        let v = matched.name("v").unwrap().as_str().parse::<i64>().unwrap();
        let modifier = matched.name("modifier").unwrap().as_str();
        let b = matched.name("b").unwrap().as_str();
        let multiplier_bits = match b {
            "b" => 1,
            "B" => 8,
            _ => unreachable!(),
        };
        let multiplier_modifier = match modifier {
            "" => 1,
            "k" => 1000,
            "M" => 1000 * 1000,
            "G" => 1000 * 1000 * 1000,
            "ki" => 1024,
            "Mi" => 1024 * 1024,
            "Gi" => 1024 * 1024 * 1024,
            _ => unreachable!(),
        };
        return Ok(v * multiplier_bits * multiplier_modifier);
    }
    Ok(input.as_i64().unwrap())
}

pub fn to_bps(input: &Value) -> Result<i64, String> {
    if let Some(value) = input.as_str() {
        let matched = regex::Regex::new(r"^(?P<v>[\d_]+)\s*(?P<modifier>|k|M|G)(?P<b>b|B)[p/]s$")
            .unwrap()
            .captures(value)
            .unwrap();
        let v = matched.name("v").unwrap().as_str().parse::<i64>().unwrap();
        let modifier = matched.name("modifier").unwrap().as_str();
        return {
            match modifier {
                "" => Ok(v),
                "k" => Ok(v * 1000),
                "M" => Ok(v * 1000 * 1000),
                "G" => Ok(v * 1000 * 1000 * 1000),
                _ => Err(format!("{} is not valid bandwidth", value)),
            }
        };
    }
    Ok(input.as_i64().unwrap())
}

pub fn calc_credits(
    streams: &HashMap<char, Vec<CbsChild>>,
    linkspeed: i64,
) -> (CbsCredit, CbsCredit) {
    let mut idle_slope_a = 0;
    let mut max_frame_a = 0;
    for stream in streams.get(&'a').unwrap() {
        idle_slope_a += stream.bandwidth;
        max_frame_a += stream.max_frame;
    }
    let send_slope_a = idle_slope_a - linkspeed;
    let hicredit_a = f64::ceil(idle_slope_a as f64 * max_frame_a as f64 / linkspeed as f64) as i64;
    let locredit_a = f64::ceil(send_slope_a as f64 * max_frame_a as f64 / linkspeed as f64) as i64;
    let credits_a = CbsCredit {
        sendslope: f64::floor(send_slope_a as f64 / 1000.0) as i64,
        idleslope: f64::floor(idle_slope_a as f64 / 1000.0) as i64,
        hicredit: hicredit_a,
        locredit: locredit_a,
    };

    let mut idle_slope_b = 0;
    let mut max_frame_b = 0;
    for stream in streams.get(&'b').unwrap() {
        idle_slope_b += stream.bandwidth;
        max_frame_b += stream.max_frame;
    }
    let send_slope_b = idle_slope_b - linkspeed;
    let hicredit_b = f64::ceil(
        idle_slope_b as f64
            * ((max_frame_b as f64 / (linkspeed - idle_slope_a) as f64)
                + (max_frame_a as f64 / linkspeed as f64)),
    ) as i64;
    let locredit_b = f64::ceil(send_slope_b as f64 * max_frame_b as f64 / linkspeed as f64) as i64;
    let credits_b = CbsCredit {
        sendslope: f64::floor(send_slope_b as f64 / 1000.0) as i64,
        idleslope: f64::floor(idle_slope_b as f64 / 1000.0) as i64,
        hicredit: hicredit_b,
        locredit: locredit_b,
    };
    (credits_a, credits_b)
}

pub fn normalise_cbs(ifname: &str, config: &Value) -> Result<CbsConfig, String> {
    let mut tc_map = HashMap::new();
    let mut ret_map = HashMap::new();
    let link = get_linkspeed(ifname);
    let mut streams = HashMap::new();
    let mut children: HashMap<i64, CbsCredit> = HashMap::new();
    let mut queues: Vec<String> = Vec::new();
    streams.insert('a', Vec::new());
    streams.insert('b', Vec::new());
    let linkspeed: i64 = match link {
        Ok(speed) => to_bps(&Value::String(speed))?,
        Err(_) => 1_000_000_000, // 1000Mbps
    };
    for (prio, priomap) in config.as_mapping().unwrap() {
        if !tc_map.contains_key(&prio.as_i64().unwrap()) {
            tc_map.insert(prio.as_i64().unwrap(), tc_map.len() as i64);
        }
        let child = CbsChild {
            prio: prio.as_i64().unwrap(),
            max_frame: to_bits(
                priomap
                    .get(&Value::String("max_frame".to_string()))
                    .expect("max_frame should be present"),
            )?,
            bandwidth: to_bps(
                priomap
                    .get(&Value::String("bandwidth".to_string()))
                    .expect("bandwidth should be present"),
            )?,
        };
        let index = priomap
            .get(Value::String("class".to_string()).as_str().unwrap())
            .expect("class should be present")
            .as_str()
            .expect("class should be a string")
            .chars()
            .next()
            .expect("Cannot parse class");
        streams
            .get_mut(&index)
            .expect("Cannot parse cbs class")
            .push(child);
    }
    tc_map.insert(-1, tc_map.len() as i64);
    let num_tc = tc_map.len() as i64;
    for i in 0..16 {
        if tc_map.contains_key(&i) {
            ret_map.insert(i, *tc_map.get(&i).unwrap());
        } else {
            ret_map.insert(i, *tc_map.get(&-1).unwrap());
        }
    }
    let (credits_a, credits_b) = calc_credits(&streams, linkspeed);
    children.insert(1, credits_a);
    children.insert(2, credits_b);
    for i in 0..num_tc {
        queues.push(format!("1@{}", i));
    }
    Ok(CbsConfig {
        tc_map: ret_map,
        num_tc,
        queues,
        children,
        streams,
    })
}
