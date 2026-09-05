/*
 * spectrum.h — band spectrum rows for the waterfall view (M8).
 *
 * An FFT tap on the raw IQ stream, independent of the channelizer: the
 * channelizer's 125/250 Hz channels are far too coarse for a picture, and a
 * picture must show keying, which means a window SHORTER than a dit
 * (≈ 40–50 ms at contest speed). Time and frequency resolution trade
 * one for one (Δf · Δt ≈ 1), so the tap is defined by two named constants:
 *
 *   SKIM_SPECTRUM_BIN_HZ   target bin width — N is derived from the rate so
 *                          the bin stays the same at 48/96/192/384 k
 *                          (iq_samplerate is device-global radio state);
 *   SKIM_SPECTRUM_HOP_DIV  rows per window — the hop is N / HOP_DIV.
 *
 * At 192 k that is N = 8192 (42.7 ms Hann window, 23.4 Hz bins) and a row
 * every 10.7 ms. Both are first guesses for Richard's live look, not
 * conclusions.
 *
 * Orientation: the TCI wire carries the TRUE spectrum (CLAUDE.md — do not
 * conjugate), so a forward FFT puts a +f tone in a positive bin. Rows are
 * fftshifted: row[i] is the power at offset (i − N/2) · bin_hz from the
 * stream centre; +12 kHz lands ABOVE the middle. The gate asserts it.
 *
 * Byte encoding (sdr-for-linux waterfall convention, shifted to dBFS):
 * byte = dBFS + SKIM_SPECTRUM_DB_OFFSET, clamped to 0..255. A full-scale
 * tone at bin centre reads ~200; a −120 dBFS noise floor reads ~80; the
 * viewer auto-ranges on the floor, so absolute calibration is not needed.
 *
 * GLib-only, one thread (the engine thread — fftw's planner is not
 * thread-safe, so the object is built where the channelizer is built).
 */
#ifndef SKIM_SPECTRUM_H
#define SKIM_SPECTRUM_H

#include <glib.h>

#define SKIM_SPECTRUM_BIN_HZ     23.4375     /* 192000 / 8192                */
#define SKIM_SPECTRUM_HOP_DIV    4           /* hop = N / 4 → 93.75 rows/s   */
#define SKIM_SPECTRUM_DB_OFFSET  200.0

typedef struct _SkimSpectrum SkimSpectrum;

/* One finished row: nbins bytes, fftshifted (index 0 = lowest frequency). */
typedef void (*SkimSpectrumRowCb)(const guint8 *row, guint nbins, gpointer user);

SkimSpectrum *skim_spectrum_new(double rate);
void          skim_spectrum_free(SkimSpectrum *s);

guint  skim_spectrum_bins(const SkimSpectrum *s);      /* N                  */
double skim_spectrum_bin_hz(const SkimSpectrum *s);    /* rate / N           */
guint  skim_spectrum_hop(const SkimSpectrum *s);       /* frames per row     */

void skim_spectrum_set_row_cb(SkimSpectrum *s, SkimSpectrumRowCb cb, gpointer user);

/* Feed interleaved I/Q frames; the row callback fires from inside. */
void skim_spectrum_push(SkimSpectrum *s, const float *iq, guint nframes);

/* Forget buffered samples (re-enable after a pause: no stale half-row). */
void skim_spectrum_reset(SkimSpectrum *s);

#endif /* SKIM_SPECTRUM_H */
