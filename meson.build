project('skimmer-for-linux', 'c',
  version : '0.0.0',
  license : 'GPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language : 'c')
add_project_arguments('-DSKIMMER_VERSION="@0@"'.format(meson.project_version()),
                      language : 'c')

# --- Dependencies -----------------------------------------------------------
# The engine is GLib-only (headless). The app adds GTK4/libadwaita. As milestones
# land, their deps are added here: M1 → libwebsockets (TCI client), M2 → fftw3f
# (+ vendored WDSP resampler). They are declared now because they are already on
# the system (sdr-for-linux uses them) and the engine will reach for them soon.
cc        = meson.get_compiler('c')
glib_dep  = dependency('glib-2.0')            # headless engine
gio_dep   = dependency('gio-2.0')             # M6: RBN telnet server sockets
gtk_dep   = dependency('gtk4')                # app front-end
adw_dep   = dependency('libadwaita-1')        # app control surface
m_dep     = cc.find_library('m', required : false)
lws_dep   = dependency('libwebsockets')       # M1: TCI client transport
fftw3f_dep  = dependency('fftw3f')            # M2: channelizer FFT (float)
fftw3_dep   = dependency('fftw3')             # WDSP fir.c min-phase path (double)
threads_dep = dependency('threads')

# WDSP subset (FIR designer + resampler) — declares wdsp_dep. VENDOR.md there
# records provenance and the subset rationale.
subdir('vendor/wdsp')

engine_inc = include_directories('src/engine')

# --- Engine (headless, GLib-only) -------------------------------------------
# Grows milestone by milestone (docs/SCOPE.md). M0: skeleton modules that build
# and expose the architecture; no pipeline wired yet.
engine_sources = [
  'src/engine/tci_client.c',   # M1: WS client, IQ ingest, conjugate, outgoing SPOT
  'src/engine/channelizer.c',  # M2: polyphase filter bank → complex channels
  'src/engine/decode_cw.c',    # M3: CW backend (implements decode.h)
  'src/engine/decode_cw_v2.c', # CW v2: semi-Markov Viterbi (QSB-proof)
  'src/engine/cw_reader.c',    # neural over re-reader (TCN+CTC, pane-only)
  'src/engine/tone_split.c',   # per-channel carrier splitter (multi-signal)
  'src/engine/station.c',      # M5: per-frequency station tracker
  'src/engine/callsign.c',     # M4: extraction + validation
  'src/engine/spot_out.c',     # M5/M6: spot policy — dedup, rate, sinks
  'src/engine/rbn_feed.c',     # M6: RBN telnet server (CW Skimmer dialect)
  'src/engine/pipeline.c',     # M5: the engine assembled end to end
]
engine_deps = [glib_dep, gio_dep, lws_dep, wdsp_dep, fftw3f_dep, m_dep]

engine_lib = static_library('skimmer-engine',
  engine_sources,
  include_directories : engine_inc,
  dependencies : engine_deps,
)

# --- Gates (offline tests + live probes, sdr-for-linux house style) ---------
# M1: offline gate — mock TCI server + skim_tci_client round-trip (handshake,
# Stream reassembly, conjugate orientation, spot plumbing).
skimmer_tci_test = executable('skimmer-tci-test',
  ['src/tci_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('tci-client', skimmer_tci_test, timeout : 60)

# M1: live gate — IQ ingest + orientation eyeball check against a running
# sdr-for-linux (ws://host:40001).
executable('skimmer-tci-probe',
  ['src/tci_probe_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# M3 A/B harness: offline decode of a probe-recorded cf32 sample through the
# full pipeline — decoder versions diff on the same off-air corpus.
executable('skimmer-replay',
  ['src/replay_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# M2: WDSP subset smoke — headers compile against our c11, fir_bandpass +
# create_resample link and behave (the sdrfl-wdsp-smoke counterpart).
skimmer_wdsp_smoke = executable('skimmer-wdsp-smoke',
  ['src/wdsp_smoke.c'],
  dependencies : [wdsp_dep, m_dep],
  # includes WDSP's comm.h, which needs the GNU dialect (see vendor/wdsp/meson.build)
  override_options : ['c_std=gnu11'],
)
test('wdsp-smoke', skimmer_wdsp_smoke, timeout : 30)

# M2: offline gate — polyphase channelizer on synthetic tones (isolation,
# alias rejection, phase preservation, CPU headroom).
skimmer_chan_test = executable('skimmer-chan-test',
  ['src/chan_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('channelizer', skimmer_chan_test, timeout : 120)

# M3: offline gate — CW backend on synthetic keying (speeds, SNR, jitter,
# QSB, speed change, squelch-on-noise, channelizer integration).
skimmer_cw_test = executable('skimmer-cw-test',
  ['src/cw_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('cw-decode', skimmer_cw_test, timeout : 120)

# Tone splitter: offline gate — sideband immunity, Δf 50/30 Hz splits with
# both texts copied, Δf 15 Hz contested, slot TTL collapse/re-engage, and the
# WHOLE offline pipeline with SKIM_TONE_SPLIT=1 (both calls, no mutations).
skimmer_split_test = executable('skimmer-split-test',
  ['src/split_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('tone-split', skimmer_split_test, timeout : 120)

# M4: offline gate — callsign validity (ITU allocation), labelled-corpus
# precision/recall, fragment feeds, dictionary boost, noise fuzz.
skimmer_call_test = executable('skimmer-call-test',
  ['src/call_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('callsign', skimmer_call_test, timeout : 60)

# Neural CW reader: C forward pass vs an independent reference, plus the
# in-repo blob's torch-baked test vector and stream==batch equivalence —
# the shipped weights are gated on every `meson test`.
skimmer_reader_test = executable('skimmer-reader-test',
  ['src/reader_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('cw-reader', skimmer_reader_test, timeout : 60,
  env : ['SKIM_READER_BLOB=' + meson.project_source_root() / 'data/cw-reader.bin'])

# M5: offline gate — station tracker units, spot policy units, and the WHOLE
# pipeline over a real WebSocket against a mock TCI server (spots come back).
skimmer_spot_test = executable('skimmer-spot-test',
  ['src/spot_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('spot-pipeline', skimmer_spot_test, timeout : 120)

# M6: offline gate — the RBN telnet feed: login handshake, cluster line
# format, multi-client broadcast; then the OFFLINE pipeline against a local
# telnet sink (CQ-only etiquette, dedup, exact kHz, no unvalidated calls).
skimmer_rbn_test = executable('skimmer-rbn-test',
  ['src/rbn_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('rbn-feed', skimmer_rbn_test, timeout : 120)

# --- Application ------------------------------------------------------------
executable('skimmer-for-linux',
  ['src/app/main.c'],
  include_directories : engine_inc,
  dependencies : [gtk_dep, adw_dep, glib_dep],
  link_with : engine_lib,
  install : true,
  c_args : ['-DSKIMMER_DATADIR="@0@"'.format(
    get_option('prefix') / get_option('datadir') / 'skimmer-for-linux')],
)

# The neural CW reader weights (data/cw-reader.md documents provenance).
install_data('data/cw-reader.bin',
  install_dir : get_option('datadir') / 'skimmer-for-linux')
