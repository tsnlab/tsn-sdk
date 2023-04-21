use core::slice;
use nix::net::if_::if_nametoindex;
use nix::sys::socket::msghdr;
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
use std::io::Error;
use std::{mem, str};
use std::{mem::size_of, num::NonZeroUsize, os::raw::c_void, process, time::Duration};

extern crate socket;

pub struct TsnSocket {
    pub fd: i32,
    pub ifname: String,
    pub vlanid: u16,
}

mod cbs;
mod config;
mod tas;
mod vlan;

const SHM_SIZE: usize = 128;
const CONFIG_PATH: &str = "./config.yaml";

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

    pub fn close(&mut self) -> Result<(), String> {
        sock_close(self)
    }
}

#[derive(Eq, PartialEq)]
enum LockKind {
    Lock,
    Unlock,
}

fn create_vlan(ifname: &str, vlanid: u16) -> Result<i32, i32> {
    let configs = config::read_config(CONFIG_PATH).unwrap();
    let config = configs.get(ifname).unwrap();
    let shm_name = format!("{}.{}", ifname, vlanid);
    let shm_fd = shm_open(
        &*shm_name,
        OFlag::O_CREAT | OFlag::O_RDWR,
        Mode::S_IRWXU | Mode::S_IRWXG | Mode::S_IRWXO,
    )
    .unwrap();
    lock_shmem(&shm_fd, LockKind::Lock);
    let mut vlan_vec = read_shmem(&shm_name);
    let mut result = Ok(0);
    if vlan_vec.is_empty() {
        result = vlan::create_vlan(config, ifname, vlanid);
    }
    vlan_vec.push(process::id());
    write_shmem(&shm_name, &vlan_vec);
    lock_shmem(&shm_fd, LockKind::Unlock);
    result
}

fn delete_vlan(ifname: &str, vlanid: u16) -> Result<i32, i32> {
    let shm_name = format!("{}.{}", ifname, vlanid);
    let shm_fd = shm_open(
        &*shm_name,
        OFlag::O_CREAT | OFlag::O_RDWR,
        Mode::S_IRWXU | Mode::S_IRWXG | Mode::S_IRWXO,
    )
    .unwrap();
    let mut result = Ok(0);
    lock_shmem(&shm_fd, LockKind::Lock);
    let mut vlan_vec = read_shmem(&shm_name);

    for i in 0..vlan_vec.len() {
        if vlan_vec[i] == process::id() {
            vlan_vec.remove(i);
            break;
        }
    }
    let mut exit_flag = false;
    if vlan_vec.is_empty() {
        exit_flag = true;
    }
    vlan_vec.resize(SHM_SIZE / 4, 0);
    write_shmem(&shm_name, &vlan_vec);
    if exit_flag {
        shm_unlink(&*shm_name).unwrap();
        result = vlan::delete_vlan(ifname, vlanid);
    }
    result
}

pub fn sock_open(
    ifname: &str,
    vlanid: u16,
    priority: u32,
    proto: u16,
) -> Result<TsnSocket, String> {
    match create_vlan(ifname, vlanid) {
        Ok(v) => println!("{}", v),
        Err(_) => {
            return Err(format!("Create vlan fails {}", Error::last_os_error()));
        }
    }

    let sock;
    let mut res;
    let vlan_ifname = format!("{}.{}", ifname, vlanid);
    let ifindex = if_nametoindex(vlan_ifname.as_bytes()).expect("vlan_ifname index");
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
    })
}

pub fn sock_close(sock: &mut TsnSocket) -> Result<(), String> {
    match delete_vlan(&sock.ifname, sock.vlanid) {
        Ok(v) => {
            println!("{}", v);
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

fn open_shmem(shm_name: &str) -> *mut c_void {
    let shm_fd = shm_open(
        shm_name,
        OFlag::O_CREAT | OFlag::O_RDWR,
        Mode::S_IRWXU | Mode::S_IRWXG | Mode::S_IRWXO,
    )
    .unwrap();
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
        .unwrap()
    };
    unsafe { msync(shm_ptr, SHM_SIZE, MS_SYNC) };

    shm_ptr
}

fn read_shmem(shm_name: &str) -> Vec<u32> {
    let shm_ptr = open_shmem(shm_name);

    let mut vec_data: Vec<u32> = unsafe {
        let data = slice::from_raw_parts(shm_ptr as *const u8, SHM_SIZE);
        slice::from_raw_parts(data.to_vec().as_ptr() as *const u32, data.len() / 4).to_vec()
    };
    vec_data.retain(|&x| x != 0);
    vec_data.retain(|x| kill(Pid::from_raw(*x as i32), None).is_ok());
    unsafe { munmap(shm_ptr, SHM_SIZE).unwrap() };

    vec_data
}

fn write_shmem(shm_name: &str, input: &Vec<u32>) {
    let shm_ptr = open_shmem(shm_name);
    let shm_byte = unsafe {
        slice::from_raw_parts(input.as_ptr() as *const u8, size_of::<u32>() * input.len())
    };
    let addr = shm_ptr as *mut u8;
    for (i, item) in shm_byte.iter().enumerate() {
        unsafe { *addr.add(i) = *item };
    }
    unsafe { munmap(shm_ptr, SHM_SIZE).unwrap() };
}

fn lock_shmem(shm_fd: &i32, kind: LockKind) {
    let mut lock = flock {
        l_type: libc::F_WRLCK as i16,
        l_whence: libc::SEEK_SET as i16,
        l_start: 0,
        l_len: 0,
        l_pid: 0,
    };
    if kind.eq(&LockKind::Lock) {
        lock.l_type = libc::F_WRLCK as i16;
    } else {
        lock.l_type = libc::F_UNLCK as i16;
    }
    fcntl(*shm_fd, F_SETLKW(&lock)).unwrap();
}
