"""Run paired M1 Max indirect-draw workloads. Write one JSON record per process."""
import argparse
import datetime
import gzip
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

CASES = [
    ("mdi", ["--test", "render.mdi", "--depths", "4", "--param", "draws=10000",
             "--param", "bindless=1024"]),
    ("cull25", ["--test", "compute.cull_mdi", "--depths", "2", "--param",
                "objs=16384", "--param", "visible=25"]),
    ("cull75", ["--test", "compute.cull_mdi", "--depths", "2", "--param",
                "objs=16384", "--param", "visible=75"]),
    ("single", ["--test", "render.mdi", "--depths", "4", "--param", "draws=1",
                "--param", "bindless=1024"]),
]
STATS = Path("/sys/class/drm/renderD128/device/agx_stats")


def snapshot():
    return {k: int(v) for k, v in (line.split() for line in STATS.read_text().splitlines())}


def measure(options, cache, arm, name, case, pair):
    counters = cache / "submit-counts.bin"
    counters.write_bytes(bytes(32))
    env = dict(os.environ, VK_DRIVER_FILES=str(getattr(options, arm).resolve()),
               MESA_SHADER_CACHE_DIR=str(cache / arm), HK_SYSMEM="53687091200")
    if options.counter:
        env.update(LD_PRELOAD=str(options.counter.resolve()), GB_OWN_GPU_COUNTERS=str(counters))
    cmd = [str(options.bench.resolve()), *case, "--warmup-ms", "300", "--runs", "5",
           "--run-ms", "250", "--timed-runs", "3"]
    stamp = datetime.datetime.now(datetime.timezone.utc).isoformat()
    idle_start = snapshot()
    time.sleep(0.3)
    before = snapshot()
    start = time.monotonic()
    run = subprocess.run(cmd, env=env, text=True, capture_output=True, check=True)
    duration = time.monotonic() - start
    after = snapshot()
    benchmark = json.loads(run.stdout)
    row = dict(case=name, arm=arm, pair=pair + 1, utc=stamp, command=cmd,
               duration_s=duration, idle_start=idle_start, before=before,
               after=after, benchmark=benchmark)
    if run.stderr:
        row["stderr"] = run.stderr
    if options.counter:
        jobs, render, compute, errors = struct.unpack("=4Q", counters.read_bytes())
        row.update(own_submits=jobs, render_commands=render, compute_commands=compute,
                   counter_errors=errors, extra_jobs=after["jobs"] - before["jobs"] - jobs)
        if not jobs or errors:
            raise RuntimeError(f"Invalid submission counts: {row}")
    if benchmark["status"] != "ok" or not benchmark["check"]["valid"]:
        raise RuntimeError(f"Invalid benchmark result: {row}")
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("bench", "base", "candidate", "scratch", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--counter", type=Path, help="optional DRM submission counter library")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(dir=args.scratch) as tmp, gzip.open(args.output, "xt") as output:
        for pair in range(6):
            arms = ("base", "candidate") if pair % 2 == 0 else ("candidate", "base")
            offset = pair % len(CASES)
            for name, case in CASES[offset:] + CASES[:offset]:
                for arm in arms:
                    row = measure(args, Path(tmp), arm, name, case, pair)
                    output.write(json.dumps(row, separators=(",", ":")) + "\n")
                    output.flush()
                    timing = row["benchmark"]["metrics"]["us_per_submit"]["median"]
                    print(f"pair {pair + 1} {name} {arm} {timing:.2f} us", flush=True)


if __name__ == "__main__":
    main()
