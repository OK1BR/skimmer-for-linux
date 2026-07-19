#!/usr/bin/env python3
"""A/B spot harness: our telnet feed vs SDC (or any cluster-dialect skimmer).

Collects "DX de ..." lines from two telnet spot feeds side by side, logs them
as JSONL, and diffs the two streams: who spotted whom first (or at all),
frequency offsets between the sides, and same-frequency/different-call
conflicts (mutation suspects, adjudicated against MASTER.SCP when present).

    ab_spots.py collect --a localhost:7300 --b sdc-host:7300 \
                        [--label-a ours --label-b sdc] [--login OK1BR] \
                        [--minutes 30] [--out /var/tmp/skimmer-ab/<ts>]
    ab_spots.py report  <dir> [--freq-tol 0.3] [--pad 300]

Caveats the numbers carry (printed in the report header too):
  - our feed is CQ-only + score >= 0.85 by policy (stricter than the
    panadapter gate) — expect "ours misses" that the pane actually had;
  - the cluster line carries 0.1 kHz, so frequency deltas quantize to
    100 Hz on BOTH sides regardless of the decoders' true resolution.

Stdlib only; Ctrl-C during collect finishes cleanly and prints the report.
"""

import argparse
import json
import os
import re
import signal
import socket
import sys
import threading
import time
from datetime import datetime, timezone

# --- line parsing ------------------------------------------------------------------

DX_HEAD = re.compile(
    r"DX de\s+(?P<spotter>\S+?):?\s+(?P<khz>\d+\.\d+)\s+(?P<call>[A-Z0-9/]{3,})\s*(?P<tail>.*)",
    re.IGNORECASE)
SNR_RE   = re.compile(r"(-?\d+)\s*dB", re.IGNORECASE)
SPEED_RE = re.compile(r"(\d+)\s*(WPM|BPS|BD)", re.IGNORECASE)
LOGIN_RE = re.compile(r"(call(sign)?|login)\s*[:>]", re.IGNORECASE)

def parse_dx(line):
    """DX line -> spot dict, or None. Tolerant of dialect drift (RBN, SDC,
    CW Skimmer and our rbn_feed.c all share the head; the tail varies)."""
    m = DX_HEAD.match(line)
    if not m:
        return None
    tail = m.group("tail")
    mode = None
    ttok = tail.split()
    if ttok and ttok[0].isalpha():
        mode = ttok[0].upper()
    snr = SNR_RE.search(tail)
    spd = SPEED_RE.search(tail)
    return {
        "spotter": m.group("spotter").upper(),
        "khz":  float(m.group("khz")),
        "call": m.group("call").upper(),
        "mode": mode,
        "snr":  int(snr.group(1)) if snr else None,
        "speed": int(spd.group(1)) if spd else None,
        "cq":   " CQ " in (" " + tail.upper() + " "),
    }

# --- collection --------------------------------------------------------------------

class FeedReader(threading.Thread):
    """One telnet feed: login handshake, DX lines -> JSONL; reconnects."""

    def __init__(self, label, host, port, login, mode_filter, path, stop_ev):
        super().__init__(daemon=True, name=label)
        self.label, self.host, self.port = label, host, port
        self.login, self.mode_filter = login, mode_filter
        self.path, self.stop_ev = path, stop_ev
        self.n_spots = 0
        self.calls = set()
        self.connected = False

    def run(self):
        backoff = 1
        with open(self.path, "a", encoding="utf-8") as out:
            while not self.stop_ev.is_set():
                try:
                    self._session(out)
                    backoff = 1
                except OSError as e:
                    if not self.stop_ev.is_set():
                        print(f"[{self.label}] {e}; reconnect in {backoff}s",
                              file=sys.stderr)
                self.connected = False
                if self.stop_ev.wait(backoff):
                    return
                backoff = min(backoff * 2, 30)

    def _session(self, out):
        sock = socket.create_connection((self.host, self.port), timeout=10)
        sock.settimeout(1.0)
        self.connected = True
        print(f"[{self.label}] connected to {self.host}:{self.port}",
              file=sys.stderr)
        buf = b""
        logged_in = False
        connect_t = time.time()
        with sock:
            while not self.stop_ev.is_set():
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    # Some servers prompt without a newline; some not at all.
                    if not logged_in and time.time() - connect_t > 3:
                        sock.sendall((self.login + "\r\n").encode())
                        logged_in = True
                    continue
                if not chunk:
                    raise OSError("feed closed the connection")
                buf += chunk
                if not logged_in and LOGIN_RE.search(buf.decode("latin-1")):
                    sock.sendall((self.login + "\r\n").encode())
                    logged_in = True
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    self._line(raw.decode("latin-1").strip("\r "), out)

    def _line(self, line, out):
        spot = parse_dx(line)
        if not spot:
            return
        if self.mode_filter and spot["mode"] and spot["mode"] != self.mode_filter:
            return
        spot["t"] = time.time()
        spot["raw"] = line
        out.write(json.dumps(spot) + "\n")
        out.flush()
        self.n_spots += 1
        self.calls.add(spot["call"])

def cmd_collect(args):
    outdir = args.out or os.path.join(
        "/var/tmp/skimmer-ab",
        datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S"))
    os.makedirs(outdir, exist_ok=True)
    meta = {
        "a": {"label": args.label_a, "addr": args.a},
        "b": {"label": args.label_b, "addr": args.b},
        "login": args.login, "mode": args.mode,
        "started": datetime.now(timezone.utc).isoformat(),
    }
    with open(os.path.join(outdir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)

    stop_ev = threading.Event()
    readers = []
    for key, addr, label in (("a", args.a, args.label_a),
                             ("b", args.b, args.label_b)):
        host, port = addr.rsplit(":", 1)
        readers.append(FeedReader(label, host, int(port), args.login,
                                  args.mode, os.path.join(outdir, key + ".jsonl"),
                                  stop_ev))
    signal.signal(signal.SIGINT, lambda *_: stop_ev.set())
    signal.signal(signal.SIGTERM, lambda *_: stop_ev.set())
    for r in readers:
        r.start()

    deadline = time.time() + args.minutes * 60 if args.minutes else None
    print(f"collecting into {outdir}  (Ctrl-C stops and reports)",
          file=sys.stderr)
    while not stop_ev.is_set():
        wait = 60 if not deadline else max(0.2, min(60, deadline - time.time()))
        if stop_ev.wait(wait):
            break
        if deadline and time.time() >= deadline:
            break
        tally = "  ".join(
            f"{r.label}: {r.n_spots} spots/{len(r.calls)} calls"
            f"{'' if r.connected else ' [DOWN]'}" for r in readers)
        both = len(readers[0].calls & readers[1].calls)
        print(f"-- {datetime.now().strftime('%H:%M')}  {tally}  both: {both}",
              file=sys.stderr)
    stop_ev.set()
    for r in readers:
        r.join(timeout=3)
    print(file=sys.stderr)
    report(outdir, args.freq_tol, args.pad)

# --- analysis ----------------------------------------------------------------------

def levenshtein(a, b):
    if len(a) < len(b):
        a, b = b, a
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[-1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]

def load_side(path):
    spots = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    spots.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    spots.sort(key=lambda s: s["t"])
    return spots

def cluster_events(spots, freq_tol, gap_s=900):
    """Per-call activity events: re-spots of one station chain into one event
    (same call, freq within tol of the running mean, gap below gap_s)."""
    events = []
    by_call = {}
    for s in spots:
        chain = by_call.setdefault(s["call"], [])
        ev = None
        for cand in chain:
            if (abs(s["khz"] - cand["khz"]) <= freq_tol
                    and s["t"] - cand["last"] <= gap_s):
                ev = cand
                break
        if ev is None:
            ev = {"call": s["call"], "khz": s["khz"], "first": s["t"],
                  "last": s["t"], "n": 0, "snr": s["snr"], "paired": None}
            chain.append(ev)
            events.append(ev)
        ev["khz"] = (ev["khz"] * ev["n"] + s["khz"]) / (ev["n"] + 1)
        ev["last"] = max(ev["last"], s["t"])
        ev["n"] += 1
        if s["snr"] is not None and (ev["snr"] is None or s["snr"] > ev["snr"]):
            ev["snr"] = s["snr"]
    return events

def overlaps(ea, eb, pad):
    return ea["first"] - pad <= eb["last"] and eb["first"] - pad <= ea["last"]

def load_scp():
    path = os.path.expanduser("~/.config/skimmer-for-linux/master.scp")
    try:
        with open(path, encoding="latin-1") as f:
            return {ln.strip().upper() for ln in f
                    if ln.strip() and not ln.startswith("#")}
    except OSError:
        return None

def fmt_t(t):
    return datetime.fromtimestamp(t).strftime("%H:%M:%S")

def report(outdir, freq_tol, pad):
    with open(os.path.join(outdir, "meta.json")) as f:
        meta = json.load(f)
    la, lb = meta["a"]["label"], meta["b"]["label"]
    sa = load_side(os.path.join(outdir, "a.jsonl"))
    sb = load_side(os.path.join(outdir, "b.jsonl"))
    ea = cluster_events(sa, freq_tol)
    eb = cluster_events(sb, freq_tol)

    print(f"=== A/B spot report — {la} ({meta['a']['addr']}) vs "
          f"{lb} ({meta['b']['addr']}) ===")
    print(f"caveats: '{la}' telnet policy is CQ-only score>=0.85; "
          f"wire resolution 0.1 kHz both sides")
    print(f"{la}: {len(sa)} spots, {len(ea)} station-events, "
          f"{len({s['call'] for s in sa})} calls")
    print(f"{lb}: {len(sb)} spots, {len(eb)} station-events, "
          f"{len({s['call'] for s in sb})} calls")
    if not sa or not sb:
        print("one side is empty — nothing to diff")
        return

    # pair events: same call, freq within tol, time overlap (padded)
    pairs = []
    for a in ea:
        for b in eb:
            if (b["paired"] is None and a["call"] == b["call"]
                    and abs(a["khz"] - b["khz"]) <= freq_tol
                    and overlaps(a, b, pad)):
                a["paired"] = b
                b["paired"] = a
                pairs.append((a, b))
                break

    dts  = sorted(a["first"] - b["first"] for a, b in pairs)
    dfs  = [round(a["khz"] - b["khz"], 3) for a, b in pairs]
    a_first = sum(1 for dt in dts if dt < -1)
    b_first = sum(1 for dt in dts if dt > 1)
    print(f"\n--- paired stations: {len(pairs)} ---")
    if pairs:
        med = dts[len(dts) // 2]
        print(f"first-spot: {la} earlier {a_first}x, {lb} earlier {b_first}x, "
              f"median dt {med:+.0f}s ({la} minus {lb})")
        mean_df = sum(dfs) / len(dfs)
        sd = (sum((d - mean_df) ** 2 for d in dfs) / len(dfs)) ** 0.5
        print(f"freq delta ({la}-{lb}): mean {mean_df * 1000:+.0f} Hz, "
              f"sd {sd * 1000:.0f} Hz")
        hist = {}
        for d in dfs:
            hist[round(d, 1)] = hist.get(round(d, 1), 0) + 1
        print("  " + "  ".join(f"{k * 1000:+.0f}Hz:{v}"
                               for k, v in sorted(hist.items())))

    scp = load_scp()

    # unpaired events with a different-call counterpart = mutation suspects
    suspects = []
    for a in ea:
        if a["paired"]:
            continue
        for b in eb:
            if (not b["paired"] and a["call"] != b["call"]
                    and abs(a["khz"] - b["khz"]) <= freq_tol
                    and overlaps(a, b, pad)
                    and levenshtein(a["call"], b["call"]) <= 2):
                verdict = ""
                if scp:
                    ina, inb = a["call"] in scp, b["call"] in scp
                    if ina != inb:
                        verdict = f"  SCP says: {a['call'] if ina else b['call']}"
                suspects.append((a, b, verdict))
                a["paired"], b["paired"] = b, a
                break
    print(f"\n--- mutation suspects (same freq, edit distance <=2): "
          f"{len(suspects)} ---")
    for a, b, verdict in suspects:
        print(f"  {a['khz']:9.1f}  {la}: {a['call']:<12} ({a['n']}x)  "
              f"{lb}: {b['call']:<12} ({b['n']}x){verdict}")

    def only(events, label):
        lone = [e for e in events if not e["paired"]]
        lone.sort(key=lambda e: (e["khz"], e["first"]))
        print(f"\n--- {label} only: {len(lone)} station-events ---")
        for e in lone:
            snr = f"{e['snr']:3d} dB" if e["snr"] is not None else "  ? dB"
            print(f"  {e['khz']:9.1f}  {e['call']:<12} {snr}  {e['n']:3d}x  "
                  f"{fmt_t(e['first'])}-{fmt_t(e['last'])}")
    only(ea, la)
    only(eb, lb)

def cmd_report(args):
    report(args.dir, args.freq_tol, args.pad)

# --- cli ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("collect", help="attach to both feeds and log spots")
    c.add_argument("--a", required=True, metavar="HOST:PORT",
                   help="side A feed (ours: localhost:7300)")
    c.add_argument("--b", required=True, metavar="HOST:PORT",
                   help="side B feed (SDC's telnet server)")
    c.add_argument("--label-a", default="ours")
    c.add_argument("--label-b", default="sdc")
    c.add_argument("--login", default="OK1BR",
                   help="callsign sent at the login prompt")
    c.add_argument("--minutes", type=float, default=0,
                   help="stop after N minutes (0 = until Ctrl-C)")
    c.add_argument("--mode", default="CW",
                   help="keep only this mode (empty = all)")
    c.add_argument("--out", help="output dir (default /var/tmp/skimmer-ab/<ts>)")
    c.add_argument("--freq-tol", type=float, default=0.3)
    c.add_argument("--pad", type=float, default=300)
    c.set_defaults(func=cmd_collect)

    r = sub.add_parser("report", help="re-run the diff on a collected dir")
    r.add_argument("dir")
    r.add_argument("--freq-tol", type=float, default=0.3,
                   help="pairing tolerance in kHz")
    r.add_argument("--pad", type=float, default=300,
                   help="time-overlap padding in seconds")
    r.set_defaults(func=cmd_report)

    args = ap.parse_args()
    args.func(args)

if __name__ == "__main__":
    main()
