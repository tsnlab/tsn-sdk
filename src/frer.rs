//! IEEE 802.1CB Frame Replication and Elimination for Reliability (FRER)
//!
//! This module provides userspace API for FRER configuration:
//! - Stream management (add/delete)
//! - Enable/disable FRER
//! - Statistics retrieval

use std::io::{Error, ErrorKind};
use std::mem;
use std::os::unix::io::RawFd;

/// IOCTL command numbers (must match driver's frer.h)
const SIOCDEVPRIVATE: libc::c_ulong = 0x89F0;
const SIOC_FRER_ADD_STREAM: libc::c_ulong = SIOCDEVPRIVATE + 1;
const SIOC_FRER_DEL_STREAM: libc::c_ulong = SIOCDEVPRIVATE + 2;
const SIOC_FRER_GET_STATS: libc::c_ulong = SIOCDEVPRIVATE + 3;
const SIOC_FRER_ENABLE: libc::c_ulong = SIOCDEVPRIVATE + 4;

/// MAC address length
const ETH_ALEN: usize = 6;

/// Stream identifier (SMAC + DMAC)
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FrerStreamId {
    pub smac: [u8; ETH_ALEN],
    pub dmac: [u8; ETH_ALEN],
}

/// Stream configuration for add_stream
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FrerStreamConfig {
    pub id: FrerStreamId,
    pub seq_gen_active: bool,  // Talker: Enable R-TAG insertion
    pub seq_recv_active: bool, // Listener: Enable duplicate elimination
}

impl FrerStreamConfig {
    /// Create a new stream configuration
    pub fn new(smac: [u8; ETH_ALEN], dmac: [u8; ETH_ALEN]) -> Self {
        Self {
            id: FrerStreamId { smac, dmac },
            seq_gen_active: false,
            seq_recv_active: false,
        }
    }

    /// Enable as Talker (insert R-TAG with sequence numbers)
    pub fn as_talker(mut self) -> Self {
        self.seq_gen_active = true;
        self
    }

    /// Enable as Listener (eliminate duplicates)
    pub fn as_listener(mut self) -> Self {
        self.seq_recv_active = true;
        self
    }

    /// Enable both Talker and Listener
    pub fn as_both(mut self) -> Self {
        self.seq_gen_active = true;
        self.seq_recv_active = true;
        self
    }
}

/// Stream statistics
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FrerStreamStats {
    pub id: FrerStreamId,
    pub received_count: u64,
    pub eliminated_count: u64,
    pub out_of_order_count: u64,
    pub out_of_window_count: u64,
    pub recv_seq: u16,
    pub next_seq: u16,
}

/// FRER controller for a network interface
pub struct FrerController {
    sock: RawFd,
    ifname: String,
}

impl FrerController {
    /// Create a new FRER controller for the specified interface
    pub fn new(ifname: &str) -> Result<Self, Error> {
        let sock = unsafe { libc::socket(libc::AF_INET, libc::SOCK_DGRAM, 0) };
        if sock < 0 {
            return Err(Error::last_os_error());
        }

        Ok(Self {
            sock,
            ifname: ifname.to_string(),
        })
    }

    /// Prepare ifreq structure with interface name
    fn prepare_ifreq(&self) -> libc::ifreq {
        let mut ifr: libc::ifreq = unsafe { mem::zeroed() };
        let ifname_bytes = self.ifname.as_bytes();
        let len = ifname_bytes.len().min(libc::IFNAMSIZ - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(
                ifname_bytes.as_ptr(),
                ifr.ifr_name.as_mut_ptr() as *mut u8,
                len,
            );
        }
        ifr
    }

    /// Add a FRER stream
    pub fn add_stream(&self, config: &FrerStreamConfig) -> Result<(), Error> {
        let mut ifr = self.prepare_ifreq();
        ifr.ifr_ifru.ifru_data = config as *const _ as *mut libc::c_char;

        let ret = unsafe { libc::ioctl(self.sock, SIOC_FRER_ADD_STREAM, &ifr) };
        if ret < 0 {
            return Err(Error::last_os_error());
        }
        Ok(())
    }

    /// Delete a FRER stream
    pub fn del_stream(&self, id: &FrerStreamId) -> Result<(), Error> {
        let mut ifr = self.prepare_ifreq();
        ifr.ifr_ifru.ifru_data = id as *const _ as *mut libc::c_char;

        let ret = unsafe { libc::ioctl(self.sock, SIOC_FRER_DEL_STREAM, &ifr) };
        if ret < 0 {
            return Err(Error::last_os_error());
        }
        Ok(())
    }

    /// Enable or disable FRER globally
    pub fn set_enabled(&self, enabled: bool) -> Result<(), Error> {
        let mut ifr = self.prepare_ifreq();
        let enable_val: i32 = if enabled { 1 } else { 0 };
        ifr.ifr_ifru.ifru_data = &enable_val as *const _ as *mut libc::c_char;

        let ret = unsafe { libc::ioctl(self.sock, SIOC_FRER_ENABLE, &ifr) };
        if ret < 0 {
            return Err(Error::last_os_error());
        }
        Ok(())
    }

    /// Get statistics for a stream
    pub fn get_stats(
        &self,
        smac: [u8; ETH_ALEN],
        dmac: [u8; ETH_ALEN],
    ) -> Result<FrerStreamStats, Error> {
        let mut stats = FrerStreamStats {
            id: FrerStreamId { smac, dmac },
            ..Default::default()
        };

        let mut ifr = self.prepare_ifreq();
        ifr.ifr_ifru.ifru_data = &mut stats as *mut _ as *mut libc::c_char;

        let ret = unsafe { libc::ioctl(self.sock, SIOC_FRER_GET_STATS, &ifr) };
        if ret < 0 {
            return Err(Error::last_os_error());
        }
        Ok(stats)
    }
}

impl Drop for FrerController {
    fn drop(&mut self) {
        unsafe {
            libc::close(self.sock);
        }
    }
}

/// Parse MAC address from string (e.g., "00:11:22:33:44:55")
pub fn parse_mac(s: &str) -> Result<[u8; ETH_ALEN], Error> {
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() != ETH_ALEN {
        return Err(Error::new(
            ErrorKind::InvalidInput,
            "Invalid MAC address format",
        ));
    }

    let mut mac = [0u8; ETH_ALEN];
    for (i, part) in parts.iter().enumerate() {
        mac[i] = u8::from_str_radix(part, 16)
            .map_err(|_| Error::new(ErrorKind::InvalidInput, "Invalid hex in MAC address"))?;
    }
    Ok(mac)
}

/// Format MAC address to string
pub fn format_mac(mac: &[u8; ETH_ALEN]) -> String {
    format!(
        "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_mac() {
        let mac = parse_mac("00:11:22:33:44:55").unwrap();
        assert_eq!(mac, [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]);
    }

    #[test]
    fn test_format_mac() {
        let mac = [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF];
        assert_eq!(format_mac(&mac), "AA:BB:CC:DD:EE:FF");
    }
}
