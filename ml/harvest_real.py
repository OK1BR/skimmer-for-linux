#!/usr/bin/env python3
# harvest_real.py — mine v2-labeled training pairs out of a SKIM_CW_DUMP_RUNS
# dump of a real recording (phase C: real vocabulary, real callsigns, real
# machine fists).
#
#   harvest_real.py dump1.txt [dump2.txt ...] --out real.json [--model run4.pt]
#
# Labels come from decode_cw_v2 ITSELF: the dump's <path>.chars sidecar carries
# every committed char with the END TIME OF ITS AUDIO plus v2's own quality
# state (elem_err, solid) at the commit. On machine keying a solid v2 read IS
# ground truth — squelch, lattice and duration priors are the label's
# defenses — and trust comes from v2's own judgment, never from the reader
# model. The first harvest used the model as a consensus checker and thereby
# DROPPED exactly the chunks the model misreads (run4's word-gap fusion never
# trained away — live-caught 2026-07-18); with v2 labels the model is out of
# the label path entirely. When --model is given it only FLAGS surviving
# chunks the model still misreads ("teach": true) so training can oversample
# them; it never gates.
#
# Per channel, overs split at ≥ --gap-ms spaces; overs split into word groups
# at ≥5-dit gaps; groups pack into ≤ --max-runs chunks (a 7-dit synthetic gap
# replaces a split-away word gap, as in training synthesis). Leading/trailing
# groups with NO v2 chars are trimmed — the raw run keyer runs below the
# squelch too, and those noise runs would otherwise drag an unlabeled prefix
# into the pair. Chunks keep their v2 chars verbatim (spacing included);
# drops: chars outside the training alphabet, < --min-label chars, E/T babble,
# weak label (solid fraction < --solid-frac or median elem_err > --max-err),
# and RUN INFLATION: a chunk whose run count exceeds --max-inflate × the
# label's own morse element count is a channel where v2 squelched away most
# of what the raw keyer heard — the splatter shadow of a strong neighbour.
# Its fragment label would teach the model to DROP real content (measured on
# block F: inflation < 1.3 → 0 % garbage labels, > 2.0 → 94 %).
#
# Offline tooling — not part of the product. GPL-3.0-or-later.
import argparse, json, statistics, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

T_EPS_MS = 50.0                        # char tau vs run commit-time slack

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

def channel_chars(path):
    """<dump>.chars -> {freq: [(t, char, elem_err, solid), ...]}.

    File order per channel IS text order (emit() appends to the pane text
    sequentially) and must be preserved: a word space and the lag-committed
    char before it can carry the same tau, and re-sorting would swap them
    (' ' < letters — every word's last char jumped across its space)."""
    chans = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) != 5:
                continue
            chans.setdefault(float(p[0]), []).append(
                (float(p[1]), chr(int(p[2])), float(p[3]), int(p[4])))
    return chans

def split_overs(stream, gap_ms, min_runs):
    overs, cur = [], []
    for t, key, dur in stream:
        if key == 0 and dur >= gap_ms:
            if len(cur) >= min_runs:
                overs.append(cur)
            cur = []
            continue
        if not cur and key == 0:
            continue                       # never start an over with a space
        cur.append((t, key, dur))
    if len(cur) >= min_runs:
        overs.append(cur)
    return overs

def word_groups(over, dit):
    """Split at ≥5-dit spaces into word groups (runs keep their times)."""
    groups, cur = [], []
    for t, k, d in over:
        if k == 0 and d >= 5.0 * dit:
            if cur:
                groups.append(cur)
            cur = []
            continue
        cur.append((t, k, d))
    if cur:
        groups.append(cur)
    return groups

def pack_groups(groups, max_runs):
    """Greedy: word groups -> chunks (lists of groups) of ≤ max_runs runs
    counting one synthetic gap per join. An oversized lone group stays a
    chunk of its own — the caller drops it (counted)."""
    chunks, cur, n = [], [], 0
    for g in groups:
        if cur and n + 1 + len(g) > max_runs:
            chunks.append(cur)
            cur, n = [], 0
        if cur:
            n += 1
        cur.append(g)
        n += len(g)
    if cur:
        chunks.append(cur)
    return chunks

def group_span(g):
    """[start, end] of a group's audio (run t is its commit ≈ end time)."""
    return g[0][0] - g[0][2], g[-1][0]

def chars_in(chars, t0, t1):
    return [c for c in chars if t0 - T_EPS_MS <= c[0] <= t1 + T_EPS_MS]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", help="reader .pt — flags 'teach' chunks only")
    ap.add_argument("--gap-ms", type=float, default=2500.0)
    ap.add_argument("--max-runs", type=int, default=300)
    ap.add_argument("--min-runs", type=int, default=24)
    ap.add_argument("--min-label", type=int, default=10)
    ap.add_argument("--max-cer", type=float, default=0.15)
    ap.add_argument("--babble-frac", type=float, default=0.55)
    ap.add_argument("--solid-frac", type=float, default=0.9)
    ap.add_argument("--max-err", type=float, default=0.25)
    ap.add_argument("--max-inflate", type=float, default=1.5)
    a = ap.parse_args()
    from extract_runs import MORSE
    elem = {v: len(k) for k, v in MORSE.items()}

    model = None
    if a.model:
        import torch
        from train_ctc import Reader
        model = Reader()
        model.load_state_dict(torch.load(a.model))
        model.eval()
    from train_ctc import C2I, features, greedy, cer

    pairs = []
    stats = {"chunks": 0, "nochars": 0, "oversize": 0, "short": 0, "alpha": 0,
             "babble": 0, "weak": 0, "inflate": 0, "kept": 0, "teach": 0}
    errs_kept = []
    for path in a.dumps:
        chans = channel_streams(path)
        chchars = channel_chars(path + ".chars")
        print(f"{path}: {len(chans)} channels", file=sys.stderr)
        for freq in sorted(chans):
            chars = chchars.get(freq, [])
            for over in split_overs(chans[freq], a.gap_ms, a.min_runs):
                marks = [d for _, k, d in over if k == 1]
                if len(marks) < 8:
                    continue
                dit = statistics.median(
                    sorted(marks)[: max(1, len(marks) * 2 // 3)])
                for chunk in pack_groups(word_groups(over, dit), a.max_runs):
                    stats["chunks"] += 1
                    while chunk and not chars_in(chars, *group_span(chunk[0])):
                        chunk = chunk[1:]          # noise prefix — no label
                    while chunk and not chars_in(chars, *group_span(chunk[-1])):
                        chunk = chunk[:-1]
                    if not chunk:
                        stats["nochars"] += 1
                        continue
                    runs, nruns = [], sum(len(g) for g in chunk)
                    if nruns + len(chunk) - 1 > a.max_runs:
                        stats["oversize"] += 1
                        continue
                    if nruns < a.min_runs:
                        stats["short"] += 1
                        continue
                    for gi, g in enumerate(chunk):
                        if gi:
                            runs.append((0, 7.0 * dit))
                        runs.extend((k, d) for _, k, d in g)
                    t0, _ = group_span(chunk[0])
                    _, t1 = group_span(chunk[-1])
                    cs = chars_in(chars, t0, t1)
                    label = " ".join("".join(c for _, c, _, _ in cs).split())
                    if len(label.replace(" ", "")) < a.min_label:
                        stats["short"] += 1
                        continue
                    if any(c != " " and c not in C2I for c in label):
                        stats["alpha"] += 1
                        continue
                    et = sum(1 for c in label if c in "ET ")
                    if et / len(label) > a.babble_frac:
                        stats["babble"] += 1
                        continue
                    expect = sum(2 * elem.get(c, 3)
                                 for c in label if c != " ")
                    if len(runs) > a.max_inflate * expect:
                        stats["inflate"] += 1
                        continue
                    body = [c for c in cs if c[1] != " "]
                    solid = sum(1 for c in body if c[3]) / len(body)
                    med_err = statistics.median(er for _, _, er, _ in body)
                    if solid < a.solid_frac or med_err > a.max_err:
                        stats["weak"] += 1
                        continue
                    stats["kept"] += 1
                    errs_kept.append(med_err)
                    pair = {"text": label,
                            "runs": [[k, round(d, 1)] for k, d in runs],
                            "err": round(med_err, 3), "freq": freq,
                            "t0": round(t0, 1)}
                    if model is not None:
                        import torch
                        with torch.no_grad():
                            x = torch.tensor(features(pair["runs"])).unsqueeze(0)
                            hyp = greedy(model(x)[0])
                        if cer(hyp, label) > a.max_cer:
                            pair["teach"] = True
                            stats["teach"] += 1
                    pairs.append(pair)
        del chans, chchars
    with open(a.out, "w") as f:
        json.dump(pairs, f)
    if errs_kept:
        errs_kept.sort()
        qs = [errs_kept[int(p * (len(errs_kept) - 1))] for p in (.5, .9)]
        print(f"kept med_err p50={qs[0]:.3f} p90={qs[1]:.3f}", file=sys.stderr)
    print(f"{stats} -> {a.out}", file=sys.stderr)

if __name__ == "__main__":
    main()
