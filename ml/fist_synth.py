#!/usr/bin/env python3
# fist_synth.py — synthetic hand-keyed CW for the reader prototype: QSO-shaped
# text run through a parametrized "fist" that turns it into mark/space
# durations with the pathologies real operators produce.
#
# The pathologies, each independently sampled per over:
#   - speed: 12-40 WPM base, per-element log-normal drift (random walk) plus
#     rare deliberate jumps — "meni svevolne rychlost v prubehu relace"
#   - element noise + weighting: dits/dahs stretched or squeezed per class
#   - bug fist: machine-tight dits, hand-stretched dahs (ratio up to ~5:1)
#   - gap chaos: char gaps anywhere from 1.6 to 7 dits, word gaps sampled
#     OVERLAPPING char gaps — the EA1EYL degeneracy (word ~ char gap) and the
#     EA3BP one (every letter its own word) both fall out of this range
#
# Text: a small QSO grammar (CQ calls, exchanges, ragchew) + random groups so
# the model cannot lean on grammar alone; numbers sometimes keyed the way ops
# key them (0 as O, 599 as 5NN).
#
# Offline tooling — not part of the product (the engine stays C; this feeds
# ml/train_ctc.py). Part of skimmer-for-linux. GPL-3.0-or-later.
import math, random

MORSE_ENC = {
    "A":".-","B":"-...","C":"-.-.","D":"-..","E":".","F":"..-.","G":"--.",
    "H":"....","I":"..","J":".---","K":"-.-","L":".-..","M":"--","N":"-.",
    "O":"---","P":".--.","Q":"--.-","R":".-.","S":"...","T":"-","U":"..-",
    "V":"...-","W":".--","X":"-..-","Y":"-.--","Z":"--..",
    "0":"-----","1":".----","2":"..---","3":"...--","4":"....-","5":".....",
    "6":"-....","7":"--...","8":"---..","9":"----.",
    "=":"-...-","+":".-.-.","?":"..--..","/":"-..-.",
}
ALPHABET = " " + "".join(sorted(MORSE_ENC))     # blank is handled by the model

PREFIXES = ("DL DJ DK DF OK OM SP SQ EA EA1 EA3 F G GM GW M IK IZ IU IW I "
            "ON PA OE HB9 HA YO S5 9A LZ UA UR RA RW UB SM LA OH OZ CT EI "
            "YU E7 Z3 SV 5B 4X VK JA W K N AA VE PY LU ZL YV TA UN ES YL "
            "LY R OZ TF EW EU").split()
NAMES = ("PETR IVAN JAN JOHN ANDRE KARL JURI TONI LADA MILAN PAVEL HANS "
         "JEAN MARIO IGOR ALEX RUDI FRED BILL DAVE MIKE STAN OLE LARS").split()
CITIES = ("PRAHA BRNO PARIS MADRID BERLIN CATANIA KYIV OSLO TOKYO LIMA "
          "WIEN SOFIA MOSKVA GDANSK NAPOLI TURKU BILBAO PLZEN GRAZ").split()
WX = "SUNNY CLOUDY RAINY WINDY SNOW FOGGY CLEAR HOT COLD".split()
ANTS = ("DIPOLE", "VERT", "LW", "GP", "3 EL YAGI", "2 EL QUAD", "INV V",
        "WINDOM", "MAGLOOP")

def gen_call(rng):
    c = rng.choice(PREFIXES)
    if not c[-1].isdigit():
        c += str(rng.randrange(10))
    n = rng.choices((1, 2, 3), weights=(15, 50, 35))[0]
    c += "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(n))
    return c

def _num(rng, n):
    s = str(n)
    if rng.random() < 0.35:
        s = s.replace("0", "O")                 # ops key 0 as O routinely
    return s

def gen_text(rng):
    a, b = gen_call(rng), gen_call(rng)
    name, city = rng.choice(NAMES), rng.choice(CITIES)
    rst = "5NN" if rng.random() < 0.5 else _num(rng, rng.choice((559, 579, 589, 599)))
    r = rng.random()
    if r < 0.30:                                # CQ call
        t = rng.choice((
            f"CQ CQ CQ DE {a} {a} {a} PSE K",
            f"CQ CQ DE {a} {a} K",
            f"CQ CQ CQ DE {a} {a} +",
            f"CQ TEST DE {a} {a}",
            f"TEST {a} {a} TEST",
            f"QRZ DE {a} K",
        ))
    elif r < 0.50:                              # contest / minimal exchange
        t = rng.choice((
            f"{b} DE {a} UR {rst} {rst} TU",
            f"{b} {rst} {_num(rng, rng.randrange(1, 1400))}",
            f"TU {a} TEST",
            f"R {rst} TU DE {a}",
            f"{b} DE {a} QSL 73 TU EE",
        ))
    elif r < 0.85:                              # ragchew over
        bits = [f"{b} DE {a}"]
        pool = [
            f"RR DR {name}", "TNX FER CALL", f"UR RST {rst} {rst}",
            f"NAME IS {name} {name}", f"QTH IS {city} {city}",
            f"RIG IS {_num(rng, rng.choice((5, 10, 50, 100, 500)))} WATTS",
            f"ANT IS {rng.choice(ANTS)}",
            f"WX IS {rng.choice(WX)} ES TEMP IS {_num(rng, rng.randrange(0, 36))} C",
            "ALL OK", "VY QSB", "HW COPY ?", "NW QRU", "HPE CU AGN",
            "TNX FER QSO", "GL ES GB", "HI HI",
        ]
        rng.shuffle(pool)
        bits += pool[: rng.randrange(2, 6)]
        bits.append(rng.choice((f"{b} DE {a} K", f"{b} DE {a} KN", "73 SK EE")))
        t = " = ".join(bits) if rng.random() < 0.6 else " ".join(bits)
    else:                                       # random groups — no grammar
        t = " ".join(
            "".join(rng.choice(ALPHABET[1:]) for _ in range(rng.randrange(2, 7)))
            for _ in range(rng.randrange(3, 9)))
    return t

class Fist:
    def __init__(self, rng):
        u = rng.uniform
        self.dit_ms = 1200.0 / u(12.0, 40.0)
        self.drift = u(0.0, 0.02)               # log-sigma per element
        self.jump_p = 0.008
        self.sig_mark = u(0.03, 0.22)
        self.w_dit = u(0.85, 1.25)
        self.w_dah = u(0.85, 1.25)
        if rng.random() < 0.25:                 # bug: tight dits, drawn dahs
            self.sig_mark = u(0.02, 0.06)
            self.w_dit = u(0.95, 1.05)
            self.w_dah = u(1.05, 1.7)
        self.esp_c = u(0.75, 1.5)
        self.sig_esp = u(0.05, 0.30)
        self.csp_c = u(1.6, 7.0)
        self.sig_csp = u(0.08, 0.35)
        self.wsp_c = max(self.csp_c * u(0.9, 2.5), u(3.5, 14.0))
        self.sig_wsp = u(0.08, 0.35)

    def _ln(self, rng, mean, sig):
        return mean * math.exp(rng.gauss(0.0, sig))

    def durations(self, rng, text):
        """text -> [(key, dur_ms), ...] starting and ending on a mark."""
        runs, dit = [], self.dit_ms
        words = [w for w in text.split() if w]
        for wi, word in enumerate(words):
            if wi:
                runs.append((0, self._ln(rng, self.wsp_c * dit, self.sig_wsp)))
            for ci, ch in enumerate(word):
                code = MORSE_ENC.get(ch)
                if not code:
                    continue
                if ci:
                    runs.append((0, self._ln(rng, self.csp_c * dit, self.sig_csp)))
                for ei, el in enumerate(code):
                    dit *= math.exp(rng.gauss(0.0, self.drift))
                    if rng.random() < self.jump_p:
                        dit *= rng.uniform(0.75, 1.3)
                    dit = min(max(dit, 1200.0 / 55.0), 1200.0 / 8.0)
                    if ei:
                        runs.append((0, self._ln(rng, self.esp_c * dit, self.sig_esp)))
                    base = self.w_dit * dit if el == "." else 3.0 * self.w_dah * dit
                    runs.append((1, self._ln(rng, base, self.sig_mark)))
        return [(k, round(max(d, 1.0), 1)) for k, d in runs]

def sample(rng):
    """One training pair: (runs, text). Text is guaranteed ALPHABET-clean and
    matches the keyed durations token for token."""
    text = gen_text(rng)
    text = " ".join(w for w in
                    ("".join(c for c in w if c in MORSE_ENC)
                     for w in text.split()) if w)
    return Fist(rng).durations(rng, text), text

if __name__ == "__main__":
    rng = random.Random(1)
    for _ in range(5):
        runs, text = sample(rng)
        print(f"{len(runs):4d} runs | {text}")
