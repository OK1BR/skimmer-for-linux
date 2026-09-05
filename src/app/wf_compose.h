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
#define SKIM_WF_LABEL_LAG     2      /* rows the centre LABEL leads the DATA by:
                                      * the label leaves the server the moment
                                      * the knob moves, the IQ captured at that
                                      * centre lands here after the DDC apply,
                                      * the IQ transport and half an FFT window.
                                      * MEASURED live 2026-09-05 evening against
                                      * sdr-for-linux with the immediate HP
                                      * kick (51f981a): 972 voting rows of a
                                      * knob sweep — L=2: 557, L=1: 305, L=3:
                                      * 103; four discrete TCI steps with the
                                      * knob still: data flipped 3, 2, 3, 2 rows
                                      * after the label (≈ 21 ms at 94 rows/s).
                                      * Before the kick: 3–11 rows, the SDR's
                                      * 100 ms keepalive quantum. Re-measure
                                      * with SKIM_WF_DEBUG=1 (episode summary) */
#define SKIM_WF_LABEL_LAG_MAX 63     /* ceiling for the SKIM_WF_LAG_ROWS override  */

typedef struct _SkimWfHistory SkimWfHistory;

SkimWfHistory *skim_wf_history_new(guint max_rows);
void           skim_wf_history_free(SkimWfHistory *h);

/* Append one spectrum row (nbins bytes) taken at stream centre center_hz.
 * The history lives in ABSOLUTE frequency: every row carries its own bin
 * shift against the grid anchor (the first centre seen), so a retune of the
 * radio — which moves the whole IQ band, sdr-for-linux has no CTUN — leaves
 * every old row exactly where it was and only writes the new rows shifted.
 * Only a change of nbins or bin width (a rate change) clears the history. */
void skim_wf_history_push(SkimWfHistory *h, const guint8 *row, guint nbins,
                          double center_hz, double bin_hz);
void skim_wf_history_clear(SkimWfHistory *h);

guint  skim_wf_history_rows(const SkimWfHistory *h);       /* stored rows      */
guint  skim_wf_history_bins(const SkimWfHistory *h);
double skim_wf_history_center_hz(const SkimWfHistory *h);  /* NEWEST row's     */
double skim_wf_history_bin_hz(const SkimWfHistory *h);
double skim_wf_history_lo_hz(const SkimWfHistory *h);      /* current band     */
double skim_wf_history_hi_hz(const SkimWfHistory *h);      /* (newest row)     */
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

/* Label delay line. The stream centre label rides the TCI control channel
 * and leaves the server the moment the knob moves; the IQ captured at that
 * centre arrives `lag` rows later (DDC apply + IQ transport + half an FFT
 * window). Stamping every row with the label that was current `lag` rows
 * EARLIER places it on the right absolute frequency — nothing is delayed and
 * nothing is dropped, so the picture FLOWS through a retune; the traces slope
 * while the knob turns because they really did sweep through the window.
 * Rows are the clock (one per hop), so `lag` is time. Replaces the 0.7 s
 * settle DROP of 2026-09-05 morning, which paused the picture on every
 * retune (Richard: it must flow). */
typedef struct _SkimWfDelay SkimWfDelay;

SkimWfDelay *skim_wf_delay_new(guint lag_rows);
void         skim_wf_delay_free(SkimWfDelay *d);
void         skim_wf_delay_set_lag(SkimWfDelay *d, guint lag_rows);   /* ≤ LAG_MAX  */
guint        skim_wf_delay_lag(const SkimWfDelay *d);
/* Feed this row's label, get the label the row is placed on. While the line
 * fills (a fresh stream) rows ride their own label, never a stale one; a
 * bin-width change (rate change) restarts the line on the new label. */
double       skim_wf_delay_push(SkimWfDelay *d, double center_hz, double bin_hz);

/* Palette (sdr-for-linux waterfall.c table; index 0 = "Classic"). */
int         skim_wf_palette_count(void);
const char *skim_wf_palette_name(int idx);
int         skim_wf_get_palette(void);
void        skim_wf_set_palette(int idx);
void        skim_wf_palette_rgb(double t, double *r, double *g, double *b);
guint32     skim_wf_palette_argb(guint8 idx);          /* LUT entry            */

/* -------- callsign column layout ---------------------------------------------- */

/* One label of the callsign column: the caller fills anchor_y (the station's
 * frequency as a pixel y in the picture, skim_wf_y_of_hz) and prio; the
 * layout fills y — the label's CENTRE line — or −1 when the label is hidden
 * for lack of room. */
typedef struct {
  double anchor_y;
  int    prio;      /* higher wins a seat when the column is over capacity   */
  double y;
} SkimWfLabel;

/* Seat n labels of `pitch` px each on a column `hgt` px tall (CW Skimmer's
 * callsign column): no two overlap, the frequency ORDER is kept (a higher
 * station's label never drops below a lower's), every label sits as close to
 * its anchor as the neighbours allow — a cluster of near-coincident stations
 * spreads symmetrically about its mean anchor instead of piling downward —
 * and nothing leaves the column. More labels than fit: the lowest-priority
 * ones are hidden (y = −1; their dots still mark the frequency). Any input
 * order is accepted; anchors outside [0, hgt) are hidden. Returns the number
 * shown. */
guint skim_wf_layout_labels(SkimWfLabel *lab, guint n, double pitch, double hgt);

/* The shown label whose band [y − pitch/2, y + pitch/2) contains py, or −1. */
int skim_wf_label_at(const SkimWfLabel *lab, guint n, double pitch, double py);

#endif /* SKIM_WF_COMPOSE_H */
