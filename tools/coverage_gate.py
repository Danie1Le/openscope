#!/usr/bin/env python3
"""Coverage gate (NFR-11): line coverage of the trigger, measurement and SCPI parser sources.

Run after `ctest --preset coverage`. Merges the raw profiles that run wrote, writes the llvm-cov
report to build/coverage/coverage-report.txt (a CI artifact, with the per-area summary appended)
and exits 1 when any AREA has fewer than THRESHOLD percent of its lines covered. Each area is
gated on its own, so a well-tested area cannot hide an untested one; the threshold and the areas
(with their files) are set here and nowhere else.
"""

import json
import subprocess
import sys
from pathlib import Path

THRESHOLD = 85.0

# Gated areas: name -> source files relative to the repository root. Gating per area (not on the
# sum of all files) keeps a 23-line file such as trigger.cpp from being outweighed by the rest,
# and not per file, where a tiny file would gate on noise.
AREAS = {
    # Edge detection, the capture assembly that holds the pre-trigger samples, the holdoff, the
    # Auto timeout and the placement of time zero, and the block assembly that discards a capture
    # at a settings change or a dropped block (spec #1: Acquisition).
    "trigger": [
        "src/openscope/trigger.cpp",
        "src/openscope/capture_assembler.cpp",
        "src/openscope/block_assembler.cpp",
    ],
    "measurement": [
        "src/openscope/measurement.cpp",
        "src/openscope/measurement_statistics.cpp",
        "src/openscope/running_statistics.hpp",
    ],
    "SCPI parser": [
        "src/openscope/scpi/parser.cpp",
        "src/openscope/scpi/command_tree.cpp",
        "src/openscope/scpi/response.cpp",
    ],
}
GATED_FILES = [path for files in AREAS.values() for path in files]

# Everything that links the gated code and runs under ctest.
OBJECTS = ["build/coverage/tests/openscope_tests", "build/coverage/src/openscope-engine"]

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "coverage"


def percent(covered, total):
    """Percentage of lines covered; an area with no lines counts as 0, not as a pass."""
    return 100.0 * covered / total if total else 0.0


def area_totals(lines, areas=None):
    """Return {area: (covered, total)} from per-file {path: {"covered", "count"}} line counts."""
    return {
        area: (
            sum(lines[path]["covered"] for path in paths),
            sum(lines[path]["count"] for path in paths),
        )
        for area, paths in (AREAS if areas is None else areas).items()
    }


def failing_areas(totals, threshold=THRESHOLD):
    """Names of the areas whose line coverage is below the threshold."""
    return [area for area, (covered, total) in totals.items() if percent(covered, total) < threshold]


def summary(lines, totals):
    """The per-area figures, the per-file breakdown and the summed figure, as text."""
    out = [f"Line coverage by area (each must be at least {THRESHOLD:g}%):"]
    for area, (covered, total) in totals.items():
        status = "ok" if percent(covered, total) >= THRESHOLD else "BELOW"
        out.append(f"  {area}: {percent(covered, total):.2f}% ({covered} of {total} lines) {status}")
        for path in AREAS[area]:
            entry = lines[path]
            out.append(
                f"    {path}: {percent(entry['covered'], entry['count']):.2f}%"
                f" ({entry['covered']} of {entry['count']})"
            )
    covered = sum(c for c, _ in totals.values())
    total = sum(t for _, t in totals.values())
    out.append(f"Sum of all gated files (not gated): {percent(covered, total):.2f}%")
    return "\n".join(out)


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
    exported = run("llvm-cov-18", "export", "-summary-only", *common, capture_output=True)
    files = json.loads(exported)["data"][0]["files"]

    lines = {
        Path(entry["filename"]).relative_to(ROOT).as_posix(): entry["summary"]["lines"]
        for entry in files
    }
    missing = sorted(set(GATED_FILES) - set(lines))
    if missing:
        sys.exit(f"error: no coverage data for {', '.join(missing)}")

    totals = area_totals(lines)
    text = summary(lines, totals)
    (BUILD / "coverage-report.txt").write_text(f"{report}\n{text}\n")
    print(report)
    print(text)
    failed = failing_areas(totals)
    if failed:
        sys.exit(f"error: line coverage is below {THRESHOLD:g}% for: {', '.join(failed)}")


if __name__ == "__main__":
    main()
