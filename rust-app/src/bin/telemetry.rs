//! Telemetry demo binary: the single writer of the volatile telemetry keys
//! in `cache.lmdb` (docs/design.md ch.2.5).
//!
//! Every `sampleIntervalSec` (read from `config.lmdb`, read-only, default 60
//! if absent) it samples a CPU temperature and writes:
//!   - `cpuTempMilliC`   : Int32, milli-degrees Celsius
//!   - `sampleCount`     : Uint32, running sample counter (Counter32)
//!   - `lastUpdateEpoch` : Uint32, unix seconds of the last write (Unsigned32)
//!   - `statusText`      : Bytes, a short human-readable status string
//!
//! and one row per sensor of the AGENTX-DEMO-MIB `sensorTable`
//! (docs/design.md 3.2), each row written as a single transaction:
//!   - `sensorName.<n>`        : Bytes,  DisplayString
//!   - `sensorTempMilliC.<n>`  : Int32,  milli-degrees Celsius
//!   - `sensorSampleCount.<n>` : Uint32, Counter32
//!
//! Rows of a telemetry table exist exactly while this writer keeps their
//! cells present: nothing seeds them, so a subagent starting against a fresh
//! tmpfs reports an empty table rather than sensors that may not be there.

use agentx_rust_app::storage::{CacheStore, ConfigStore, Value};
use clap::Parser;
use rand::rngs::StdRng;
use rand::{RngExt, SeedableRng};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const THERMAL_ZONE_PATH: &str = "/sys/class/thermal/thermal_zone0/temp";
const DEFAULT_SAMPLE_INTERVAL_SEC: u64 = 60;

/// The columns of AGENTX-DEMO-MIB sensorTable this app owns.
const SENSOR_COLUMNS: [&str; 3] = ["sensorName", "sensorTempMilliC", "sensorSampleCount"];

/// The sensors reported as rows of sensorTable, indexed by position + 1. A
/// real deployment would discover these; the demo keeps a fixed set so the
/// integration suite has something deterministic to walk.
const SENSORS: [&str; 3] = ["cpu-package", "board-inlet", "nvme0"];

#[derive(Parser, Debug)]
#[command(about = "AgentX Rust telemetry sampler (docs/design.md)")]
struct Args {
    /// Directory for the volatile cache.lmdb environment (tmpfs in production).
    #[arg(long, default_value = "/run/agentx-subagent/cache.lmdb")]
    cache_dir: PathBuf,

    /// Directory for the persistent, read-only config.lmdb environment.
    #[arg(long, default_value = "/var/lib/agentx-subagent/config.lmdb")]
    config_dir: PathBuf,

    /// LMDB map size in bytes for both environments.
    #[arg(long, default_value_t = agentx_rust_app::storage::DEFAULT_MAPSIZE)]
    mapsize: usize,

    /// Sample once and exit (used by tests instead of looping forever).
    #[arg(long)]
    once: bool,

    /// Stop after this many samples (default: run forever unless --once).
    #[arg(long)]
    iterations: Option<u64>,

    /// Seed for the synthetic temperature generator used when
    /// `/sys/class/thermal/thermal_zone0/temp` is not present, so tests are
    /// deterministic.
    #[arg(long, default_value_t = 42)]
    seed: u64,

    /// Override the sample interval instead of reading sampleIntervalSec
    /// from config.lmdb (handy for tests).
    #[arg(long)]
    interval_sec: Option<u64>,

    /// Remove the sensorTable rows this app owns and exit, without sampling.
    /// Rows of a telemetry table live only as long as their cells do, so this
    /// is what "the sensors went away" looks like to a manager walking it.
    #[arg(long)]
    clear_sensor_rows: bool,
}

/// Reads a CPU temperature in milli-degrees Celsius. Prefers the real sysfs
/// thermal zone; falls back to a seeded synthetic value so demo/test runs
/// without that sysfs path stay deterministic and reproducible.
fn sample_temp_milli_c(rng: &mut StdRng) -> i32 {
    if let Ok(contents) = std::fs::read_to_string(THERMAL_ZONE_PATH) {
        if let Ok(v) = contents.trim().parse::<i32>() {
            return v;
        }
    }
    // Plausible CPU idle/light-load range: 35.0C-55.0C, wobbling a little
    // sample to sample.
    rng.random_range(35_000..55_000)
}

fn read_sample_interval_sec(config_dir: &Path, mapsize: usize) -> u64 {
    match ConfigStore::open(config_dir, Some(mapsize)) {
        Ok(store) => match store.try_get("sampleIntervalSec") {
            Ok(Some(Value::Uint32(v))) => u64::from(v),
            Ok(Some(Value::Int32(v))) if v > 0 => v as u64,
            _ => DEFAULT_SAMPLE_INTERVAL_SEC,
        },
        Err(_) => DEFAULT_SAMPLE_INTERVAL_SEC,
    }
}

fn run_once(
    cache: &CacheStore,
    rng: &mut StdRng,
    sample_count: u64,
) -> Result<(), Box<dyn std::error::Error>> {
    let temp_milli_c = sample_temp_milli_c(rng);
    let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();

    cache.put_int32("cpuTempMilliC", temp_milli_c)?;
    // Both of these are 32-bit in the MIB (Counter32, Unsigned32) and the
    // generated handlers read them with storage_get_uint, which rejects a
    // differently typed cell -- an SNMP GET of a Uint64 cell would answer
    // genErr. sampleCount wraps, which is exactly Counter32's semantics;
    // lastUpdateEpoch saturates instead, because a wrapped timestamp would
    // read as a plausible date in the past rather than an obvious ceiling.
    cache.put_uint32("sampleCount", sample_count as u32)?;
    cache.put_uint32("lastUpdateEpoch", u32::try_from(now).unwrap_or(u32::MAX))?;
    let status = format!("ok temp={temp_milli_c}mC count={sample_count}");
    cache.put_bytes("statusText", status.as_bytes())?;

    // One transaction per row: a walk in flight then sees each row either
    // fully updated or not at all, instead of a row with holes.
    for (idx, name) in SENSORS.iter().enumerate() {
        let instance = [(idx + 1) as u32];
        // Each sensor wobbles around the package temperature, so the rows are
        // visibly distinct without needing three sysfs zones.
        let sensor_temp = temp_milli_c + (idx as i32) * 1_500;

        cache.put_row(
            &instance,
            &[
                ("sensorName", Value::Bytes(name.as_bytes().to_vec())),
                ("sensorTempMilliC", Value::Int32(sensor_temp)),
                // Counter32 in the MIB, so Uint32 here: the generated handler
                // reads this cell as a u32 and reports anything else as a
                // type mismatch.
                ("sensorSampleCount", Value::Uint32(sample_count as u32)),
            ],
        )?;
    }

    println!(
        "[telemetry] sample={sample_count} tempMilliC={temp_milli_c} epoch={now} sensorRows={}",
        SENSORS.len()
    );
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    let cache = CacheStore::open(&args.cache_dir, Some(args.mapsize))?;

    if args.clear_sensor_rows {
        for idx in 0..SENSORS.len() {
            cache.delete_row(&[(idx + 1) as u32], &SENSOR_COLUMNS)?;
        }
        println!("[telemetry] cleared {} sensorTable row(s)", SENSORS.len());
        return Ok(());
    }

    let interval_sec = args
        .interval_sec
        .unwrap_or_else(|| read_sample_interval_sec(&args.config_dir, args.mapsize));
    let mut rng = StdRng::seed_from_u64(args.seed);

    let mut sample_count: u64 = 0;
    loop {
        sample_count += 1;
        run_once(&cache, &mut rng, sample_count)?;

        if args.once {
            break;
        }
        if let Some(max) = args.iterations {
            if sample_count >= max {
                break;
            }
        }
        std::thread::sleep(Duration::from_secs(interval_sec));
    }

    Ok(())
}
