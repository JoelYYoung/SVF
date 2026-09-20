#!/usr/bin/env python3
"""Telemetry-off mechanism pilot, not an attribution profile of real AE."""
import argparse
import hashlib
import json
import math
import platform
import re
import statistics
import subprocess
from pathlib import Path


PHASES = {"lookup_hit": 500000, "lookup_miss": 500000, "copy": 500000,
          "write_unique": 100000, "write_cow": 100000,
          "join_changed": 1000, "churn_unique": 20000}
LAYOUTS = ("inline", "packed", "adaptive")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse(output, layout, phase, occupancy, rounds):
    fields = dict(re.findall(r"(\w+)=([^\s]+)", output))
    if (fields.get("representation") != f"chunk8/{layout}8" or
            fields.get("phase") != phase or
            fields.get("occupancy") != str(occupancy) or
            fields.get("rounds") != str(rounds) or
            fields.get("pages") != "64"):
        raise ValueError("phase identity/shape mismatch")
    seconds = float(fields["seconds"])
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("invalid timing")
    return seconds, int(fields["semantic_digest"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--round-scale", type=float, default=1)
    args = parser.parse_args()
    if args.output.exists() or args.repeats < 1 or not 0 < args.round_scale <= 100:
        parser.error("require a new output and positive bounded workload")
    state = {"scope": "synthetic local mechanism pilot; not AE frequency attribution or isolated end-to-end timing",
             "passed": False, "host": platform.platform(), "runner_sha256": digest(Path(__file__)),
             "round_scale": args.round_scale, "repeats": args.repeats,
             "binaries": {}, "records": [], "summary": []}

    def save():
        args.output.write_text(json.dumps(state, indent=2) + "\n")

    try:
        for layout in LAYOUTS:
            build = args.build_root / f"build-{layout}-off"
            exe = build / "bin/box-page-contract"
            cache = (build / "CMakeCache.txt").read_text()
            if "SVF_BOX_STORAGE_TELEMETRY:BOOL=OFF" not in cache:
                raise ValueError("timing build has telemetry enabled or unspecified")
            identity = subprocess.check_output([str(exe), "--identity"], text=True).strip()
            if identity != f"representation=chunk8/{layout}8":
                raise ValueError("wrong build representation")
            state["binaries"][layout] = {"identity": identity, "exe_sha256": digest(exe),
                "cache": cache, "libraries": {p.name: digest(p) for p in
                    (build / "lib").glob("*AbstractDomainCore*") if p.is_file()}}
        save()
        for phase, base_rounds in PHASES.items():
            rounds = max(1, int(base_rounds * args.round_scale))
            for occupancy in range(1, 9):
                expected = None
                records = []
                for repeat in range(-1, args.repeats):
                    start = repeat % len(LAYOUTS)
                    order = LAYOUTS[start:] + LAYOUTS[:start]
                    for layout in order:
                        command = [str(args.build_root / f"build-{layout}-off/bin/box-page-contract"),
                                   "--phase", phase, str(occupancy), str(rounds)]
                        run = subprocess.run(command, text=True, capture_output=True, timeout=300)
                        row = {"layout": layout, "phase": phase, "occupancy": occupancy,
                               "repeat": repeat, "warmup": repeat < 0, "command": command,
                               "exit_code": run.returncode, "stdout": run.stdout, "stderr": run.stderr}
                        state["records"].append(row)
                        if run.returncode:
                            raise RuntimeError("phase executable failed")
                        seconds, semantic = parse(run.stdout, layout, phase, occupancy, rounds)
                        row.update(seconds=seconds, semantic_digest=semantic)
                        if expected is None:
                            expected = semantic
                        if semantic != expected:
                            raise ValueError("cross-layout or repeat result mismatch")
                        if repeat >= 0:
                            records.append(row)
                medians = {layout: statistics.median(r["seconds"] for r in records
                            if r["layout"] == layout) for layout in LAYOUTS}
                summary = {"phase": phase, "occupancy": occupancy, "rounds": rounds,
                           "median_seconds": medians,
                           "ranges_seconds": {layout: [min(r["seconds"] for r in records if r["layout"] == layout),
                                                      max(r["seconds"] for r in records if r["layout"] == layout)]
                                              for layout in LAYOUTS},
                           "ratios_to_inline": {layout: medians[layout] / medians["inline"]
                                                for layout in LAYOUTS[1:]}}
                state["summary"].append(summary)
                save()
                print(json.dumps(summary), flush=True)
        state["passed"] = True
    finally:
        save()


if __name__ == "__main__":
    main()
