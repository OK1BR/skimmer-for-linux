# WDSP subset — FIR designer + resampler + impulse cache (see VENDOR.md for
# provenance and for why this is a subset, unlike sdr-for-linux's full mirror).
#
# We build & link these sources unmodified (warnings silenced, never patched).
# malloc0 + the CriticalSection wrappers come from src/engine/wdsp_port.c
# (upstream's utilities.c / linux_port.c drag in the whole engine).

wdsp_inc = [
  include_directories('.'),
  include_directories('../rnnoise/include'),       # rnnoise.h for comm.h → rnnr.h
  include_directories('../libspecbleach/include'), # specbleach_adenoiser.h → sbnr.h
]

libwdsp = static_library('wdsp',
  [
    'fir.c',
    'resample.c',
    'impulse_cache.c',
    '../../src/engine/wdsp_port.c',
  ],
  include_directories : wdsp_inc,
  # GNU dialect (as upstream builds it): comm.h neutralises Windows __declspec
  # only under `#if defined(linux)`, and the bare `linux` macro exists only in
  # the GNU dialect — strict -std=c11 would leave __declspec unhandled.
  override_options : ['warning_level=0', 'c_std=gnu11'],
  dependencies : [fftw3f_dep, fftw3_dep, threads_dep, m_dep],
  install : false,
)

wdsp_dep = declare_dependency(
  link_with : libwdsp,
  include_directories : wdsp_inc,
  dependencies : [fftw3f_dep, fftw3_dep, threads_dep, m_dep],
)
