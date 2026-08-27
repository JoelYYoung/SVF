#!/usr/bin/env python3

"""Interleaved end-to-end dense-AE timing for storage implementations.

Fixtures can be reproduced with GenerateAEDenseBenchmark.py and an LLVM clang:

  GenerateAEDenseBenchmark.py --blocks 64 > fixture.c
  clang -O0 -emit-llvm -c fixture.c -o fixture.bc
"""

import argparse
import csv
import os
import time


def implementation(value):
    fields = value.split("=", 1)
    if len(fields) != 2:
        raise argparse.ArgumentTypeError(
            "implementation must be NAME=AE,EXTAPI[,native|legacy]")
    parts = fields[1].split(",")
    if len(parts) not in (2, 3):
        raise argparse.ArgumentTypeError(
            "implementation must be NAME=AE,EXTAPI[,native|legacy]")
    mode = parts[2] if len(parts) == 3 else "native"
    if mode not in ("native", "legacy"):
        raise argparse.ArgumentTypeError("mode must be native or legacy")
    return fields[0], parts[0], parts[1], mode


def measure(executable, extapi, mode, fixture, library_path):
    arguments = [
        executable,
        "-ae-sparsity=dense",
        "-ae-dense-octagon=false",
        "-stat=false",
        "-extapi=" + extapi,
        fixture,
    ]
    if mode == "legacy":
        arguments.insert(2, "-ae-dense-legacy-interval=true")
    start = time.perf_counter_ns()
    pid = os.fork()
    if pid == 0:
        with open(os.devnull, "wb", buffering=0) as sink:
            os.dup2(sink.fileno(), 1)
            os.dup2(sink.fileno(), 2)
            environment = dict(os.environ)
            if library_path:
                environment["DYLD_LIBRARY_PATH"] = library_path
            os.execvpe(executable, arguments, environment)
    _, status, usage = os.wait4(pid, 0)
    finish = time.perf_counter_ns()
    if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        raise RuntimeError("AE failed: " + " ".join(arguments))
    return finish - start, usage.ru_utime, usage.ru_stime, usage.ru_maxrss


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--implementation", action="append", type=implementation,
                        required=True)
    parser.add_argument("--fixture", action="append", required=True)
    parser.add_argument("--repetitions", type=int, default=9)
    parser.add_argument("--library-path")
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.repetitions < 3 or options.repetitions % 2 == 0:
        parser.error("repetitions must be an odd number of at least three")

    with open(options.output, "w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow([
            "implementation", "fixture", "repetition", "wall_ns",
            "user_seconds", "system_seconds", "peak_rss_raw",
        ])
        for fixture in options.fixture:
            for _, executable, extapi, mode in options.implementation:
                measure(executable, extapi, mode, fixture,
                        options.library_path)
            for repetition in range(options.repetitions):
                offset = repetition % len(options.implementation)
                ordered = (options.implementation[offset:] +
                           options.implementation[:offset])
                for name, executable, extapi, mode in ordered:
                    wall, user, system, rss = measure(
                        executable, extapi, mode, fixture,
                        options.library_path)
                    writer.writerow([
                        name, os.path.basename(fixture), repetition + 1,
                        wall, f"{user:.9f}", f"{system:.9f}", rss,
                    ])
                    output.flush()


if __name__ == "__main__":
    main()
