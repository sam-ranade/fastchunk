#!/usr/bin/env python3
"""Fail when benchmark throughput regresses beyond the configured threshold."""

import argparse
import json
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument("--max-regression", type=float, default=0.05)
    args = parser.parse_args()

    with open(args.baseline, encoding="utf-8") as stream:
        baseline = json.load(stream)
    with open(args.current, encoding="utf-8") as stream:
        current = json.load(stream)

    if baseline.get("schema_version") != 1 or current.get("schema_version") != 1:
        print("unsupported benchmark baseline schema", file=sys.stderr)
        return 1

    failures = []
    for name, expected in baseline.get("benchmarks", {}).items():
        actual = current.get("benchmarks", {}).get(name, {})
        expected_rate = expected.get("throughput_mbps")
        actual_rate = actual.get("throughput_mbps")
        if not expected_rate or not actual_rate:
            failures.append(f"{name}: missing throughput_mbps")
            continue
        regression = (expected_rate - actual_rate) / expected_rate
        print(f"{name}: baseline={expected_rate:.3f} current={actual_rate:.3f} regression={regression:.2%}")
        if regression > args.max_regression:
            failures.append(f"{name}: regression {regression:.2%} exceeds {args.max_regression:.2%}")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())