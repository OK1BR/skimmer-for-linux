#!/usr/bin/env python3
# train_ctc.py — the CW reader prototype: a small bidirectional LSTM + CTC over
# SYMBOLIC mark/space durations (not audio).
#
#   train_ctc.py [--steps 30000] [--out /var/tmp/skimmer-cw-ml]
#
# Why symbolic: for a STRONG hand-keyed station the Schmitt keying is clean —
# what breaks the classical decoder is the operator's timing (torn/fused gaps,
# speed changes mid-over, bug fists). So the model reads the run sequence the
# engine already produces (SKIM_CW_DUMP_RUNS format) and learns timing AND
# lexical context jointly; CTC handles the unknown alignment.
#
# Features per run: [log(dur / median mark dur of the over), key ? +1 : -1].
# The per-over normalization makes the input speed-invariant; drift within the
# over stays visible relatively. Model: 2×96 BiLSTM -> linear, ~310k params —
# small enough for a dependency-free C forward pass later (weights exported as
# a flat .npz, see export()).
#
# Training data is fist_synth.py, generated on the fly (infinite, seeded).
# Eval prints CER on a fixed validation set: 'clean' (mild fists) vs 'ugly'
# (the degenerate tail this exists for).
#
# Offline tooling — not part of the product. GPL-3.0-or-later.
import argparse, math, os, random, statistics, sys, time

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fist_synth import ALPHABET, Fist, gen_text, sample

BLANK = 0
CHARS = ALPHABET                                  # index 1.. in CTC space
C2I = {c: i + 1 for i, c in enumerate(CHARS)}
I2C = {i + 1: c for i, c in enumerate(CHARS)}

def features(runs):
    """[log(dur/median mark), key ±1, log ratio to prev, log ratio to next] —
    the ratios hand the net local timing structure (dit vs dah vs gap class)
    it would otherwise have to learn as differencing. Clamped: a glitch next
    to a word gap is a huge but uninformative ratio."""
    marks = [d for k, d in runs if k == 1]
    med = statistics.median(marks) if marks else 100.0
    ds = [max(d, 1.0) for _, d in runs]
    out = []
    for i, (k, d) in enumerate(runs):
        d = max(d, 1.0)
        f2 = math.log(d / ds[i - 1]) if i > 0 else 0.0
        f3 = math.log(d / ds[i + 1]) if i + 1 < len(ds) else 0.0
        out.append([math.log(d / med), 1.0 if k == 1 else -1.0,
                    max(-3.0, min(3.0, f2)), max(-3.0, min(3.0, f3))])
    return out

class Reader(nn.Module):
    """Dilated TCN, non-causal (sees both directions): receptive field
    ±~250 runs ≈ ±35 chars — enough lexical context to resolve torn gaps.
    All-conv on purpose: parallel over T on CPU, and the later C port is a
    handful of conv loops instead of LSTM gate plumbing. Pre-norm residual
    blocks (per-timestep LayerNorm over channels) — the un-normed variant
    plateaued at CER ~0.30."""
    DIL = (1, 2, 4, 8, 16, 32, 1, 2, 4, 8, 16, 32)
    IN = 4

    def __init__(self, ch=96):
        super().__init__()
        self.convs = nn.ModuleList(
            nn.Conv1d(self.IN if i == 0 else ch, ch, 3, padding=d, dilation=d)
            for i, d in enumerate(self.DIL))
        self.norms = nn.ModuleList(
            nn.LayerNorm(ch) for _ in self.DIL[1:])
        self.head = nn.Conv1d(ch, 1 + len(CHARS), 1)

    def forward(self, x, lens=None):
        y = torch.relu(self.convs[0](x.transpose(1, 2)))   # [B, ch, T]
        for i, conv in enumerate(self.convs[1:].__iter__()):
            z = self.norms[i](y.transpose(1, 2)).transpose(1, 2)
            y = y + torch.relu(conv(z))
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

def make_batch(rng, n, max_runs=320):
    pairs = []
    while len(pairs) < n:
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
    ap.add_argument("--threads", type=int, default=4)   # small LSTM: more
    a = ap.parse_args()                                  # threads = lock churn
    os.makedirs(a.out, exist_ok=True)
    torch.set_num_threads(a.threads)
    torch.manual_seed(42)

    model = Reader()
    print(f"params: {sum(p.numel() for p in model.parameters())}")
    if a.resume and os.path.exists(f"{a.out}/model.pt"):
        model.load_state_dict(torch.load(f"{a.out}/model.pt"))
        print("resumed")
    opt = torch.optim.Adam(model.parameters(), lr=2e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.steps, 2e-4)
    ctc = nn.CTCLoss(blank=BLANK, zero_infinity=True)

    rng = random.Random(1234)
    vclean = val_set(91, 120, ugly=False)
    vugly = val_set(92, 120, ugly=True)
    best = 9e9
    t0 = time.time()
    for step in range(1, a.steps + 1):
        x, lens, targets, tlens, _ = make_batch(rng, a.batch)
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
            el = time.time() - t0
            print(f"step {step:6d} loss {loss.item():.3f} "
                  f"CER clean {cc:.3f} ugly {cu:.3f} [{el:.0f}s]", flush=True)
            score = cc + cu
            if score < best:
                best = score
                torch.save(model.state_dict(), f"{a.out}/model.pt")
                export(model, f"{a.out}/model.npz")
    print(f"best clean+ugly CER sum: {best:.3f}; saved to {a.out}")

if __name__ == "__main__":
    main()
