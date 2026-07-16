#!/usr/bin/env python3
# extract_runs.py — pull one channel's mark/space runs out of a SKIM_CW_DUMP_RUNS
# dump and split them into overs.
#
#   extract_runs.py dump.txt --freq 14020002 [--tol 60] [--gap-ms 2500]
#                   [--min-runs 20] [--json out.json] [--decode]
#
# The dump is what decode_cw_v2.c writes: "freq_hz t_ms key dur_ms", one line
# per completed run, all channels interleaved. This filters to the channel(s)
# within --tol of --freq, splits the stream into overs on space runs longer
# than --gap-ms, and writes them as JSON: [{"t0_ms":…, "runs":[[key,dur_ms],…]},…].
#
# --decode adds a TRIVIAL adaptive-dit ITU decode per over (sanity check and
# hand-labelling aid only — the whole point of the ML prototype is that this
# classifier breaks on degenerate fists).
#
# Offline tooling for the CW reader prototype (docs in ml/train_ctc.py) — not
# part of the product. Part of skimmer-for-linux. GPL-3.0-or-later.
import argparse, json, statistics, sys

MORSE = {
    ".-":"A","-...":"B","-.-.":"C","-..":"D",".":"E","..-.":"F","--.":"G",
    "....":"H","..":"I",".---":"J","-.-":"K",".-..":"L","--":"M","-.":"N",
    "---":"O",".--.":"P","--.-":"Q",".-.":"R","...":"S","-":"T","..-":"U",
    "...-":"V",".--":"W","-..-":"X","-.--":"Y","--..":"Z",
    "-----":"0",".----":"1","..---":"2","...--":"3","....-":"4",".....":"5",
    "-....":"6","--...":"7","---..":"8","----.":"9",
    "-...-":"=",".-.-.":"+","..--..":"?","-..-.":"/",".-.-.-":".","--..--":",",
}

def trivial_decode(runs):
    marks = [d for k, d in runs if k == 1]
    if len(marks) < 3:
        return ""
    dit = statistics.median(sorted(marks)[: max(1, len(marks) * 2 // 3)])
    out, code = [], ""
    for k, d in runs:
        if k == 1:
            code += "." if d < 2.0 * dit else "-"
        else:
            if d < 2.0 * dit:
                continue
            out.append(MORSE.get(code, "*") if code else "")
            code = ""
            if d >= 5.0 * dit:
                out.append(" ")
    if code:
        out.append(MORSE.get(code, "*"))
    return "".join(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--freq", type=float, required=True)
    ap.add_argument("--tol", type=float, default=60.0)
    ap.add_argument("--gap-ms", type=float, default=2500.0)
    ap.add_argument("--min-runs", type=int, default=20)
    ap.add_argument("--json")
    ap.add_argument("--decode", action="store_true")
    a = ap.parse_args()

    stream = []                       # (t_ms, key, dur_ms) time-ordered
    with open(a.dump) as f:
        for line in f:
            p = line.split()
            if len(p) != 4:
                continue
            hz, t, key, dur = float(p[0]), float(p[1]), int(p[2]), float(p[3])
            if abs(hz - a.freq) <= a.tol:
                stream.append((t, key, dur))
    stream.sort()

    overs, cur = [], []
    for t, key, dur in stream:
        if key == 0 and dur >= a.gap_ms:
            if len(cur) >= a.min_runs:
                overs.append(cur)
            cur = []
            continue
        if not cur and key == 0:
            continue                  # never start an over with a space
        cur.append((t, key, dur))
    if len(cur) >= a.min_runs:
        overs.append(cur)

    out = [{"t0_ms": o[0][0], "runs": [[k, d] for _, k, d in o]} for o in overs]
    print(f"{len(stream)} runs -> {len(out)} overs", file=sys.stderr)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f)
    if a.decode:
        for o in out:
            t0 = o["t0_ms"] / 1000.0
            print(f"[{t0:7.1f}s {len(o['runs']):4d} runs] "
                  f"{trivial_decode([(k, d) for k, d in o['runs']])}")

if __name__ == "__main__":
    main()
