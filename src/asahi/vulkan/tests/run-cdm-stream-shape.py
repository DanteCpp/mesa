#!/usr/bin/env python3
# Copyright 2026 Aurora Silicon
# SPDX-License-Identifier: MIT
"""Offline compute-stream shape check for transfer command buffers.

Runs hk_cdm_stream_shape.c on the asahi drm-shim for every chip profile and
case, keeps the shim's submit log of the measured command buffer, and then
either prints the per-submit shape table or compares two runs byte for byte.

  run   --probe BIN --icd libvulkan_asahi.so --shim libasahi_noop_drm_shim.so
        --out DIR [--chips ...] [--cases ...] [--jobs N]
  table DIR CHIP          per case: submits, commands, stream lengths
  check-sealed DIR CHIP   every measured compute stream is 48 bytes, one
                          command per submit, one submit per transfer
  compare DIR_A DIR_B CHIP...   measured submit logs byte-equal per case
  split OLD_DIR NEW_DIR CHIP    each new 48-byte stream equals its transfer's
                          44 bytes inside the old fused stream + terminator
  vm-status --probe --icd --shim --out [--chip]
                          vkWaitForFences after a submit whose fence signals:
                          VK_SUCCESS, or VK_ERROR_DEVICE_LOST once the shim
                          publishes a sticky VM error (the drm winsys path)

Nothing executes on a GPU; the shim accepts and drops the submits.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys

CHIPS = ["g17g-compile-only", "g18g", "g17p", "g16g", "g16s", "g15s", "g15c",
         "g14g", "g13g", "g13x"]
CASES = ["F", "C", "K", "U", "FC", "CF", "KF", "FCKFC", "FFFFF", "CCCCC",
         "FUCKF"]
BARRIERS = ["nobarrier", "barrier"]


def chip_env(chip):
    if chip == "g17g-compile-only":
        return {"ASAHI_M5_BRINGUP": "1", "ASAHI_SHIM_G17G_ACCEPT_SUBMIT": "1",
                "ASAHI_SHIM_SOFT_FAULTS": "1"}
    if chip == "g18g":
        return {"ASAHI_SHIM_G18G_ACCEPT_SUBMIT": "1"}
    return {}


def run_one(args, chip, case, barrier):
    d = os.path.join(args.out, chip, f"{case}-{barrier}")
    os.makedirs(d, exist_ok=True)
    for f in ("measured.log", "setup.log"):
        p = os.path.join(d, f)
        if os.path.exists(p):
            os.unlink(p)
    env = {"PATH": "/usr/bin:/bin", "HOME": "/tmp",
           "LD_PRELOAD": args.shim, "ASAHI_SHIM_CHIP": chip,
           "MESA_SHADER_CACHE_DISABLE": "true",
           "ASAHI_SHIM_SUBMIT_LOG": os.path.join(d, "measured.log"),
           "HK_CDM_SETUP_LOG": os.path.join(d, "setup.log"),
           "ASAHI_SHIM_SUBMIT_LOG_CDM_BYTES": "1"}
    env.update(chip_env(chip))
    with open(os.path.join(d, "stdout.txt"), "w") as out, \
         open(os.path.join(d, "stderr.txt"), "w") as err:
        rc = subprocess.call([args.probe, args.icd, case, barrier], env=env,
                             stdout=out, stderr=err, timeout=300)
    with open(os.path.join(d, "rc.txt"), "w") as f:
        f.write(f"rc={rc}\n")
    return chip, case, barrier, rc


def cmd_run(args):
    jobs = [(c, k, b) for c in args.chips for k in args.cases for b in BARRIERS]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for chip, case, barrier, rc in ex.map(lambda j: run_one(args, *j), jobs):
            print(f"{chip} {case} {barrier} rc={rc}")


def parse(path):
    """Submits of a measured log: list of lists of commands; a command is
    (kind, length, bytes-hex or None)."""
    subs = []
    if not os.path.exists(path):
        return None
    for line in open(path):
        if line.startswith("submit "):
            subs.append([])
        elif line.startswith("  cmd "):
            kind = line.split()[1]
            length = None
            for f in line.split():
                if f.startswith("cdm_len="):
                    length = int(f[8:])
            subs[-1].append([kind, length, None])
        elif line.startswith("    cdm_bytes "):
            subs[-1][-1][2] = line.split()[1]
    return subs


def case_dir(root, chip, case, barrier):
    return os.path.join(root, chip, f"{case}-{barrier}")


def rc_of(d):
    return open(os.path.join(d, "rc.txt")).read().strip()


def cmd_table(args):
    print(f"chip {args.chip}: transfers -> submits [commands: C<len>=compute "
          f"stream bytes, A=attachment set, R=render]")
    for case in CASES:
        for barrier in BARRIERS:
            d = case_dir(args.dir, args.chip, case, barrier)
            subs = parse(os.path.join(d, "measured.log")) or []
            shape = " ".join(
                "[" + ",".join(("C%d" % c[1]) if c[0] == "compute" else
                               ("A" if c[0].startswith("set-") else "R")
                               for c in s) + "]" for s in subs)
            print(f"  {case:6} N={len(case)} {barrier:9} {rc_of(d)} "
                  f"submits={len(subs)} {shape}")


def cmd_check_sealed(args):
    bad = 0
    for case in CASES:
        for barrier in BARRIERS:
            d = case_dir(args.dir, args.chip, case, barrier)
            subs = parse(os.path.join(d, "measured.log")) or []
            computes = [c for s in subs for c in s if c[0] == "compute"]
            ok = (rc_of(d) == "rc=0" and len(subs) == len(case) and
                  all(sum(c[0] == "compute" for c in s) == 1 and
                      sum(c[0] == "render" for c in s) == 0 for s in subs) and
                  len(computes) == len(case) and
                  all(c[1] == 48 and c[2] and len(c[2]) == 96 and
                      c[2].endswith("00000040") for c in computes))
            print(f"  {'ok  ' if ok else 'FAIL'} {case:6} {barrier:9} "
                  f"submits={len(subs)} lens={[c[1] for c in computes]}")
            bad += not ok
    print(f"check-sealed {args.chip}: {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


def cmd_compare(args):
    bad = 0
    for chip in args.chips:
        same = 0
        for case in CASES:
            for barrier in BARRIERS:
                a = case_dir(args.a, chip, case, barrier)
                b = case_dir(args.b, chip, case, barrier)
                ok = True
                for f in ("rc.txt", "measured.log", "stdout.txt"):
                    pa, pb = os.path.join(a, f), os.path.join(b, f)
                    if not os.path.exists(pa) or not os.path.exists(pb) or \
                       open(pa, "rb").read() != open(pb, "rb").read():
                        ok = False
                if ok:
                    same += 1
                else:
                    print(f"  DIFF {chip} {case} {barrier}")
                    bad += 1
        print(f"  {chip:18} {same}/{len(CASES) * len(BARRIERS)} case logs "
              f"byte-equal (rc, stdout, measured submit log incl. stream bytes)")
    print(f"compare: {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


def cmd_split(args):
    """Old: one fused stream (44*N + 4). New: N streams of 48. Each new
    stream must be the old stream's i-th 44-byte record + terminator."""
    bad = 0
    for case in CASES:
        for barrier in BARRIERS:
            old = parse(os.path.join(case_dir(args.old, args.chip, case,
                                              barrier), "measured.log")) or []
            new = parse(os.path.join(case_dir(args.new, args.chip, case,
                                              barrier), "measured.log")) or []
            oc = [c for s in old for c in s if c[0] == "compute"]
            nc = [c for s in new for c in s if c[0] == "compute"]
            ok = len(oc) == 1 and len(nc) == len(case) and oc[0][2]
            if ok:
                fused = bytes.fromhex(oc[0][2])
                term = fused[-4:]
                recs = [fused[i * 44:(i + 1) * 44] for i in range(len(case))]
                ok = (len(fused) == 44 * len(case) + 4 and
                      all(nc[i][2] and bytes.fromhex(nc[i][2]) == recs[i] + term
                          for i in range(len(case))))
            barrier_words = [rec[40:44].hex() for rec in
                             ([bytes.fromhex(c[2])[:44] for c in nc] if ok else [])]
            print(f"  {'ok  ' if ok else 'FAIL'} {case:6} {barrier:9} "
                  f"barrier words {barrier_words}")
            bad += not ok
    print(f"split {args.chip}: {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


def cmd_vm_status(args):
    """vkWaitForFences on a submit whose fence signals: VK_SUCCESS without
    VM_STATUS and with a clean status, VK_ERROR_DEVICE_LOST (-4) once a
    sticky VM error is published at that submit."""
    cases = [("no-vm-status", {}, 0),
             ("vm-status-clean", {"ASAHI_SHIM_VM_STATUS": "1"}, 0),
             ("vm-status-error", {"ASAHI_SHIM_VM_STATUS": "1",
                                  "HK_CDM_VM_ERROR": "-5"}, -4)]
    bad = 0
    for name, extra, want in cases:
        d = os.path.join(args.out, name)
        os.makedirs(d, exist_ok=True)
        env = {"PATH": "/usr/bin:/bin", "HOME": "/tmp",
               "LD_PRELOAD": args.shim, "ASAHI_SHIM_CHIP": args.chip,
               "MESA_SHADER_CACHE_DISABLE": "true",
               "ASAHI_SHIM_SUBMIT_LOG": os.path.join(d, "measured.log"),
               "HK_CDM_SETUP_LOG": os.path.join(d, "setup.log"),
               "HK_CDM_FENCE_WAIT": "1"}
        env.update(chip_env(args.chip))
        env.update(extra)
        r = subprocess.run([args.probe, args.icd, "F", "nobarrier"], env=env,
                           capture_output=True, text=True, timeout=300)
        open(os.path.join(d, "stdout.txt"), "w").write(r.stdout)
        open(os.path.join(d, "stderr.txt"), "w").write(r.stderr)
        got = None
        for line in r.stdout.splitlines():
            if line.startswith("WAIT result="):
                got = int(line.split("=")[1])
        ok = r.returncode == 0 and got == want
        bad += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name:16} vkWaitForFences={got} "
              f"(want {want}) rc={r.returncode}")
    print(f"vm-status {args.chip}: {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("vm-status")
    v.add_argument("--probe", required=True)
    v.add_argument("--icd", required=True)
    v.add_argument("--shim", required=True)
    v.add_argument("--out", required=True)
    v.add_argument("--chip", default="g17g-compile-only")
    r = sub.add_parser("run")
    r.add_argument("--probe", required=True)
    r.add_argument("--icd", required=True)
    r.add_argument("--shim", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--chips", nargs="*", default=CHIPS)
    r.add_argument("--cases", nargs="*", default=CASES)
    r.add_argument("--jobs", type=int, default=6)
    t = sub.add_parser("table")
    t.add_argument("dir")
    t.add_argument("chip")
    s = sub.add_parser("check-sealed")
    s.add_argument("dir")
    s.add_argument("chip")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("chips", nargs="+")
    x = sub.add_parser("split")
    x.add_argument("old")
    x.add_argument("new")
    x.add_argument("chip")
    args = p.parse_args()
    return {"run": cmd_run, "table": cmd_table, "check-sealed": cmd_check_sealed,
            "compare": cmd_compare, "split": cmd_split,
            "vm-status": cmd_vm_status}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
