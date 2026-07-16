#!/usr/bin/env python3
# eval_real.py — run the trained CW reader on REAL overs (extract_runs.py JSON)
# and print it next to the trivial classifier; with --truth also the CER.
#
#   eval_real.py model.pt overs.json [--over N] [--t0 S --t1 S] [--truth "..."]
#
# --t0/--t1 slice an over by stream time (seconds) — real captures merge
# consecutive transmissions when the operator never pauses long enough
# (EA3BP keys through 180 s without a 2.5 s gap).
#
# Offline tooling — not part of the product. GPL-3.0-or-later.
import argparse, json, sys, os

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from train_ctc import Reader, features, greedy, cer
from extract_runs import trivial_decode

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("overs")
    ap.add_argument("--over", type=int, default=-1)
    ap.add_argument("--t0", type=float)
    ap.add_argument("--t1", type=float)
    ap.add_argument("--truth")
    a = ap.parse_args()

    model = Reader()
    model.load_state_dict(torch.load(a.model))
    model.eval()

    overs = json.load(open(a.overs))
    idxs = range(len(overs)) if a.over < 0 else [a.over]
    for i in idxs:
        o = overs[i]
        runs, t = [], o["t0_ms"]
        for k, d in o["runs"]:
            tt = t / 1000.0
            if (a.t0 is None or tt >= a.t0) and (a.t1 is None or tt <= a.t1):
                runs.append((k, d))
            t += d
        if len(runs) < 4:
            continue
        with torch.no_grad():
            x = torch.tensor(features(runs)).unsqueeze(0)
            hyp = greedy(model(x)[0])
        print(f"--- over {i} ({len(runs)} runs, t0 {o['t0_ms']/1000.0:.1f}s)")
        print(f"TRIV : {trivial_decode(runs)}")
        print(f"MODEL: {hyp}")
        if a.truth:
            print(f"TRUTH: {a.truth}")
            print(f"CER  : triv {cer(trivial_decode(runs), a.truth):.3f}  "
                  f"model {cer(hyp, a.truth):.3f}")

if __name__ == "__main__":
    main()
