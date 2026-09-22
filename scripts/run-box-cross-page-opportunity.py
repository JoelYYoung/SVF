#!/usr/bin/env python3
"""Run a fail-closed cross-page opportunity replay on a sealed Box trace."""

import argparse
import hashlib
import json
import platform
import resource
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def save(path, state):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-result", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--cap-seconds", type=int, required=True)
    args = parser.parse_args()
    if args.cap_seconds <= 0:
        parser.error("cap must be positive")

    analyzer = Path(__file__).with_name("analyze-box-cowrite-trace.py")
    source = json.loads(args.source_result.read_text())
    required = {
        "passed": True,
        "semantic_projection_exact": True,
        "semantic_coverage_exact": True,
        "semantic_trace_exact": True,
        "trace_matches_semantic_coverage": True,
        "trace_matches_semantic_icfg": True,
    }
    for name, expected in required.items():
        if source.get(name) != expected:
            raise ValueError(f"source result failed gate: {name}")
    if not all(source.get("replay", {}).get("validation", {}).values()):
        raise ValueError("source G0 replay was not exact")

    trace = Path(source["trace_path"])
    if digest(trace) != source["trace_sha256"]:
        raise ValueError("sealed trace hash changed")
    if trace.stat().st_size != source["trace_bytes"]:
        raise ValueError("sealed trace size changed")

    parent = (args.output_root / source["program"] / source["sparsity"] /
              "attempts")
    parent.mkdir(parents=True, exist_ok=True)
    attempt = Path(tempfile.mkdtemp(
        prefix=f"{platform.node()}-", dir=parent))
    result_path = attempt / "result.json"
    analysis_path = attempt / "analysis.json"
    stdout_path = attempt / "analysis.stdout"
    stderr_path = attempt / "analysis.stderr"
    state = {
        "schema": "box-cross-page-opportunity-run-v1",
        "passed": False,
        "program": source["program"],
        "sparsity": source["sparsity"],
        "host": platform.node(),
        "attempt": str(attempt),
        "source_result": str(args.source_result),
        "source_result_sha256": digest(args.source_result),
        "trace": str(trace),
        "trace_sha256": source["trace_sha256"],
        "trace_bytes": source["trace_bytes"],
        "analyzer": str(analyzer),
        "analyzer_sha256": digest(analyzer),
        "cap_seconds": args.cap_seconds,
    }
    save(result_path, state)
    started = time.monotonic()
    command = [sys.executable, str(analyzer), str(trace),
               "--opportunity-only", "--json", str(analysis_path)]
    try:
        with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
            completed = subprocess.run(
                command, stdout=stdout, stderr=stderr,
                timeout=args.cap_seconds, check=False)
        state["exit_code"] = completed.returncode
        if completed.returncode != 0:
            raise RuntimeError(f"analyzer exit {completed.returncode}")
        analysis = json.loads(analysis_path.read_text())
        if analysis.get("schema") != "box-cross-page-replay-v1":
            raise ValueError("wrong opportunity schema")
        if not all(analysis.get("validation", {}).values()):
            raise ValueError("new G0 replay did not match sealed trace")
        if analysis["raw"] != source["replay"]["raw"]:
            raise ValueError("new raw replay totals differ")
        expected = {"g0_current", "g1_registration_dense",
                    "g4_retained_support"}
        if set(analysis["replay"]) != expected:
            raise ValueError("opportunity layout set changed")
        state["analysis"] = analysis
        state["analysis_sha256"] = digest(analysis_path)
        state["passed"] = True
    except subprocess.TimeoutExpired:
        state["error"] = "timeout"
        state["exit_code"] = 124
        raise
    except Exception as error:
        state["error"] = str(error)
        raise
    finally:
        state["wall_seconds"] = time.monotonic() - started
        state["child_max_rss_kib"] = resource.getrusage(
            resource.RUSAGE_CHILDREN).ru_maxrss
        if stdout_path.exists():
            state["stdout_sha256"] = digest(stdout_path)
        if stderr_path.exists():
            state["stderr_sha256"] = digest(stderr_path)
        save(result_path, state)
        (attempt / "terminal").touch()
        if state["passed"]:
            (attempt / "passed").touch()
    print(json.dumps(state, sort_keys=True))


if __name__ == "__main__":
    main()
