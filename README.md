# TSN SDK

![Build status](https://github.com/tsnlab/tsn-sdk/actions/workflows/build.yml/badge.svg)

TSN SDK is a full suite for making <abbr title="Time Sensitive Networking">TSN</abbr> application.

For the GStreamer, sensor, and TAS/CBS examples, start with [TSN application examples](examples/README.md).


## Build

To install rust, run `curl -fsS https://sh.rustup.rs | sh`

```sh
cargo build --release  # Release build
cargo build  # Debug build
```

## Running examples

Use the interface connected to the peer and the peer interface's MAC address. Both tools default to untagged Ethernet. This path needs no VLAN interface, config file, or Linux traffic-control offload.

```sh
# Set IFACE to the local interface on each host.
# Set PEER_MAC to the receiver's interface MAC on the client.
sudo ./target/release/latency -s -i "$IFACE" --tstamp sw
sudo ./target/release/latency -c -i "$IFACE" -t "$PEER_MAC" --tstamp sw
```

Start the server first. The client prints latency samples in nanoseconds. The commands above use software timestamps so a board without transmit hardware timestamps can run the example.

```sh
sudo ./target/release/throughput -s -i "$IFACE"
sudo ./target/release/throughput -c -i "$IFACE" -t "$PEER_MAC"
```

The throughput server prints the received packet and byte counts and the measured bit rate. Swap the roles to measure the other direction.

For tagged traffic, add `--vlanid <VID>` (1–4094) on both hosts. Allow that VLAN along the link and use a board image that admits it. In `config.yaml`, use the actual local interface name under `nics`. To select a different file, set `CONFIG_PATH`. Leave `tas` and `cbs` unset for TSN mode. Its host traffic uses one hardware queue, and Linux traffic-control hardware offload is unavailable. The [TAS/CBS example](examples/tas_cbs/README.md) configures the board shaper over PCIe.

Run either binary with `--help` to see the remaining options.

## License

The TSN SDK is distributed under GPLv3 license. See [license](./LICENSE)  
If you need other license than GPLv3 for proprietary use or professional support, please mail us to contact at tsnlab dot com.
