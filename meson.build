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
  'src/engine/station.c',      # M5: per-frequency station tracker
  'src/engine/callsign.c',     # M4: extraction + validation
  'src/engine/spot_out.c',     # M5/M6: TCI SPOT + RBN telnet feed
]
engine_deps = [glib_dep, lws_dep, wdsp_dep, fftw3f_dep, m_dep]

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

# --- Application ------------------------------------------------------------
executable('skimmer-for-linux',
  ['src/app/main.c'],
  include_directories : engine_inc,
  dependencies : [gtk_dep, adw_dep, glib_dep],
  link_with : engine_lib,
  install : true,
)
