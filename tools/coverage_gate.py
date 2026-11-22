#!/usr/bin/env python3
"""Coverage gate (NFR-11): line coverage of the trigger, measurement and SCPI parser sources.

Run after `ctest --preset coverage`. Merges the raw profiles that run wrote, writes the llvm-cov
report to build/coverage/coverage-report.txt (a CI artifact) and exits 1 when the gated files
together have fewer than THRESHOLD percent of their lines covered. The threshold and the files are
set here and nowhere else.
"""

import json
import subprocess
import sys
from pathlib import Path

THRESHOLD = 85.0

# Trigger, measurement and SCPI parser sources, relative to the repository root.
GATED_FILES = [
    "src/openscope/trigger.cpp",
    "src/openscope/measurement.cpp",
    "src/openscope/measurement_statistics.cpp",
    "src/openscope/running_statistics.hpp",
    "src/openscope/scpi/parser.cpp",
    "src/openscope/scpi/command_tree.cpp",
    "src/openscope/scpi/response.cpp",
]

# Everything that links the gated code and runs under ctest.
OBJECTS = ["build/coverage/tests/openscope_tests", "build/coverage/src/openscope-engine"]

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "coverage"


def run(*command, **kwargs):
    return subprocess.run(command, cwd=ROOT, check=True, text=True, **kwargs).stdout


def main():
    profiles = sorted((BUILD / "profraw").glob("*.profraw"))
    if not profiles:
        sys.exit("error: no profiles in build/coverage/profraw; run ctest --preset coverage first")
    profdata = BUILD / "coverage.profdata"
    run("llvm-profdata-18", "merge", "-sparse", *profiles, "-o", profdata)

    # The first object is positional; every other one needs -object.
    objects = [OBJECTS[0]] + [arg for extra in OBJECTS[1:] for arg in ("-object", extra)]
    common = [f"-instr-profile={profdata}", *objects, *GATED_FILES]
    report = run("llvm-cov-18", "report", "-use-color=false", *common, capture_output=True)
    (BUILD / "coverage-report.txt").write_text(report)
    exported = run("llvm-cov-18", "export", "-summary-only", *common, capture_output=True)
    summary = json.loads(exported)

    lines = {
        Path(entry["filename"]).relative_to(ROOT).as_posix(): entry["summary"]["lines"]
        for entry in summary["data"][0]["files"]
    }
    missing = sorted(set(GATED_FILES) - set(lines))
    if missing:
        sys.exit(f"error: no coverage data for {', '.join(missing)}")

    print(report)
    covered = sum(entry["covered"] for entry in lines.values())
    total = sum(entry["count"] for entry in lines.values())
    percent = 100.0 * covered / total
    print(f"Line coverage of the gated files: {percent:.2f}% ({covered} of {total} lines)")
    if percent < THRESHOLD:
        sys.exit(f"error: line coverage {percent:.2f}% is below the {THRESHOLD:g}% threshold")


main()
