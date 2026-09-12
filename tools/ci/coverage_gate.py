#!/usr/bin/env python3
"""Coverage gate (REQ §12): fail the build under the threshold.

§12 makes coverage a hard requirement and says so in design terms: "domain
logic must be a plain class with no socket, no filesystem, and no hardware
dependency... A design that cannot reach high coverage is a design defect."
The threshold is passed in by the pipeline (COVERAGE_MIN, default 80) so
raising it is a one-line change rather than an edit here.
"""
import sys
import xml.etree.ElementTree as ET

def main() -> int:
    if len(sys.argv) != 3:
        print("usage: coverage_gate.py <coverage.xml> <min-lines-pct>", file=sys.stderr)
        return 2
    tree = ET.parse(sys.argv[1])
    root = tree.getroot()
    lr = root.attrib.get("line-rate") or root.find(".//coverage").attrib.get("line-rate")
    pct = float(lr) * 100.0
    threshold = float(sys.argv[2])
    print(f"coverage: {pct:.1f}% (gate: {threshold:.0f}%)")
    if pct < threshold:
        print(f"FAIL: line coverage {pct:.1f}% below gate {threshold:.0f}%", file=sys.stderr)
        return 1
    print("PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())
