use crate::cbs::{normalise_cbs, CbsConfig};
use crate::tas::{normalise_tas, TasConfig};
use serde_yaml::{self, Value};
use std::collections::HashMap;
use std::fs::File;
use std::io::BufReader;
use std::str;

#[derive(Clone)]
pub struct Config {
    pub tas: Option<TasConfig>,
    pub cbs: Option<CbsConfig>,
}

impl Config {
    pub fn new() -> Config {
        Config {
            tas: None,
            cbs: None,
        }
    }
}

pub fn read_config(config_path: &str) -> Result<HashMap<String, Config>, i64> {
    let file = File::open(config_path).expect("failed to open config.yaml");
    let reader = BufReader::new(file);
    let config: Value = serde_yaml::from_reader(reader).expect("failed to parse YAML");
    let config = config
        .as_mapping()
        .expect("config file should be a dictionary")
        .get(&Value::String("nics".to_string()))
        .expect("config file should have a 'nics' key")
        .as_mapping()
        .expect("'nics' value should be a dictionary");
    let mut ifname;
    let mut ret = HashMap::new();
    for (key, value) in config {
        let mut info = Config::new();
        let value = value
            .as_mapping()
            .expect("config value should be a dictionary");
        ifname = key.as_str().unwrap();
        if value.contains_key(&Value::String("tas".to_string())) {
            match normalise_tas(
                value
                    .get(&Value::String("tas".to_string()))
                    .expect("tas should be a dictionary"),
            ) {
                Ok(tas) => info.tas = Some(tas),
                Err(e) => {
                    eprintln!("{}", e);
                    return Err(-1);
                }
            }
        }
        if value.contains_key(&Value::String("cbs".to_string())) {
            match normalise_cbs(
                ifname,
                value
                    .get(&Value::String("cbs".to_string()))
                    .expect("cbs should be a dictionary"),
            ) {
                Ok(cbs) => info.cbs = Some(cbs),
                Err(e) => {
                    eprintln!("{}", e);
                    return Err(-1);
                }
            }
        }
        ret.insert(ifname.to_string(), info);
    }
    Ok(ret)
}
