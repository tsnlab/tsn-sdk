use std::time::{Duration, SystemTime, UNIX_EPOCH};

static mut ERROR_CLOCK_GETTIME: Duration = Duration::new(1, 0);
static mut ERROR_NANOSLEEP: Duration = Duration::new(1, 0);

#[derive(Debug)]
pub struct Timespec {
    pub tv_sec: i64,
    pub tv_nsec: i64,
}

fn is_analysed() -> bool {
    unsafe {
        let error_clock_gettime = ERROR_CLOCK_GETTIME;
        let error_nanosleep = ERROR_NANOSLEEP;
        error_clock_gettime.as_secs() != 1 && error_nanosleep.as_secs() != 1
    }
}

pub fn tsn_time_analyze() {
    if is_analysed() {
        return;
    }

    eprintln!("Calculating sleep errors");

    const COUNT: i32 = 10;

    // Analyze clock_gettime
    // expanded loop, because of for-loop increases time error
    let start = SystemTime::now();
    let end = {
        let mut _end: SystemTime;
        _end = SystemTime::now();
        _end = SystemTime::now();
        _end = SystemTime::now();
        _end = SystemTime::now();
        _end = SystemTime::now();
        _end = SystemTime::now();
        _end = SystemTime::now();
        _end = SystemTime::now();
        _end = SystemTime::now();
        SystemTime::now()
    };
    let diff = end
        .duration_since(start)
        .expect("Failed to calculate time difference");
    unsafe {
        ERROR_CLOCK_GETTIME = Duration::from_nanos((diff.subsec_nanos() / COUNT as u32).into());
    };
    // Analyze nanosleep
    let request = Duration::new(1, 0);
    let mut diff = Duration::new(0, 0);
    for _ in 0..COUNT {
        let start = SystemTime::now();
        std::thread::sleep(request);
        let end = SystemTime::now();
        diff = diff.saturating_add(
            end.duration_since(start)
                .expect("Failed to calculate time difference"),
        );
    }

    unsafe {
        ERROR_NANOSLEEP = Duration::from_nanos((diff.subsec_nanos() / COUNT as u32) as u64);
    }
}

pub fn tsn_time_sleep_until(endtime: &Duration) -> Result<i64, i64> {
    let now = SystemTime::now();

    let now = now.duration_since(UNIX_EPOCH).unwrap();
    // If already future, Don't need to sleep
    if endtime.saturating_sub(now) == Duration::new(0, 0) {
        return Ok(0);
    }
    let mut diff = endtime.saturating_sub(now);
    if diff < unsafe { ERROR_NANOSLEEP } {
        std::thread::sleep(diff);
    }

    unsafe {
        loop {
            let now = SystemTime::now();
            let now = now.duration_since(UNIX_EPOCH).unwrap();
            diff = endtime.saturating_sub(now);
            if diff < ERROR_CLOCK_GETTIME {
                break;
            }
        }
    }
    Ok(0)
}
