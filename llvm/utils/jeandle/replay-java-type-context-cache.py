#!/usr/bin/env python3
"""Three paired native replays of expensive JavaType context reuse."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess

def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--batches", type=int, default=4096)
    p.add_argument("--cpu", type=int, default=16)
    a = p.parse_args()
    if a.rounds < 1 or a.batches < 1:
        p.error("rounds and batches must be positive")
    a.out.mkdir(parents=True, exist_ok=False)
    binary = a.binary.resolve()
    identity = {"binary": str(binary), "sha256": sha(binary),
                "rounds": a.rounds, "batches": a.batches, "cpu": a.cpu,
                "seed": 20261023,
                "note": "native timer excludes fixture construction; cold clears JavaType only; mutation/full clears on edits; timed oracle counting and fresh verification are disabled"}
    (a.out / "identity.json").write_text(json.dumps(identity, indent=2))
    variants = {"final": ["cold", "cached"], "prefix": ["cold", "cached"],
                "mutation": ["full", "cached"]}
    def run(case, mode, batches, kind):
        cmd = ["taskset", "-c", str(a.cpu), str(binary), case, mode,
               str(batches), kind]
        r = subprocess.run(cmd, check=True, capture_output=True, text=True)
        row = json.loads(r.stdout)
        row["argv"] = cmd
        return row
    diag = {}
    for case, modes in variants.items():
        diag[case] = {mode: run(case, mode, 1, "diagnostic") for mode in modes}
        baseline, cached = (diag[case][mode] for mode in modes)
        assert baseline["checksum"] == cached["checksum"]
        assert all(x["fresh_reference_checked"] for x in diag[case].values())
        assert cached["oracle_calls"] < baseline["oracle_calls"]
    assert diag["final"]["cached"]["final_hits"] == 15
    assert diag["prefix"]["cached"]["final_hits"] == 0
    assert diag["prefix"]["cached"]["facts_hits"] == 15
    assert diag["mutation"]["cached"]["final_hits"] == 46
    assert diag["mutation"]["full"]["final_hits"] == 45
    (a.out / "diagnostics.json").write_text(json.dumps(diag, indent=2))
    rows = []
    rng = random.Random(20261023)
    for round_id in range(a.rounds):
        cases = list(variants)
        rng.shuffle(cases)
        for case in cases:
            modes = list(variants[case])
            rng.shuffle(modes)
            for mode in modes:
                row = run(case, mode, a.batches, "timing")
                row["round"] = round_id
                rows.append(row)
                (a.out / "timings.json").write_text(json.dumps(rows, indent=2))
        print("completed round", round_id + 1, flush=True)
    summary = {}
    for case, modes in variants.items():
        rr = [r for r in rows if r["scenario"] == case]
        paired = []
        for i in range(a.rounds):
            pair = {r["mode"]: r for r in rr if r["round"] == i}
            assert pair[modes[0]]["checksum"] == pair["cached"]["checksum"]
            paired.append(100 * (1 - pair["cached"]["user_s"] /
                                 pair[modes[0]]["user_s"]))
        summary[case] = {"baseline_mode": modes[0], "rounds": a.rounds,
                        "baseline_user_median": statistics.median(r["user_s"] for r in rr if r["mode"] == modes[0]),
                        "cached_user_median": statistics.median(r["user_s"] for r in rr if r["mode"] == "cached"),
                        "paired_cpu_reduction_pct_median": statistics.median(paired),
                        "paired_reductions_pct": paired}
    assert sha(binary) == identity["sha256"]
    (a.out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))

if __name__ == "__main__":
    main()
