#!/usr/bin/env python3
"""Controlled page occupancy pilot; not a substitute for end-to-end AE."""
import argparse
import hashlib
import json
import platform
import re
import statistics
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=20000)
    args = parser.parse_args()
    if args.output.exists():
        raise RuntimeError("refuse to overwrite prior results")
    records, binaries, summary = [], {}, []
    for layout in ("inline", "packed"):
        for instrument in ("off", "on"):
            build = args.build_root / f"build-{layout}-{instrument}"
            exe = build / "bin/box-page-contract"
            identity = subprocess.check_output([str(exe), "--identity"], text=True).strip()
            if identity != f"representation=chunk8/{layout}8":
                raise RuntimeError(f"incorrect runtime representation: {identity}")
            binaries[f"{layout}-{instrument}"] = {
                "exe_sha256": digest(exe), "identity": identity,
                "cache": (build / "CMakeCache.txt").read_text(),
                "libraries": {p.name: digest(p) for p in (build / "lib").glob("*AbstractDomainCore*") if p.is_file()},
            }
    for occupancy in range(1, 9):
        expected = None
        for repetition in range(-1, 5):
            order = ("inline", "packed") if repetition % 2 == 0 else ("packed", "inline")
            for layout in order:
                exe = args.build_root / f"build-{layout}-off/bin/box-page-contract"
                command = [str(exe), "--bench", str(occupancy), str(args.rounds)]
                output = subprocess.check_output(command, text=True)
                values = dict(re.findall(r"(\w+)=([^\s]+)", output))
                if expected is None:
                    expected = values["semantic_digest"]
                if values["semantic_digest"] != expected:
                    raise RuntimeError("semantic digest mismatch")
                records.append({"layout": layout, "occupancy": occupancy,
                                "repetition": repetition, "warmup": repetition < 0,
                                "seconds": float(values["seconds"]), "output": output,
                                "command": command})
        memory = {}
        for layout in ("inline", "packed"):
            exe = args.build_root / f"build-{layout}-on/bin/box-page-contract"
            output = subprocess.check_output([str(exe), "--bench", str(occupancy), str(args.rounds)], text=True)
            values = dict(re.findall(r"(\w+)=([^\s]+)", output))
            if values["semantic_digest"] != expected:
                raise RuntimeError("diagnostic semantic digest mismatch")
            memory[layout] = {"bytes": int(values["retained_page_shallow_bytes"]), "output": output}
        medians = {layout: statistics.median(r["seconds"] for r in records
                                           if r["layout"] == layout and r["occupancy"] == occupancy and not r["warmup"])
                   for layout in ("inline", "packed")}
        row = {"occupancy": occupancy, "median_seconds": medians, "memory": memory,
               "time_ratio": medians["packed"] / medians["inline"],
               "page_bytes_ratio": memory["packed"]["bytes"] / memory["inline"]["bytes"]}
        summary.append(row)
        print(json.dumps(row), flush=True)
    result = {"scope": "synthetic controlled lifecycle; timing telemetry-off; retained bytes telemetry-on; no RSS claim",
              "host": platform.platform(), "rounds": args.rounds,
              "binaries": binaries, "records": records, "summary": summary}
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
