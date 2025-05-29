use core::slice;
use nix::net::if_::if_nametoindex;
use nix::sys::socket::{cmsghdr, msghdr};
use nix::sys::time::{TimeSpec, TimeValLike};
use nix::unistd::close;
use nix::{
    fcntl::{fcntl, FcntlArg::F_SETLKW, OFlag},
    libc::{self, flock, ftruncate, msync, MS_SYNC},
    sys::{
        mman::{mmap, munmap, shm_open, shm_unlink, MapFlags, ProtFlags},
        signal::kill,
        stat::Mode,
    },
    unistd::Pid,
};
use std::io::{Error, ErrorKind};
use std::{env, mem, str};
use std::{mem::size_of, num::NonZeroUsize, os::raw::c_void, process, time::Duration};

extern crate socket;

pub struct TsnSocket {
    pub fd: i32,
    pub ifname: String,
    pub vlanid: u16,
    pub msg: Option<msghdr>,
}

mod cbs;
mod config;
mod tas;
pub mod time;
mod vlan;
const SHM_SIZE: usize = 128;

// Make imple for TsnSocket
impl TsnSocket {
    pub fn set_timeout(&mut self, timeout: Duration) -> Result<(), String> {
        sock_set_timeout(self, timeout)
    }

    pub fn send(&self, buf: &[u8]) -> Result<isize, String> {
        send(self, buf)
    }

    pub fn recv(&self, buf: &mut [u8]) -> Result<isize, String> {
        recv(self, buf)
    }

    pub fn recv_msg(&self, msg: &mut msghdr) -> Result<isize, String> {
        recv_msg(self, msg)
    }

    pub fn enable_timestamps(&mut self, iov: Option<&mut libc::iovec>) -> Result<(), Error> {
        enable_timestamps(self, iov)
    }

    pub fn get_tx_timestamp(&self) -> Result<time::Timespec, Error> {
        get_tx_timestamp(self)
    }

    pub fn get_rx_timestamp(&self) -> Result<time::Timespec, Error> {
        get_rx_timestamp(self)
    }

    pub fn close(&mut self) -> Result<(), String> {
        sock_close(self)
    }
}

fn create_vlan(ifname: &str, vlanid: u16) -> Result<String, String> {
    let config = get_config(ifname)?;
    let shm_name = get_shmem_name(ifname, vlanid);
    let shm_fd = get_shmem_fd(&shm_name)?;
    lock_shmem(&shm_fd)?;
    let mut vlan_vec = read_shmem(&shm_name)?;
    let name = vlan::get_vlan_name(ifname, vlanid);
    // If I am the frist user of this vlan, create it
    let result = if vlan_vec.is_empty() {
        vlan::create_vlan(&config, ifname, vlanid)
    } else {
        Ok(0)
    };
    vlan_vec.push(process::id());
    write_shmem(&shm_name, &vlan_vec)?;
    unlock_shmem(&shm_fd)?;
    match result {
        Ok(_) => Ok(name),
        Err(_) => Err(format!("Create vlan fails {}", Error::last_os_error())),
    }
}

fn delete_vlan(ifname: &str, vlanid: u16) -> Result<i32, String> {
    let shm_name = get_shmem_name(ifname, vlanid);
    let shm_fd = get_shmem_fd(&shm_name)?;
    lock_shmem(&shm_fd)?;
    let mut vlan_vec = read_shmem(&shm_name)?;
    // remove my pid from shmem
    for i in 0..vlan_vec.len() {
        if vlan_vec[i] == process::id() {
            vlan_vec.remove(i);
            break;
        }
    }
    // delete dead process from vector
    vlan_vec.retain(|x| kill(Pid::from_raw(*x as i32), None).is_ok());
    let exit_flag = vlan_vec.is_empty();
    vlan_vec.resize(SHM_SIZE / size_of::<u32>(), 0);
    write_shmem(&shm_name, &vlan_vec)?;
    let result = if exit_flag {
        if shm_unlink(&*shm_name).is_err() {
            return Err(format!("Delete shmem fails {}", Error::last_os_error()));
        }
        match vlan::delete_vlan(ifname, vlanid) {
            Ok(v) => Ok(v),
            Err(_) => Err(format!("Delete vlan fails {}", Error::last_os_error())),
        }
    } else {
        Ok(0)
    };
    unlock_shmem(&shm_fd)?;
    result
}

pub fn sock_open(
    ifname: &str,
    vlanid: u16,
    priority: u32,
    proto: u16,
) -> Result<TsnSocket, String> {
    let name = match create_vlan(ifname, vlanid) {
        Ok(v) => v,
        Err(_) => {
            return Err(format!("Create vlan fails {}", Error::last_os_error()));
        }
    };
    let sock;
    let mut res;
    let ifindex = if_nametoindex(name.as_bytes()).expect("vlan_ifname index");
    unsafe {
        sock = libc::socket(
            libc::AF_PACKET,
            libc::SOCK_RAW,
            socket::htons(proto) as libc::c_int,
        );
    }
    if sock < 0 {
        return Err(Error::last_os_error().to_string());
    }
    let prio: *const u32 = &priority;
    unsafe {
        res = libc::setsockopt(
            sock as libc::c_int,
            libc::SOL_SOCKET,
            libc::SO_PRIORITY,
            prio as *const libc::c_void,
            mem::size_of_val(&prio) as u32,
        );
    }

    if res < 0 {
        return Err(format!("Socket option error: {}", Error::last_os_error()));
    }

    let sock_ll = libc::sockaddr_ll {
        sll_family: libc::AF_PACKET as u16,
        sll_ifindex: ifindex as i32,
        sll_addr: [0, 0, 0, 0, 0, 0, 0, 0],
        sll_halen: 0,
        sll_hatype: 0,
        sll_protocol: 0,
        sll_pkttype: 0,
    };

    unsafe {
        res = libc::bind(
            sock,
            &sock_ll as *const libc::sockaddr_ll as *const libc::sockaddr,
            mem::size_of_val(&sock_ll) as u32,
        );
    }
    if res < 0 {
        return Err(format!("Bind error: {}", Error::last_os_error()));
    }

    Ok(TsnSocket {
        fd: sock,
        ifname: ifname.to_string(),
        vlanid,
        msg: None,
    })
}

pub fn sock_close(sock: &mut TsnSocket) -> Result<(), String> {
    match delete_vlan(&sock.ifname, sock.vlanid) {
        Ok(_) => {
            close(sock.fd).unwrap();
            Ok(())
        }
        Err(_) => Err(format!("Delete vlan fails: {}", Error::last_os_error())),
    }
}

pub fn sock_set_timeout(sock: &mut TsnSocket, timeout: Duration) -> Result<(), String> {
    let sock_timeout = libc::timeval {
        tv_sec: timeout.as_secs() as i64,
        tv_usec: timeout.subsec_micros() as i64,
    };

    let res = unsafe {
        libc::setsockopt(
            sock.fd,
            libc::SOL_SOCKET,
            libc::SO_RCVTIMEO,
            &sock_timeout as *const libc::timeval as *const libc::c_void,
            mem::size_of::<libc::timeval>() as u32,
        )
    };

    if res < 0 {
        Err(format!("Set timeout error: {}", Error::last_os_error()))
    } else {
        Ok(())
    }
}

pub fn send(sock: &TsnSocket, buf: &[u8]) -> Result<isize, String> {
    let res = unsafe {
        libc::sendto(
            sock.fd,
            buf.as_ptr() as *const libc::c_void,
            buf.len(),
            0,
            std::ptr::null_mut::<libc::sockaddr>(),
            0_u32,
        )
    };

    if res < 0 {
        Err(format!("Send error: {}", Error::last_os_error()))
    } else {
        Ok(res)
    }
}

pub fn recv(sock: &TsnSocket, buf: &mut [u8]) -> Result<isize, String> {
    let res = unsafe {
        libc::recvfrom(
            sock.fd,
            buf.as_ptr() as *mut libc::c_void,
            buf.len(),
            0 as libc::c_int, /* flags */
            std::ptr::null_mut::<libc::sockaddr>(),
            std::ptr::null_mut::<u32>(),
        )
    };

    if res < 0 {
        Err(format!("Recv error: {}", Error::last_os_error()))
    } else {
        Ok(res)
    }
}

pub fn recv_msg(sock: &TsnSocket, msg: &mut msghdr) -> Result<isize, String> {
    let res = unsafe { libc::recvmsg(sock.fd, msg, 0) };

    if res < 0 {
        Err(format!("Recv error: {}", Error::last_os_error()))
    } else {
        Ok(res)
    }
}

pub fn enable_timestamps(sock: &mut TsnSocket, iov: Option<&mut libc::iovec>) -> Result<(), Error> {
    let sockfd = sock.fd;
    let interface_name = &sock.ifname;

    let ts_flags: u32 = libc::SOF_TIMESTAMPING_TX_HARDWARE
        | libc::SOF_TIMESTAMPING_RX_HARDWARE
        | libc::SOF_TIMESTAMPING_SYS_HARDWARE
        | libc::SOF_TIMESTAMPING_RAW_HARDWARE;

    let err = unsafe {
        libc::setsockopt(
            sockfd,
            libc::SOL_SOCKET,
            libc::SO_TIMESTAMPING,
            &ts_flags as *const _ as *const libc::c_void,
            std::mem::size_of::<u32>().try_into().unwrap(),
        )
    };
    if err < 0 {
        return Err(Error::last_os_error());
    }

    // setsockopt for err queue

    let flags: i32 = 1;
    let err = unsafe {
        libc::setsockopt(
            sockfd,
            libc::SOL_SOCKET,
            libc::SO_SELECT_ERR_QUEUE,
            &flags as *const _ as *const libc::c_void,
            std::mem::size_of::<i32>().try_into().unwrap(),
        )
    };
    if err < 0 {
        return Err(Error::last_os_error());
    }
    
    // Always enable timestamps as disabling it will stop ptp4l

    // ioctl
    let mut ts_cfg = libc::hwtstamp_config {
        tx_type: libc::HWTSTAMP_TX_ON as i32,
        rx_filter: libc::HWTSTAMP_FILTER_ALL as i32,
        flags: 0,
    };

    let mut ifr_name: [libc::c_char; libc::IFNAMSIZ] = [0; libc::IFNAMSIZ];
    for (source, target) in interface_name.as_bytes().iter().zip(ifr_name.iter_mut()) {
        *target = *source as libc::c_char;
    }

    let ifreq = libc::ifreq {
        ifr_name,
        ifr_ifru: libc::__c_anonymous_ifr_ifru {
            ifru_data: (&mut ts_cfg as *mut _) as *mut libc::c_char,
        },
    };

    let err = unsafe {
        // Not useless conversion because aarch64 has different type
        #[allow(clippy::useless_conversion)]
        libc::ioctl(sockfd, libc::SIOCSHWTSTAMP.try_into().unwrap(), &ifreq)
    };
    if err < 0 {
        // XXX: While ioctl failed, SW timestamp is still enabled.
        eprintln!("ioctl SIOCSHWTSTAMP failed: {}", Error::last_os_error());
        eprintln!("But SW timestamp by kernel is still enabled.")
    }

    if Option::is_some(&iov) {
        const CONTROLSIZE: usize = 1024;
        let mut control: [libc::c_char; CONTROLSIZE] = [0; CONTROLSIZE];

        let msg = msghdr {
            msg_iov: iov.unwrap(),
            msg_iovlen: 1,
            msg_control: control.as_mut_ptr() as *mut libc::c_void,
            msg_controllen: CONTROLSIZE,
            msg_flags: 0,
            msg_name: std::ptr::null_mut::<libc::c_void>(),
            msg_namelen: 0,
        };

        sock.msg = Some(msg);
    }

    Ok(())
}

pub fn get_tx_timestamp(sock: &TsnSocket) -> Result<time::Timespec, Error> {
    let sockfd = sock.fd;

    let buf: [u8; 256] = [0u8; 256];
    let buflen = std::mem::size_of_val(&buf);

    let control: [u8; 256] = [0u8; 256];

    let iov = libc::iovec {
        iov_base: buf.as_ptr() as *mut libc::c_void,
        iov_len: buflen,
    };

    let msg: libc::msghdr = unsafe {
        // Avoid private field not provided error
        let mut msg: libc::msghdr = std::mem::MaybeUninit::zeroed().assume_init();

        msg.msg_name = std::ptr::null_mut();
        msg.msg_namelen = 0;
        msg.msg_iov = &iov as *const _ as *mut libc::iovec;
        msg.msg_iovlen = 1;
        msg.msg_control = control.as_ptr() as *mut libc::c_void;
        msg.msg_controllen = {
            // aarch64 has msg_controllen as u32, not usize
            #[allow(clippy::useless_conversion)]
            std::mem::size_of_val(&control).try_into().unwrap()
        };
        msg.msg_flags = 0;
        msg
    };

    let pfd = libc::pollfd {
        fd: sockfd,
        events: libc::POLLPRI,
        revents: 0,
    };

    let res = unsafe { libc::poll(&pfd as *const _ as *mut libc::pollfd, 1, 1000) };

    match res {
        0 => {
            return Err(Error::new(ErrorKind::TimedOut, "poll timeout"));
        }
        res if res < 0 => {
            return Err(Error::last_os_error());
        }
        _ => {}
    }

    // XXX: IDK why but this doesn't work on NXP
    // Commenting this out for now
    // if !(pfd.revents & libc::POLLPRI) != 0 {
    //     return Err(Error::new(ErrorKind::Other, format!("unexpected revents {}", pfd.revents)));
    // }

    // Poll done. Now read the timestamp

    let cnt = unsafe {
        libc::recvmsg(
            sockfd,
            &msg as *const _ as *mut libc::msghdr,
            libc::MSG_ERRQUEUE,
        )
    };

    if cnt < 0 {
        return Err(Error::last_os_error());
    }

    // Recvmsg done. Parse the timestamp
    let mut cm = unsafe { libc::CMSG_FIRSTHDR(&msg) };
    // Loop while cm is not null
    while !cm.is_null() {
        let cmsg_level = unsafe { (*cm).cmsg_level };
        let cmsg_type = unsafe { (*cm).cmsg_type };

        if cmsg_level == libc::SOL_SOCKET && cmsg_type == libc::SO_TIMESTAMPING {
            let ts = unsafe {
                let ts = libc::CMSG_DATA(cm) as *const [libc::timespec; 3];
                *ts
            };

            // 0 - SW tx timestamp
            // 1 - Legacy HW tx timestamp
            // 2 - HW tx timestamp
            let ts = match ts {
                ts if ts[2].tv_sec != 0 || ts[2].tv_nsec != 0 => ts[2],
                ts if ts[1].tv_sec != 0 || ts[1].tv_nsec != 0 => ts[1],
                ts if ts[0].tv_sec != 0 || ts[0].tv_nsec != 0 => {
                    // Log warning
                    eprintln!("SW TX timestamp(from driver) used");
                    ts[0]
                }
                _ => unreachable!(),
            };
            return Ok(time::Timespec {
                tv_sec: ts.tv_sec,
                tv_nsec: ts.tv_nsec,
            });
        }

        cm = unsafe { libc::CMSG_NXTHDR(&msg, cm) };
    }

    Err(Error::new(ErrorKind::NotFound, "No timestamp found"))
}

pub fn get_rx_timestamp(sock: &TsnSocket) -> Result<time::Timespec, Error> {
    let msg = match sock.msg {
        Some(msg) => msg,
        _ => {
            // TODO: It might be better to return sw timestamp
            return Err(Error::new(ErrorKind::InvalidInput, "rx timestamp not enabled"));
        }
    };
    let mut tend: libc::timespec = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let mut cmsg: *mut cmsghdr;

    let mut cmsg_level;
    let mut cmsg_type;
    unsafe {
        cmsg = libc::CMSG_FIRSTHDR(&msg);
    }
    while !cmsg.is_null() {
        unsafe {
            cmsg_level = (*cmsg).cmsg_level;
            cmsg_type = (*cmsg).cmsg_type;
            if cmsg_level != libc::SOL_SOCKET {
                cmsg = libc::CMSG_NXTHDR(&msg, cmsg);
                continue;
            }
        }
        if libc::SO_TIMESTAMPING == cmsg_type {
            unsafe {
                libc::memcpy(
                    &mut tend as *mut _ as *mut libc::c_void,
                    libc::CMSG_DATA(cmsg).add(size_of::<libc::timespec>() * 2) as *const libc::c_void,
                    mem::size_of_val(&tend),
                );
            }
            let time: time::Timespec = time::Timespec {
                tv_sec: tend.tv_sec,
                tv_nsec: tend.tv_nsec,
            };
            return Ok(time);
        }
    }
    Err(Error::new(ErrorKind::Other, "failed to get timestamp"))
}

pub fn timespecff_diff(start: &mut TimeSpec, stop: &mut TimeSpec, result: &mut TimeSpec) {
    if start.tv_sec() > stop.tv_sec()
        || (start.tv_sec() == stop.tv_sec() && start.tv_nsec() > stop.tv_nsec())
    {
        timespecff_diff(start, stop, result);
        let result_sec: TimeSpec = TimeValLike::seconds(result.tv_sec());
        let result_nsec: TimeSpec = TimeValLike::nanoseconds(result.tv_nsec());
        *result = (result_sec * -1) + result_nsec;
        return;
    }

    if (stop.tv_nsec() - start.tv_nsec()) < 0 {
        let result_sec: TimeSpec = TimeValLike::seconds(stop.tv_sec() - start.tv_sec() - 1);
        let result_nsec: TimeSpec =
            TimeValLike::nanoseconds(stop.tv_nsec() - start.tv_nsec() + 1000000000);

        *result = result_sec + result_nsec;
    } else {
        let result_sec: TimeSpec = TimeValLike::seconds(stop.tv_sec() - start.tv_sec());
        let result_nsec: TimeSpec = TimeValLike::nanoseconds(stop.tv_nsec() - start.tv_nsec());

        *result = result_sec + result_nsec;
    }
}

fn open_shmem(shm_name: &str) -> Result<*mut c_void, String> {
    let shm_fd = shm_open(
        shm_name,
        OFlag::O_CREAT | OFlag::O_RDWR,
        Mode::S_IRWXU | Mode::S_IRWXG | Mode::S_IRWXO,
    );
    let shm_fd = match shm_fd {
        Ok(v) => v,
        Err(_) => return Err(format!("Open shmem fails: {}", Error::last_os_error())),
    };
    let shm_ptr = unsafe {
        ftruncate(shm_fd, SHM_SIZE as libc::off_t);
        mmap(
            None,
            NonZeroUsize::new_unchecked(SHM_SIZE),
            ProtFlags::PROT_READ | ProtFlags::PROT_WRITE,
            MapFlags::MAP_SHARED,
            shm_fd,
            0,
        )
    };
    let shm_ptr = match shm_ptr {
        Ok(v) => v,
        Err(_) => return Err(format!("Open shmem fails: {}", Error::last_os_error())),
    };

    unsafe { msync(shm_ptr, SHM_SIZE, MS_SYNC) };

    Ok(shm_ptr)
}

fn read_shmem(shm_name: &str) -> Result<Vec<u32>, String> {
    let shm_ptr = open_shmem(shm_name)?;

    let mut vec_data: Vec<u32> = unsafe {
        let data = slice::from_raw_parts(shm_ptr as *const u8, SHM_SIZE);
        slice::from_raw_parts(
            data.to_vec().as_ptr() as *const u32,
            data.len() / size_of::<u32>(),
        )
        .to_vec()
    };
    vec_data.retain(|&x| x != 0);
    unsafe {
        match munmap(shm_ptr, SHM_SIZE) {
            Ok(_) => Ok(vec_data),
            Err(_) => Err(format!("Read shmem fails: {}", Error::last_os_error())),
        }
    }
}

fn write_shmem(shm_name: &str, input: &Vec<u32>) -> Result<String, String> {
    let shm_ptr = open_shmem(shm_name)?;
    let shm_byte = unsafe {
        slice::from_raw_parts(input.as_ptr() as *const u8, size_of::<u32>() * input.len())
    };
    let addr = shm_ptr as *mut u8;
    for (i, item) in shm_byte.iter().enumerate() {
        unsafe { *addr.add(i) = *item };
    }
    unsafe {
        match munmap(shm_ptr, SHM_SIZE) {
            Ok(_) => Ok("".to_string()),
            Err(_) => Err(format!("Write shmem fails: {}", Error::last_os_error())),
        }
    }
}

fn lock_shmem(shm_fd: &i32) -> Result<i32, String> {
    let lock = flock {
        l_type: libc::F_WRLCK as i16,
        l_whence: libc::SEEK_SET as i16,
        l_start: 0,
        l_len: 0,
        l_pid: 0,
    };
    match fcntl(*shm_fd, F_SETLKW(&lock)) {
        Ok(v) => Ok(v),
        Err(_) => Err(format!("Lock shmem fails: {}", Error::last_os_error())),
    }
}

fn unlock_shmem(shm_fd: &i32) -> Result<i32, String> {
    let lock = flock {
        l_type: libc::F_UNLCK as i16,
        l_whence: libc::SEEK_SET as i16,
        l_start: 0,
        l_len: 0,
        l_pid: 0,
    };
    match fcntl(*shm_fd, F_SETLKW(&lock)) {
        Ok(v) => Ok(v),
        Err(_) => Err(format!("Unlock shmem fails: {}", Error::last_os_error())),
    }
}

fn get_config(ifname: &str) -> Result<config::Config, String> {
    let config_path = env::var("CONFIG_PATH").unwrap_or("./config.yaml".to_string());
    let configs = config::read_config(&config_path);
    let configs = match configs {
        Ok(v) => v,
        Err(_) => return Err(format!("Read config fails: {}", Error::last_os_error())),
    };
    let config = configs.get(ifname);
    match config {
        Some(v) => Ok(v.clone()),
        None => Err(format!("No config for {}", ifname)),
    }
}

fn get_shmem_name(ifname: &str, vlanid: u16) -> String {
    format!("libtsn_vlan_{}", vlan::get_vlan_name(ifname, vlanid))
}

fn get_shmem_fd(shm_name: &str) -> Result<i32, String> {
    match shm_open(
        shm_name,
        OFlag::O_CREAT | OFlag::O_RDWR,
        Mode::S_IRWXU | Mode::S_IRWXG | Mode::S_IRWXO,
    ) {
        Ok(v) => Ok(v),
        Err(_) => Err(format!("Open shmem fails: {}", Error::last_os_error())),
    }
}
