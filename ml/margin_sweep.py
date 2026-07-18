#!/usr/bin/env python3
# margin_sweep.py — calibrate the per-word confidence gate (RR_MARG_MEAN /
# RR_MARG_MIN in decode_cw_v2.c) against labeled data instead of a hand fit.
#
#   SKIM_CW_V2=1 SKIM_CW_READER=<blob> SKIM_RR_DBG=1 \
#   SKIM_CW_DUMP_RUNS=dump.txt skimmer-replay rec.cf32 2> rr.log
#   margin_sweep.py rr.log dump.txt.chars
#
# The replay emits one "RRWORD freq t=<ms> mean=<m> min=<m> OK|REJ |word|"
# line per word the reader streams (t = end of the word's audio, sample
# clock), and the same run writes v2's own decode with char end times into
# the .chars dump. On machine keying v2 is the reference: a reader word is
# CORRECT iff it exactly matches the v2 word whose audio ends nearest (the
# per-word gate replaces v2 draft text, so "disagrees with v2" = harm on a
# machine channel — hand-fist corrections are validated separately on the
# EA1EYL/EA3BP replays). Sweeps the (mean, min) grid and prints the
# precision/accept table; pick the operating point, then set RR_MARG_*.
#
# Offline tooling — not part of the product. GPL-3.0-or-later.
import argparse, re, sys

T_SLACK_MS = 300.0

def load_words(chars_path):
    """.chars -> {freq: [(t_end_last_char, word), ...]} (v2 text, file order)."""
    chans = {}
    with open(chars_path) as f:
        for line in f:
            p = line.split()
            if len(p) != 5:
                continue
            chans.setdefault(float(p[0]), []).append(
                (float(p[1]), chr(int(p[2]))))
    words = {}
    for freq, cs in chans.items():
        out, cur, t_last = [], [], None
        for t, c in cs:
            if c == " ":
                if cur:
                    out.append((t_last, "".join(cur)))
                cur = []
                continue
            cur.append(c)
            t_last = t
        if cur:
            out.append((t_last, "".join(cur)))
        words[freq] = out
    return words

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rrlog")
    ap.add_argument("chars", nargs="+")
    ap.add_argument("--show-harm", action="store_true",
                    help="print wrong words the swept gate would accept")
    a = ap.parse_args()

    truth = {}
    for path in a.chars:
        for freq, ws in load_words(path).items():
            truth.setdefault(freq, []).extend(ws)

    rx = re.compile(r"RRWORD ([0-9.]+) t=([-0-9.]+) mean=([-0-9.]+) "
                    r"min=([-0-9.]+) (OK|REJ|MUT)\s+\|(.*)\|")
    cand = []                          # (mean, min, word, correct)
    n_lines = n_not = 0
    with open(a.rrlog) as f:
        for line in f:
            m = rx.search(line)
            if not m:
                continue
            n_lines += 1
            freq, t = float(m.group(1)), float(m.group(2))
            mean, lo = float(m.group(3)), float(m.group(4))
            word = m.group(6).strip()
            if len(word) < 2 or t < 0:
                continue
            best = None
            for wt, w in truth.get(freq, []):
                d = abs(wt - t)
                if d <= T_SLACK_MS and (best is None or d < best[0]):
                    best = (d, w)
            # Three classes, not two: MATCH = the v2 word verbatim; CONFLICT
            # = v2 decoded something ELSE there (accepting would overwrite a
            # classical read — the mutation class the gate exists to stop);
            # ORPHAN = v2 was silent (weak-signal adds and babble both land
            # here — judged by eye and by the hand-fist replays, not here).
            if best is None:
                n_not += 1
                cls = "orphan"
            elif best[1] == word:
                cls = "match"
            else:
                cls = "conflict"
            cand.append((mean, lo, word, cls, best[1] if best else ""))
    n_match = sum(1 for c in cand if c[3] == "match")
    print(f"{n_lines} RRWORD lines -> {len(cand)} candidates "
          f"({n_match} match, {n_not} orphan)")

    print(f"\n{'mean':>5} {'min':>4} | {'acc':>5} {'match':>6} {'confl':>6} "
          f"{'orphan':>6} {'mut%':>5} {'recall':>6}")
    for M in (3, 4, 5, 6, 7, 8, 9, 10, 12):
        for m in (0, 1, 2, 3, 4, 5):
            acc = [c for c in cand if c[0] >= M and c[1] >= m]
            if not acc:
                continue
            ok = sum(1 for c in acc if c[3] == "match")
            cf = sum(1 for c in acc if c[3] == "conflict")
            orp = len(acc) - ok - cf
            rec = ok / n_match if n_match else 0.0
            mut = cf / (ok + cf) if ok + cf else 0.0
            print(f"{M:5.1f} {m:4.1f} | {len(acc):5d} {ok:6d} {cf:6d} "
                  f"{orp:6d} {mut:5.2f} {rec:6.3f}")
    if a.show_harm:
        print("\n-- CONFLICTS accepted at the current 6/3 gate --")
        for mean, lo, word, cls, v2w in cand:
            if cls == "conflict" and mean >= 6.0 and lo >= 3.0:
                print(f"  mean={mean:5.1f} min={lo:4.1f} |{word}| v2=|{v2w}|")

if __name__ == "__main__":
    main()
