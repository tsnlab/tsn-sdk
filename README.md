# TSN SDK

![Build status](https://github.com/tsnlab/tsn-sdk/actions/workflows/build.yml/badge.svg)

TSN SDK is a full suite for making <abbr title="Time Sensitive Networking">TSN</abbr> application.


## Build

To install rust, run `curl -fsS https://sh.rustup.rs | sh`

```sh
cargo build --release  # Release build
cargo build  # Debug build
```

## Running examples

```sh
#Run latency

#Server
sudo ./target/release/latency -s -i <interface>
#Client
sudo ./target/release/latency -c -i <interface> -t <target MAC address>

#To see more options
sudo ./target/release/latency --help
sudo ./target/release/latency server help
sudo ./target/release/latency client help
```

```sh
#Run throughput

#Server
sudo ./target/release/throughput -s -i <interface>
#Client
sudo ./target/release/latency -c -i <interface> -t <target MAC address>

#To see more options
sudo ./target/release/throughput --help
sudo ./target/release/throughput server help
sudo ./target/release/throughput client help
```

## License

The TSN SDK is distributed under GPLv3 license. See [license](./LICENSE)  
If you need other license than GPLv3 for proprietary use or professional support, please mail us to contact at tsnlab dot com.
