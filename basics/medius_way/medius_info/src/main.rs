//! medius_info: read-only state dump of a Medius box (replacement firmware, NOT MAKCU).
//!
//! Prints what the web dashboard shows on Overview and Options, via the host library's queries.
//! Sends no movement, buttons, locks or option writes.
//!
//! Usage: medius_info [COMx]      (no argument: first box found)

use medius::Device;

fn show<T: std::fmt::Debug>(label: &str, r: medius::Result<T>) {
    match r {
        Ok(v) => println!("{label:<18} {v:?}"),
        Err(e) => println!("{label:<18} ERROR: {e}"),
    }
}

fn main() {
    println!("== boxes ==");
    for b in Device::list() {
        println!(
            "{}  name={}  port={}  serial={}  {}",
            b.id(),
            b.name(),
            b.port.path,
            b.serial().unwrap_or("-"),
            b.version
        );
    }

    let device = match std::env::args().nth(1) {
        Some(p) => Device::open(p),
        None => Device::find(),
    };
    let device = match device {
        Ok(d) => d,
        Err(e) => {
            eprintln!("open failed: {e}");
            std::process::exit(2);
        }
    };

    println!("\n== box ==");
    match device.query_version() {
        Ok(v) => println!(
            "version            fw {}.{}.{}  proto {}  mac {}  name {:?}",
            v.fw_major, v.fw_minor, v.fw_patch, v.proto_ver, v.mac_hex(), v.name
        ),
        Err(e) => println!("version            ERROR: {e}"),
    }
    show("health", device.query_health());

    println!("\n== clone ==");
    match device.device_info() {
        Ok(i) => println!("device_info        {i}  kind={}  {i:?}", i.kind),
        Err(e) => println!("device_info        ERROR: {e}"),
    }
    show("caps", device.caps());
    show("rate", device.query_rate());

    println!("\n== options ==");
    show("imperfect", device.query_imperfect());
    show("movement_riding", device.query_movement_riding());
    show("bearing", device.query_bearing());
    show("render", device.query_render());
    show("spread", device.query_spread());
    show("emit_pace", device.query_emit_pace());

    println!("\n== runtime ==");
    show("locks", device.query_locks());
    show("catch", device.query_catch());
    show("stats", device.query_stats());
    println!("{:<18} {:?}", "host counters", device.counters());
}
