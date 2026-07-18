#!/usr/bin/env python3
# train_ctc.py — the CW reader: a dilated TCN + CTC over SYMBOLIC mark/space
# durations (not audio). Phase A: causal features + bounded lookahead, so the
# same net streams live with a ~2-4 s commit lag (see Reader.LOOK).
#
#   train_ctc.py [--steps 30000] [--out /var/tmp/skimmer-cw-ml]
#
# Why symbolic: for a STRONG hand-keyed station the Schmitt keying is clean —
# what breaks the classical decoder is the operator's timing (torn/fused gaps,
# speed changes mid-over, bug fists). So the model reads the run sequence the
# engine already produces (SKIM_CW_DUMP_RUNS format) and learns timing AND
# lexical context jointly; CTC handles the unknown alignment.
#
# Features per run: [log(dur / causal windowed mark median), key ? +1 : -1,
# clamped log ratios to prev/next run]. The median normalization makes the
# input speed-invariant; the window (MED_WIN marks) keeps it causal AND
# tracking mid-over speed changes. Model: 12-layer dilated TCN -> linear,
# ~310k params — small enough for a dependency-free C forward pass (weights
# exported as a flat .npz, see export()).
#
# Training data is fist_synth.py, generated on the fly (infinite, seeded).
# Eval prints CER on a fixed validation set: 'clean' (mild fists) vs 'ugly'
# (the degenerate tail this exists for).
#
# Offline tooling — not part of the product. GPL-3.0-or-later.
import argparse, json, math, os, random, statistics, sys, time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fist_synth import ALPHABET, Fist, gen_text, sample

BLANK = 0
CHARS = ALPHABET                                  # index 1.. in CTC space
C2I = {c: i + 1 for i, c in enumerate(CHARS)}
I2C = {i + 1: c for i, c in enumerate(CHARS)}

MED_WIN = 31                                      # causal mark-median window

def features(runs, med_win=MED_WIN):
    """[log(dur/median mark), key ±1, log ratio to prev, log ratio to next] —
    the ratios hand the net local timing structure (dit vs dah vs gap class)
    it would otherwise have to learn as differencing. Clamped: a glitch next
    to a word gap is a huge but uninformative ratio.

    The mark median is CAUSAL: median of the last med_win marks up to and
    INCLUDING run i — exactly what streaming C inference can maintain — and
    a windowed median also follows mid-over speed changes the whole-over one
    blurred. med_win=None restores the v1 whole-over median (run2 blobs)."""
    ds = [max(d, 1.0) for _, d in runs]
    if med_win is None:
        marks = [d for (k, _), d in zip(runs, ds) if k == 1]
        med_all = statistics.median(marks) if marks else 100.0
    out, win = [], []
    for i, (k, _) in enumerate(runs):
        d = ds[i]
        if med_win is None:
            med = med_all
        else:
            if k == 1:
                win.append(d)
                if len(win) > med_win:
                    win.pop(0)
            med = statistics.median(win) if win else 100.0
        f2 = math.log(d / ds[i - 1]) if i > 0 else 0.0
        f3 = math.log(d / ds[i + 1]) if i + 1 < len(ds) else 0.0
        out.append([math.log(d / med), 1.0 if k == 1 else -1.0,
                    max(-3.0, min(3.0, f2)), max(-3.0, min(3.0, f3))])
    return out

class Reader(nn.Module):
    """Dilated TCN with a BOUNDED right receptive field (phase A, streaming):
    kernel-3 taps sit at (-2d, -d, 0) on causal layers and (-d, 0, +d) where
    LOOK[i] == d. Total lookahead = sum(LOOK) = 22 runs — the commit lag
    (~2-4 s of keying, fine+mid scales only, budgeted for live streaming) —
    while the left field grows to ~230 runs ≈ ~35 chars of lexical context.
    look=DIL restores the v1 symmetric net bit-exactly (run2 blobs).
    All-conv on purpose: parallel over T on CPU, and the C port is a handful
    of conv loops instead of LSTM gate plumbing. Pre-norm residual blocks
    (per-timestep LayerNorm over channels) — the un-normed variant plateaued
    at CER ~0.30."""
    DIL  = (1, 2, 4, 8, 16, 32, 1, 2, 4, 8, 16, 32)
    LOOK = (1, 2, 4, 8, 0,  0,  1, 2, 4, 0,  0,  0)
    IN = 4

    def __init__(self, ch=96, look=None):
        super().__init__()
        self.look = self.LOOK if look is None else look
        self.convs = nn.ModuleList(
            nn.Conv1d(self.IN if i == 0 else ch, ch, 3, dilation=d)
            for i, d in enumerate(self.DIL))
        self.norms = nn.ModuleList(
            nn.LayerNorm(ch) for _ in self.DIL[1:])
        self.head = nn.Conv1d(ch, 1 + len(CHARS), 1)

    def _pad(self, x, i):                         # [B, ch, T] -> padded
        d, r = self.DIL[i], self.look[i]
        return F.pad(x, (2 * d - r, r))

    def forward(self, x, lens=None):
        y = torch.relu(self.convs[0](self._pad(x.transpose(1, 2), 0)))
        for i, conv in enumerate(self.convs[1:].__iter__()):
            z = self.norms[i](y.transpose(1, 2)).transpose(1, 2)
            y = y + torch.relu(conv(self._pad(z, i + 1)))
        return self.head(y).transpose(1, 2)                # [B, T, C]

def greedy(logits):
    """CTC greedy decode of one [T, C] tensor."""
    ids = logits.argmax(-1).tolist()
    out, prev = [], BLANK
    for i in ids:
        if i != BLANK and i != prev:
            out.append(I2C[i])
        prev = i
    return "".join(out)

def cer(hyp, ref):
    """Levenshtein distance / len(ref)."""
    if not ref:
        return 1.0 if hyp else 0.0
    prev = list(range(len(hyp) + 1))
    for i, r in enumerate(ref, 1):
        cur = [i]
        for j, h in enumerate(hyp, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1,
                           prev[j - 1] + (r != h)))
        prev = cur
    return prev[-1] / len(ref)

def load_real(paths, teach_boost=1):
    """v2-labeled real pairs (ml/harvest_real.py) -> [(runs, text)].
    Chunks the harvest flagged "teach" (the reader model misread them —
    its systematic errors) enter the pool teach_boost times."""
    out = []
    for p in paths:
        for e in json.load(open(p)):
            n = teach_boost if e.get("teach") else 1
            out.extend([([(k, d) for k, d in e["runs"]], e["text"])] * n)
    return out

def make_batch(rng, n, max_runs=320, real=None, real_frac=0.0):
    pairs = []
    while len(pairs) < n:
        if real and rng.random() < real_frac:
            r0, t = real[rng.randrange(len(real))]
            sp = rng.uniform(0.85, 1.2)      # small pools repeat: re-speed
            r = [(k, max(1.0, d * sp * math.exp(rng.gauss(0.0, 0.03))))
                 for k, d in r0]             # + per-run timing jitter
        else:
            r, t = sample(rng)
        if 4 <= len(r) <= max_runs and t:         # cap T for CPU throughput
            pairs.append((r, t))
    T = max(len(r) for r, _ in pairs)
    x = torch.zeros(len(pairs), T, Reader.IN)
    lens, targets, tlens = [], [], []
    for bi, (runs, text) in enumerate(pairs):
        f = features(runs)
        x[bi, : len(f)] = torch.tensor(f)
        lens.append(len(f))
        targets.extend(C2I[c] for c in text)
        tlens.append(len(text))
    return (x, torch.tensor(lens), torch.tensor(targets),
            torch.tensor(tlens), [t for _, t in pairs])

def val_set(seed, n, ugly):
    """Fixed validation pairs; ugly=True forces the degenerate-fist corner."""
    rng = random.Random(seed)
    out = []
    while len(out) < n:
        text = gen_text(rng)
        text = " ".join(w for w in
                        ("".join(c for c in w if c in C2I) for w in text.split())
                        if w)
        f = Fist(rng)
        if ugly:                                  # EA1EYL/EA3BP territory
            f.csp_c = rng.uniform(4.0, 7.0)
            f.wsp_c = f.csp_c * rng.uniform(0.95, 1.6)
            f.sig_csp = rng.uniform(0.2, 0.35)
            f.drift = rng.uniform(0.01, 0.02)
        runs = f.durations(rng, text)
        if len(runs) >= 4 and text:
            out.append((runs, text))
    return out

@torch.no_grad()
def eval_cer(model, pairs):
    model.eval()
    tot = 0.0
    for runs, text in pairs:
        x = torch.tensor(features(runs)).unsqueeze(0)
        logits = model(x, torch.tensor([x.shape[1]]))[0]
        tot += cer(greedy(logits), text)
    model.train()
    return tot / len(pairs)

def export(model, path):
    """Flat .npz — one array per parameter, C-inference friendly."""
    np.savez(path, **{k: v.detach().numpy() for k, v in
                      model.state_dict().items()})

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=30000)
    ap.add_argument("--batch", type=int, default=48)
    ap.add_argument("--out", default="/var/tmp/skimmer-cw-ml")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--init", help="warm-start checkpoint (fine-tuning)")
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--real", action="append", default=[],
                    help="harvest_real.py json (repeatable) mixed into batches")
    ap.add_argument("--real-frac", type=float, default=0.25)
    ap.add_argument("--real-val", action="append", default=[],
                    help="held-out real json — CER reported at every eval")
    ap.add_argument("--teach-boost", type=int, default=3,
                    help="oversampling factor for harvest 'teach' pairs")
    ap.add_argument("--threads", type=int, default=4)   # small LSTM: more
    a = ap.parse_args()                                  # threads = lock churn
    os.makedirs(a.out, exist_ok=True)
    torch.set_num_threads(a.threads)
    torch.manual_seed(42)

    model = Reader()
    print(f"params: {sum(p.numel() for p in model.parameters())}")
    if a.init:
        model.load_state_dict(torch.load(a.init))
        print(f"warm start: {a.init}")
    if a.resume and os.path.exists(f"{a.out}/model.pt"):
        model.load_state_dict(torch.load(f"{a.out}/model.pt"))
        print("resumed")
    real = load_real(a.real, teach_boost=a.teach_boost)
    rval = load_real(a.real_val)
    if real or rval:
        print(f"real pairs: train {len(real)}, val {len(rval)}")
    opt = torch.optim.Adam(model.parameters(), lr=a.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.steps, a.lr / 10)
    ctc = nn.CTCLoss(blank=BLANK, zero_infinity=True)

    rng = random.Random(1234)
    vclean = val_set(91, 120, ugly=False)
    vugly = val_set(92, 120, ugly=True)
    vreal = random.Random(93).sample(rval, min(150, len(rval))) if rval else []
    best = 9e9
    t0 = time.time()
    for step in range(1, a.steps + 1):
        x, lens, targets, tlens, _ = make_batch(rng, a.batch, real=real,
                                                real_frac=a.real_frac)
        logits = model(x, lens)                   # [B, T, C]
        loss = ctc(logits.permute(1, 0, 2).log_softmax(-1),
                   targets, lens, tlens)
        opt.zero_grad()
        loss.backward()
        nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        sched.step()
        if step % 100 == 0:
            print(f"step {step:6d} loss {loss.item():.3f} "
                  f"[{time.time() - t0:.0f}s]", flush=True)
        if step % 500 == 0 or step == a.steps:
            cc, cu = eval_cer(model, vclean), eval_cer(model, vugly)
            cr = eval_cer(model, vreal) if vreal else float("nan")
            el = time.time() - t0
            print(f"step {step:6d} loss {loss.item():.3f} "
                  f"CER clean {cc:.3f} ugly {cu:.3f} real {cr:.3f} "
                  f"[{el:.0f}s]", flush=True)
            score = cc + cu + (cr if vreal else 0.0)
            if score < best:
                best = score
                torch.save(model.state_dict(), f"{a.out}/model.pt")
                export(model, f"{a.out}/model.npz")
    print(f"best clean+ugly CER sum: {best:.3f}; saved to {a.out}")

if __name__ == "__main__":
    main()
