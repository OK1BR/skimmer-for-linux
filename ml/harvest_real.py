#!/usr/bin/env python3
# harvest_real.py — mine consensus-labeled training pairs out of a
# SKIM_CW_DUMP_RUNS dump of a real recording (phase C: real vocabulary,
# real callsigns, real machine fists).
#
#   harvest_real.py model.pt dump1.txt [dump2.txt ...] --out real.json
#
# Machine/contest keying is the trivial classifier's home turf: on a solid
# channel its read IS ground truth. The reader model acts as an independent
# CHECKER only — a chunk survives when trivial and model agree (CER ≤
# --max-cer); where they disagree neither is trusted and the chunk drops.
# Labels therefore never teach the model its own mistakes, and never teach
# it the trivial decoder's mistakes either (the model would disagree there).
#
# Per channel, overs split at ≥ --gap-ms spaces; long overs chunk at word
# gaps into ≤ --max-runs pieces (chunks start and end on a mark). Reads
# containing '*' (unreadable code), babble (mostly E/T) and characters
# outside the training alphabet are dropped.
#
# Offline tooling — not part of the product. GPL-3.0-or-later.
import argparse, json, statistics, sys, os

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from train_ctc import Reader, features, greedy, cer, C2I
from extract_runs import trivial_decode

def channel_streams(path):
    """dump -> {freq: [(t, key, dur), ...]} (each channel time-ordered)."""
    chans = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) != 4:
                continue
            chans.setdefault(float(p[0]), []).append(
                (float(p[1]), int(p[2]), float(p[3])))
    for runs in chans.values():
        runs.sort()
    return chans

def split_overs(stream, gap_ms, min_runs):
    overs, cur = [], []
    for _, key, dur in stream:
        if key == 0 and dur >= gap_ms:
            if len(cur) >= min_runs:
                overs.append(cur)
            cur = []
            continue
        if not cur and key == 0:
            continue                       # never start an over with a space
        cur.append((key, dur))
    if len(cur) >= min_runs:
        overs.append(cur)
    return overs

def chunk_over(runs, max_runs, min_runs):
    """Split at word gaps into mark-to-mark chunks of ≤ max_runs."""
    marks = [d for k, d in runs if k == 1]
    if len(marks) < 8:
        return []
    dit = statistics.median(sorted(marks)[: max(1, len(marks) * 2 // 3)])
    groups, cur = [], []                   # word groups incl. inner gaps
    for k, d in runs:
        if k == 0 and d >= 5.0 * dit:
            if cur:
                groups.append(cur)
            cur = []
            continue
        cur.append((k, d))
    if cur:
        groups.append(cur)
    chunks, cur = [], []
    for g in groups:
        if cur and len(cur) + 1 + len(g) > max_runs:
            chunks.append(cur)
            cur = list(g)
        else:
            if cur:
                cur.append((0, 7.0 * dit))     # the word gap we split away
            cur.extend(g)
    if cur:
        chunks.append(cur)
    out = []
    for c in chunks:
        while c and c[0][0] == 0:
            c = c[1:]
        while c and c[-1][0] == 0:
            c = c[:-1]
        if len(c) >= min_runs:
            out.append(c)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--gap-ms", type=float, default=2500.0)
    ap.add_argument("--max-runs", type=int, default=300)
    ap.add_argument("--min-runs", type=int, default=24)
    ap.add_argument("--max-cer", type=float, default=0.15)
    ap.add_argument("--babble-frac", type=float, default=0.55)
    a = ap.parse_args()

    model = Reader()
    model.load_state_dict(torch.load(a.model))
    model.eval()

    pairs, stats = [], {"chunks": 0, "starred": 0, "alpha": 0, "babble": 0,
                        "short": 0, "disagree": 0, "kept": 0}
    for path in a.dumps:
        chans = channel_streams(path)
        print(f"{path}: {len(chans)} channels", file=sys.stderr)
        for freq in sorted(chans):
            for over in split_overs(chans[freq], a.gap_ms, a.min_runs):
                for chunk in chunk_over(over, a.max_runs, a.min_runs):
                    stats["chunks"] += 1
                    label = " ".join(trivial_decode(chunk).split())
                    if "*" in label:
                        stats["starred"] += 1
                        continue
                    if len(label) < 10:
                        stats["short"] += 1
                        continue
                    if any(c != " " and c not in C2I for c in label):
                        stats["alpha"] += 1
                        continue
                    et = sum(1 for c in label if c in "ET ")
                    if et / len(label) > a.babble_frac:
                        stats["babble"] += 1
                        continue
                    with torch.no_grad():
                        x = torch.tensor(features(chunk)).unsqueeze(0)
                        hyp = greedy(model(x)[0])
                    if cer(hyp, label) > a.max_cer:
                        stats["disagree"] += 1
                        continue
                    stats["kept"] += 1
                    pairs.append({"text": label,
                                  "runs": [[k, round(d, 1)] for k, d in chunk]})
        del chans
    with open(a.out, "w") as f:
        json.dump(pairs, f)
    print(f"{stats} -> {a.out}", file=sys.stderr)

if __name__ == "__main__":
    main()
