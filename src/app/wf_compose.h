/*
 * wf_compose.h — waterfall history + image composer for the M8 view.
 *
 * GLib-only on purpose: the spectrum gate exercises the frequency↔pixel
 * mapping and the pooling here, where a mirrored band or an off-by-one bin
 * would otherwise hide behind a GTK widget. The widget (wf_view.c) owns the
 * texture and the interaction; this file owns the pixels.
 *
 * Geometry (CW Skimmer's layout, Richard 2026-09-05): frequency runs
 * VERTICALLY — the top pixel row is the HIGHEST frequency of the visible
 * window — and time runs sideways, newest column at the RIGHT edge. Where
 * several spectrum bins share a pixel row (zoomed out) the pixel takes the
 * MAXIMUM, never the mean: a 50 Hz CW line must not vanish into its
 * neighbours' noise. The same for time (rows_per_px history rows per column).
 *
 * The colour map auto-ranges on the tracked noise floor (a low percentile
 * of each incoming row, smoothed) and spans SKIM_WF_SPAN_DB above it; the
 * palette table is copied from sdr-for-linux's waterfall.c (same author,
 * same licence) so the two apps look alike. Bytes are spectrum.h's
 * dBFS + 200.
 */
#ifndef SKIM_WF_COMPOSE_H
#define SKIM_WF_COMPOSE_H

#include <glib.h>

#define SKIM_WF_HISTORY_ROWS  2048   /* 21.8 s at 93.75 rows/s; 16 MB at 192 k  */
#define SKIM_WF_ROWS_PER_PX   2      /* time pooling: 21.3 ms per pixel column   */
#define SKIM_WF_SPAN_DB       40.0   /* noise floor → top of the palette          */
#define SKIM_WF_FLOOR_PCT     20     /* percentile used as the floor estimate     */
#define SKIM_WF_FLOOR_SMOOTH  0.01   /* EMA per row (τ ≈ 1 s at 94 rows/s)        */
#define SKIM_WF_DB_OFFSET     200.0  /* spectrum.h byte encoding                  */

typedef struct _SkimWfHistory SkimWfHistory;

SkimWfHistory *skim_wf_history_new(guint max_rows);
void           skim_wf_history_free(SkimWfHistory *h);

/* Append one spectrum row (nbins bytes). A change of nbins, bin width or
 * stream centre means the band moved: the history is cleared first. */
void skim_wf_history_push(SkimWfHistory *h, const guint8 *row, guint nbins,
                          double center_hz, double bin_hz);
void skim_wf_history_clear(SkimWfHistory *h);

guint  skim_wf_history_rows(const SkimWfHistory *h);       /* stored rows      */
guint  skim_wf_history_bins(const SkimWfHistory *h);
double skim_wf_history_center_hz(const SkimWfHistory *h);
double skim_wf_history_bin_hz(const SkimWfHistory *h);
double skim_wf_history_lo_hz(const SkimWfHistory *h);      /* band edges       */
double skim_wf_history_hi_hz(const SkimWfHistory *h);
double skim_wf_history_floor_db(const SkimWfHistory *h);   /* tracked, dBFS    */

/* The visible window: frequency at the TOP pixel edge (y = 0) and at the
 * BOTTOM edge (y = hgt), plus the time pooling factor. */
typedef struct {
  double f_top_hz;
  double f_bot_hz;
  guint  rows_per_px;
} SkimWfWindow;

/* Fractional pixel y of a frequency (0 = top edge), and back. */
double skim_wf_y_of_hz(const SkimWfWindow *win, int hgt, double hz);
double skim_wf_hz_of_y(const SkimWfWindow *win, int hgt, double y);

/* Compose the rightmost `cols` columns (cols = w composes everything) of a
 * w×hgt buffer of 0xAARRGGBB pixels (native endian — B8G8R8A8 bytes on
 * little-endian). Newest time at x = w − 1. Pixels outside the band or
 * beyond the stored history take the palette's floor colour. */
void skim_wf_compose(const SkimWfHistory *h, const SkimWfWindow *win,
                     guint32 *pix, int w, int hgt, int cols);

/* Palette (sdr-for-linux waterfall.c table; index 0 = "Classic"). */
int         skim_wf_palette_count(void);
const char *skim_wf_palette_name(int idx);
int         skim_wf_get_palette(void);
void        skim_wf_set_palette(int idx);
void        skim_wf_palette_rgb(double t, double *r, double *g, double *b);
guint32     skim_wf_palette_argb(guint8 idx);          /* LUT entry            */

#endif /* SKIM_WF_COMPOSE_H */
