#!/usr/bin/env python3
"""Build one immutable Linux borrowed-read candidate with retry-safe publication."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import runpy
import subprocess
import time

HERE = Path(__file__).resolve().parent
PROCESS = runpy.run_path(str(HERE / "experiment_process.py"))


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run(argv, log, timeout):
    with log.open("w") as stream:
        result = PROCESS["run_managed"](
            argv,
            stdout=stream,
            stderr=subprocess.STDOUT,
            timeout_seconds=timeout,
        )
    if result.returncode:
        raise RuntimeError(
            f"command failed with exit {result.returncode}: {argv}; log={log}"
        )


def publish_link(target, link):
    if os.path.lexists(link):
        raise RuntimeError(f"refusing to replace existing publication: {link}")
    temporary = link.with_name(f".{link.name}.{os.getpid()}.tmp")
    os.symlink(target, temporary)
    os.replace(temporary, link)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument(
        "--repository", default="https://github.com/JoelYYoung/SVF.git"
    )
    args = parser.parse_args()
    study = args.study.resolve()
    baseline_source = study / "directory-control-source-v1"
    stable_build = study / "build-read-views-v1"
    lock_path = study / "locks/read-views-build-v1.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)

    with lock_path.open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if os.path.lexists(stable_build):
            metadata_path = stable_build / "box-read-views-build.json"
            metadata = json.loads(metadata_path.read_text())
            if (not stable_build.is_symlink() or not metadata.get("passed") or
                    metadata.get("source_commit") != args.commit):
                raise RuntimeError("existing read-views publication is incompatible")
            print(json.dumps({
                "passed": True,
                "reused": True,
                "metadata": str(metadata_path),
            }))
            return

        attempt = PROCESS["create_attempt"](
            study / "attempts/read-views-build-v1", platform.node()
        )
        source = attempt / "source"
        build = attempt / "build"
        state = {
            "schema": 1,
            "passed": False,
            "host": platform.node(),
            "source_commit": args.commit,
            "attempt": attempt.name,
            "started_epoch": time.time(),
            "runner_sha256": digest(Path(__file__)),
            "process_helper_sha256": digest(HERE / "experiment_process.py"),
            "commands": [],
        }

        def save():
            (attempt / "build-state.json").write_text(
                json.dumps(state, indent=2) + "\n"
            )

        def execute(name, command, timeout):
            state["commands"].append({"name": name, "argv": command})
            save()
            run(command, attempt / f"{name}.log", timeout)

        try:
            execute(
                "clone",
                ["git", "clone", "--shared", "--no-checkout",
                 str(baseline_source), str(source)],
                600,
            )
            execute(
                "fetch",
                ["git", "-C", str(source), "fetch", "--no-tags",
                 args.repository,
                 args.commit],
                600,
            )
            execute(
                "checkout",
                ["git", "-C", str(source), "checkout", "--detach", args.commit],
                300,
            )
            head = subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
            ).strip()
            dirty = subprocess.check_output(
                ["git", "-C", str(source), "status", "--porcelain"], text=True
            )
            if head != args.commit or dirty:
                raise RuntimeError("candidate source checkout is not exact and clean")
            configure = [
                "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON",
                "-DSVF_ENABLE_ASSERTIONS=OFF", "-DSVF_WARN_AS_ERROR=ON",
                "-DCMAKE_C_COMPILER=/usr/bin/gcc",
                "-DCMAKE_CXX_COMPILER=/usr/bin/g++",
                "-DMPFR_ROOT=/mnt/scratch/PAG/yjw/toolchains/texlive",
                "-DLLVM_DIR=/mnt/scratch/PAG/yjw/toolchains/llvm-21.1.0.obj/lib/cmake/llvm",
                "-DSVF_BOX_GROUP_CONTENTS=OFF", "-DSVF_BOX_PACKED_PAGES=OFF",
                "-DSVF_BOX_ADAPTIVE_PAGES=OFF",
                "-DSVF_BOX_STORAGE_TELEMETRY=OFF",
                "-DSVF_BOX_PAGE_INTERNING=OFF",
                "-DSVF_BOX_WHOLE_DIRECTORY=OFF",
                "-DSVF_BUILD_BOX_PAGE_TESTS=ON",
            ]
            execute("configure", configure, 900)
            execute(
                "build",
                ["cmake", "--build", str(build), "--parallel", "16", "--target",
                 "ae", "box-page-semantic-observer", "box-page-contract",
                 "box-read-view-contract"],
                3600,
            )
            execute(
                "ctest",
                ["ctest", "--test-dir", str(build), "-R",
                 "^box-(page-contract|read-view-contract|adapter-layout-contract)$",
                 "--output-on-failure"],
                900,
            )
            for test in (
                "test-experiment-process.py",
                "test-box-read-views-runner.py",
                "test-box-storage-controls.py",
            ):
                execute(
                    f"python-{Path(test).stem}",
                    ["python3", str(source / "scripts" / test), "-v"],
                    300,
                )
            identity = subprocess.check_output(
                [str(build / "bin/box-page-contract"), "--interning-identity"],
                text=True,
            ).splitlines()
            if identity != ["representation=chunk8/inline8", "pool_policy=off"]:
                raise RuntimeError(f"wrong runtime identity: {identity}")
            artifacts = [
                build / "bin" / name
                for name in ("ae", "box-page-semantic-observer",
                             "box-page-contract", "box-read-view-contract")
            ] + [build / "lib/libAbstractDomainCore.so.3.4"]
            state.update({
                "passed": True,
                "finished_epoch": time.time(),
                "source": str(source),
                "build": str(build),
                "identity": identity,
                "artifacts": {str(path): digest(path) for path in artifacts},
                "cmake_cache_sha256": digest(build / "CMakeCache.txt"),
            })
            save()
            (build / "box-read-views-build.json").write_text(
                json.dumps(state, indent=2) + "\n"
            )
            publish_link(build, stable_build)
        except BaseException as error:
            state["error"] = f"{type(error).__name__}: {error}"
            state["finished_epoch"] = time.time()
            save()
            raise
        print(json.dumps({
            "passed": True,
            "reused": False,
            "metadata": str(stable_build / "box-read-views-build.json"),
        }))


if __name__ == "__main__":
    main()
