#!/usr/bin/env python3
# export_c.py — pack the trained reader (train_ctc.py checkpoint) into the flat
# little-endian blob src/engine/cw_reader.c loads, plus a baked-in test vector
# (fixed input -> torch logits) so the C forward pass can prove itself
# numerically in the offline gate.
#
#   export_c.py /var/tmp/skimmer-cw-ml/run1/model.pt model.bin
#
# Blob layout (all little-endian, f32 weights):
#   "CWRD" u32 ver=2 | u32 n_conv, ch, n_out, in_dim | u32 alpha_len, alphabet
#   per conv: u32 in_ch, dil, has_norm | [gamma[in_ch] beta[in_ch]]
#             | w[ch][in_ch][3] | b[ch]
#   head: w[n_out][ch] | b[n_out]
#   test vector: u32 T | feats f32[T][in_dim] | logits f32[T][n_out]
# has_norm marks a pre-norm residual block: per-timestep LayerNorm (eps 1e-5)
# over in_ch channels is applied to the conv INPUT, and the block output is
# residual-added (layer 0 has neither).
#
# Offline tooling — not part of the product. GPL-3.0-or-later.
import struct, sys, os

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from train_ctc import Reader, CHARS

def main():
    src, dst = sys.argv[1], sys.argv[2]
    model = Reader()
    model.load_state_dict(torch.load(src))
    model.eval()

    sd = model.state_dict()
    n_conv = len(model.convs)
    ch = sd["convs.0.weight"].shape[0]
    n_out = sd["head.weight"].shape[0]

    torch.manual_seed(7)
    T = 64
    x = torch.randn(1, T, model.IN)
    with torch.no_grad():
        logits = model(x)[0]                      # [T, n_out]

    with open(dst, "wb") as f:
        f.write(b"CWRD" + struct.pack("<IIIII", 2, n_conv, ch, n_out,
                                      model.IN))
        f.write(struct.pack("<I", len(CHARS)) + CHARS.encode())
        for i in range(n_conv):
            w = sd[f"convs.{i}.weight"]           # [ch, in, 3]
            b = sd[f"convs.{i}.bias"]
            f.write(struct.pack("<III", w.shape[1], model.DIL[i],
                                1 if i > 0 else 0))
            if i > 0:
                f.write(sd[f"norms.{i-1}.weight"].numpy().astype("<f4").tobytes())
                f.write(sd[f"norms.{i-1}.bias"].numpy().astype("<f4").tobytes())
            f.write(w.numpy().astype("<f4").tobytes())
            f.write(b.numpy().astype("<f4").tobytes())
        f.write(sd["head.weight"].squeeze(-1).numpy().astype("<f4").tobytes())
        f.write(sd["head.bias"].numpy().astype("<f4").tobytes())
        f.write(struct.pack("<I", T))
        f.write(x[0].numpy().astype("<f4").tobytes())
        f.write(logits.numpy().astype("<f4").tobytes())
    print(f"{dst}: {os.path.getsize(dst)} bytes, {n_conv} convs, ch {ch}, "
          f"out {n_out}, test T={T}")

if __name__ == "__main__":
    main()
