//! Telemetry demo binary: the single writer of the volatile telemetry keys
//! in `cache.lmdb` (docs/design.md ch.2.5).
//!
//! Every `sampleIntervalSec` (read from `config.lmdb`, read-only, default 60
//! if absent) it samples a CPU temperature and writes:
//!   - `cpuTempMilliC`   : Int32, milli-degrees Celsius
//!   - `sampleCount`     : Uint64, running sample counter
//!   - `lastUpdateEpoch` : Uint64, unix seconds of the last write
//!   - `statusText`      : Bytes, a short human-readable status string

use agentx_rust_app::storage::{CacheStore, ConfigStore, Value};
use clap::Parser;
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const THERMAL_ZONE_PATH: &str = "/sys/class/thermal/thermal_zone0/temp";
const DEFAULT_SAMPLE_INTERVAL_SEC: u64 = 60;

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
    rng.gen_range(35_000..55_000)
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
    cache.put_uint64("sampleCount", sample_count)?;
    cache.put_uint64("lastUpdateEpoch", now)?;
    let status = format!("ok temp={temp_milli_c}mC count={sample_count}");
    cache.put_bytes("statusText", status.as_bytes())?;

    println!("[telemetry] sample={sample_count} tempMilliC={temp_milli_c} epoch={now}");
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    let cache = CacheStore::open(&args.cache_dir, Some(args.mapsize))?;
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
