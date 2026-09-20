#!/usr/bin/env python3
"""Run one mode/program pair with runtime-identity and semantic prerequisites.

Semantic screening can be concurrent. Performance is a separate phase and
requires the unchanged binary/input manifest from a successful semantic gate.
This script does not submit jobs or provide host isolation.
"""
import argparse
import json
import platform
import re
import runpy
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
SEM = runpy.run_path(str(HERE / "run-t5-semantic-case.py"))
PERF = runpy.run_path(str(HERE / "run-t5-storage-performance-case.py"))
digest = SEM["digest"]


def fingerprint(build, layout):
    identity = subprocess.check_output(
        [str(build / "bin/box-page-contract"), "--identity"], text=True
    ).strip()
    if identity != f"representation=chunk8/{layout}8":
        raise RuntimeError(f"wrong runtime layout: {identity}")
    files = [build / "bin" / name for name in
             ("ae", "box-page-contract", "box-page-semantic-observer")]
    files += [build / "lib" / name for name in
              ("libAbstractDomainCore.so.3.4", "libSvfCore.so.3.4", "libSvfLLVM.so.3.4")]
    linkage = {}
    for executable in files[:3]:
        ldd = subprocess.check_output(["ldd", str(executable)], text=True)
        # Loader addresses vary with ASLR; file resolution is the fingerprint.
        linkage[executable.name] = re.sub(r"\(0x[0-9a-fA-F]+\)", "(address)", ldd)
        loaded = re.search(r"libAbstractDomainCore\S* => (\S+)", ldd)
        if not loaded or Path(loaded[1]).resolve() != files[3].resolve():
            raise RuntimeError(f"unexpected core library resolution: {executable}")
    return {"identity": identity, "files": {str(p): digest(p) for p in files},
            "ldd": linkage, "cmake_cache_sha256": digest(build / "CMakeCache.txt")}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--bitcode", type=Path, required=True)
    parser.add_argument("--input-sha256", required=True)
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--mode", choices=("semi-sparse", "sparse"), required=True)
    parser.add_argument("--phase", choices=("semantic", "performance"), required=True)
    parser.add_argument("--result-set", required=True)
    parser.add_argument("--semantic-result", type=Path)
    parser.add_argument("--candidate", choices=("packed", "adaptive"), default="packed")
    parser.add_argument("--first", choices=("inline", "packed", "adaptive"), default="inline")
    args = parser.parse_args()
    layouts = ("inline", args.candidate)
    if args.first not in layouts:
        parser.error("--first must be inline or the selected candidate")
    builds = {layout: args.study / f"build-page-{layout}-v1" for layout in layouts}
    output = args.study / "results" / args.result_set / args.mode / args.program
    output.mkdir(parents=True, exist_ok=False)
    extapi = args.study / "build-original/lib/extapi.bc"
    state = {"passed": False, "phase": args.phase, "mode": args.mode,
             "program": args.program, "host": platform.node(), "runs": [],
             "runner_sha256": digest(Path(__file__)),
             "helper_sha256": {p: digest(HERE / p) for p in
                               ("run-t5-semantic-case.py", "run-t5-storage-performance-case.py")},
             "cap_seconds": args.cap_seconds,
             "timing_class": "concurrent-screening" if args.phase == "semantic" else "requires-external-isolation"}

    def save():
        (output / "result.json").write_text(json.dumps(state, indent=2) + "\n")

    try:
        if digest(args.bitcode) != args.input_sha256:
            raise RuntimeError("bitcode checksum mismatch")
        manifest = {"input": str(args.bitcode), "input_sha256": args.input_sha256,
                    "extapi": str(extapi), "extapi_sha256": digest(extapi),
                    "variants": {name: fingerprint(build, name) for name, build in builds.items()}}
        core_hashes = {value for variant in manifest["variants"].values()
                       for path, value in variant["files"].items() if "libAbstractDomainCore" in path}
        if len(core_hashes) != 2:
            raise RuntimeError("the two representations resolved to identical core binaries")
        state["manifest"] = manifest
        save()
        order = (args.first, args.candidate if args.first == "inline" else "inline")
        if args.phase == "semantic":
            projections = []
            for repeat in range(2):
                for layout in order if repeat % 2 == 0 else order[::-1]:
                    directory = output / f"{layout}-{repeat}"
                    projection, record = SEM["run_variant"](
                        builds[layout] / "bin/box-page-semantic-observer", extapi,
                        args.bitcode, args.cap_seconds, directory, args.mode)
                    record.update(layout=layout, repeat=repeat)
                    state["runs"].append(record)
                    save()
                    if projection is None:
                        raise RuntimeError(f"observer failed: {layout}")
                    if f"BOX_REPRESENTATION chunk8/{layout}8" not in (directory / "analysis.log").read_text():
                        raise RuntimeError("observer runtime identity missing")
                    projections.append(projection)
            changes = [SEM["differences"](projections[0], value) for value in projections[1:]]
            state["differences"] = changes
            state["projection_entries"] = len(projections[0])
            if any(changes):
                raise RuntimeError("canonical projection differs across layout or repetition")
            for key in ("function_coverage_percent", "icfg_node_trace"):
                values = [record[key] for record in state["runs"]]
                if not values[0] or any(value != values[0] for value in values[1:]):
                    raise RuntimeError(f"observer workload mismatch: {key}")
        else:
            if args.semantic_result is None:
                raise RuntimeError("performance requires a semantic gate")
            gate = json.loads(args.semantic_result.read_text())
            if not gate["passed"] or gate["phase"] != "semantic" or gate["mode"] != args.mode or gate["program"] != args.program or gate["manifest"] != manifest:
                raise RuntimeError("semantic prerequisite or binary/input fingerprint changed")
            if gate["host"] != platform.node():
                raise RuntimeError("paired semantic/performance host changed")
            state["semantic_result_sha256"] = digest(args.semantic_result)
            for repeat in range(-1, 3):
                for layout in order if repeat % 2 == 0 else order[::-1]:
                    record = PERF["run_variant"](
                        builds[layout] / "bin/ae", extapi, args.bitcode, args.cap_seconds,
                        output / f"{layout}-{repeat}", args.mode)
                    record.update(layout=layout, repeat=repeat, warmup=repeat < 0)
                    state["runs"].append(record)
                    save()
                    for key in ("function_coverage_percent", "icfg_node_trace"):
                        if [record[key]] != gate["runs"][0][key]:
                            raise RuntimeError(f"performance workload differs from gate: {key}")
        if any(fingerprint(build, name) != manifest["variants"][name] for name, build in builds.items()):
            raise RuntimeError("runtime build changed during execution")
        state["passed"] = True
    except Exception as error:
        state["error"] = str(error)
        raise
    finally:
        save()
    print(json.dumps({"program": args.program, "mode": args.mode, "phase": args.phase,
                      "passed": state["passed"], "result": str(output / "result.json")}))


if __name__ == "__main__":
    main()
