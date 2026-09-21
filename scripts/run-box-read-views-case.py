#!/usr/bin/env python3
"""Semantic gate or balanced screen for the borrowed-read Box candidate."""

import argparse
import json
from pathlib import Path
import platform
import re
import runpy
import subprocess

HERE = Path(__file__).resolve().parent
PROCESS = runpy.run_path(str(HERE / "experiment_process.py"))
SEMANTIC = runpy.run_path(str(HERE / "run-t5-semantic-case.py"))
PERFORMANCE = runpy.run_path(str(HERE / "run-t5-storage-performance-case.py"))
digest = SEMANTIC["digest"]
VARIANTS = ("baseline", "read-views")


def fingerprint(build, require_read_contract=False):
    identity = subprocess.check_output(
        [str(build / "bin/box-page-contract"), "--interning-identity"],
        text=True,
    ).splitlines()
    if identity != ["representation=chunk8/inline8", "pool_policy=off"]:
        raise RuntimeError(f"unexpected Box representation: {identity}")
    cache = (build / "CMakeCache.txt").read_text().splitlines()
    required = {
        "SVF_BOX_WHOLE_DIRECTORY": "OFF",
        "SVF_BOX_GROUP_CONTENTS": "OFF",
        "SVF_BOX_PACKED_PAGES": "OFF",
        "SVF_BOX_ADAPTIVE_PAGES": "OFF",
        "SVF_BOX_STORAGE_TELEMETRY": "OFF",
        "SVF_ENABLE_ASSERTIONS": "OFF",
        "SVF_WARN_AS_ERROR": "ON",
    }
    if any(f"{key}:BOOL={value}" not in cache for key, value in required.items()):
        raise RuntimeError(f"unexpected build configuration: {build}")
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        raise RuntimeError(f"not a Release build: {build}")
    if "SVF_BOX_PAGE_INTERNING:STRING=OFF" not in cache:
        raise RuntimeError(f"page interning is not disabled: {build}")
    executable_names = ["ae", "box-page-contract", "box-page-semantic-observer"]
    if require_read_contract:
        executable_names.append("box-read-view-contract")
    executables = [build / "bin" / name for name in executable_names]
    libraries = [
        build / "lib" / f"{name}.so.3.4"
        for name in ("libAbstractDomainCore", "libSvfCore", "libSvfLLVM")
    ]
    result = {
        "identity": identity,
        "files": {str(path): digest(path) for path in executables + libraries},
        "cache_sha256": digest(build / "CMakeCache.txt"),
        "ldd": {},
    }
    for executable in executables:
        linkage = subprocess.check_output(["ldd", str(executable)], text=True)
        result["ldd"][executable.name] = re.sub(
            r"\(0x[0-9a-fA-F]+\)", "(address)", linkage
        )
        expected = libraries[:1] if executable.name in (
            "box-page-contract", "box-read-view-contract"
        ) else libraries
        for library in expected:
            match = re.search(
                re.escape(library.name.split(".so")[0]) + r"\S* => (\S+)",
                linkage,
            )
            if not match or Path(match[1]).resolve() != library.resolve():
                raise RuntimeError(f"unexpected runtime library: {library}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--mode", choices=("semi-sparse", "sparse"), required=True)
    parser.add_argument("--bitcode", type=Path, required=True)
    parser.add_argument("--input-sha256", required=True)
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--phase", choices=("semantic", "performance"), required=True)
    parser.add_argument("--first", choices=VARIANTS, required=True)
    parser.add_argument("--semantic-result", type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()

    if digest(args.bitcode) != args.input_sha256:
        raise RuntimeError(f"input changed: {args.bitcode}")
    builds = {
        "baseline": args.study / "build-directory-control-chunk-v1",
        "read-views": args.study / "build-read-views-v1",
    }
    extapi = args.study / "build-original/lib/extapi.bc"
    fingerprints = {
        name: fingerprint(build, name == "read-views")
        for name, build in builds.items()
    }
    core_hashes = {
        value
        for item in fingerprints.values()
        for path, value in item["files"].items()
        if "libAbstractDomainCore" in path
    }
    if len(core_hashes) != 2:
        raise RuntimeError("baseline and candidate resolve to the same core library")

    attempt = PROCESS["create_attempt"](
        args.study / "results/read-views-v1" / args.phase / args.mode /
        args.program / "attempts",
        platform.node(),
    )
    state = {
        "schema": 1,
        "passed": False,
        "attempt": attempt.name,
        "host": platform.node(),
        "phase": args.phase,
        "program": args.program,
        "mode": args.mode,
        "input": str(args.bitcode),
        "input_sha256": args.input_sha256,
        "extapi": str(extapi),
        "extapi_sha256": digest(extapi),
        "cap_seconds": args.cap_seconds,
        "first": args.first,
        "repeats": args.repeats,
        "fingerprints": fingerprints,
        "runner_sha256": digest(Path(__file__)),
        "helper_sha256": {
            name: digest(HERE / name)
            for name in (
                "experiment_process.py",
                "run-t5-semantic-case.py",
                "run-t5-storage-performance-case.py",
            )
        },
        "timing_class": (
            "semantic-correctness-gate" if args.phase == "semantic"
            else "concurrent-screening-not-final-timing"
        ),
        "runs": [],
    }

    def save():
        (attempt / "result.json").write_text(json.dumps(state, indent=2) + "\n")

    order = (args.first, next(name for name in VARIANTS if name != args.first))
    try:
        if args.phase == "semantic":
            projections = []
            for repeat in range(2):
                for name in order if repeat == 0 else order[::-1]:
                    directory = attempt / f"{name}-{repeat}"
                    projection, record = SEMANTIC["run_variant"](
                        builds[name] / "bin/box-page-semantic-observer",
                        extapi,
                        args.bitcode,
                        args.cap_seconds,
                        directory,
                        args.mode,
                    )
                    record.update(variant=name, repeat=repeat)
                    state["runs"].append(record)
                    save()
                    if projection is None:
                        raise RuntimeError(f"semantic observer failed: {name}")
                    identities = (directory / "analysis.log").read_text(
                        errors="replace"
                    ).splitlines()
                    for identity in (
                        "BOX_REPRESENTATION chunk8/inline8",
                        "BOX_CONTENT_LAYOUT registration",
                        "BOX_PAGE_INTERNING off",
                    ):
                        if identity not in identities:
                            raise RuntimeError(f"missing runtime identity: {identity}")
                    projections.append(projection)
            state["differences"] = [
                SEMANTIC["differences"](projections[0], value)
                for value in projections[1:]
            ]
            if any(state["differences"]):
                raise RuntimeError("canonical projection differs")
            for key in ("function_coverage_percent", "icfg_node_trace"):
                values = [record[key] for record in state["runs"]]
                if not values[0] or any(value != values[0] for value in values[1:]):
                    raise RuntimeError(f"semantic workload differs: {key}")
            state["projection_entries"] = len(projections[0])
        else:
            if args.semantic_result is None:
                raise RuntimeError("performance requires --semantic-result")
            gate = json.loads(args.semantic_result.read_text())
            expected = {
                key: state[key]
                for key in ("program", "mode", "input", "input_sha256",
                            "extapi", "extapi_sha256", "fingerprints")
            }
            if (not gate.get("passed") or gate.get("phase") != "semantic" or
                    gate.get("host") != state["host"] or
                    any(gate.get(key) != value for key, value in expected.items())):
                raise RuntimeError("semantic prerequisite or artifacts changed")
            state["semantic_result"] = str(args.semantic_result)
            state["semantic_result_sha256"] = digest(args.semantic_result)
            for name in order:
                record = PERFORMANCE["run_variant"](
                    builds[name] / "bin/ae", extapi, args.bitcode,
                    args.cap_seconds, attempt / f"warmup-{name}", args.mode,
                )
                record.update(variant=name, phase="warmup", repeat=None)
                state["runs"].append(record)
                save()
            for repeat in range(args.repeats):
                for name in order if repeat % 2 == 0 else order[::-1]:
                    record = PERFORMANCE["run_variant"](
                        builds[name] / "bin/ae", extapi, args.bitcode,
                        args.cap_seconds, attempt / f"measured-{repeat}-{name}",
                        args.mode,
                    )
                    record.update(variant=name, phase="measured", repeat=repeat)
                    state["runs"].append(record)
                    save()
                    for key in ("function_coverage_percent", "icfg_node_trace"):
                        if record[key] != gate["runs"][0][key][0]:
                            raise RuntimeError(f"performance workload differs: {key}")
        current = {
            name: fingerprint(build, name == "read-views")
            for name, build in builds.items()
        }
        if fingerprints != current:
            raise RuntimeError("build changed during experiment")
        state["passed"] = True
    except BaseException as error:
        state["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        save()
    (attempt / "finished").touch()
    print(json.dumps({"passed": True, "result": str(attempt / "result.json")}))


if __name__ == "__main__":
    main()
