/*
 * wf_view.h — the M8 waterfall widget (GTK4).
 *
 * Frequency vertical (highest at the top), time flowing to the right, a kHz
 * scale strip to the right of the picture, the tuned frequency as a green
 * marker, and the callsign column (CW Skimmer's: a rail with a dot on every
 * station's frequency, the callsign beside it, crowded labels fanned apart
 * with a connector back to the dot, the station list's columns in the
 * label's tooltip — a click on a label tunes the radio).
 * Pixels come from wf_compose.c; this file owns the texture, the scale, the
 * marker, the column and the mouse: the wheel pans the frequency window,
 * Ctrl+wheel zooms it, and the scale strip can be GRABBED and dragged. A retune of the radio moves ONLY the green marker — the
 * window never jumps under the operator's eyes (Richard, 2026-09-05); when
 * the marker leaves the window a small arrow on the scale says which way.
 */
#ifndef SKIM_WF_VIEW_H
#define SKIM_WF_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SKIM_TYPE_WF_VIEW (skim_wf_view_get_type())
G_DECLARE_FINAL_TYPE(SkimWfView, skim_wf_view, SKIM, WF_VIEW, GtkWidget)

GtkWidget *skim_wf_view_new(void);

/* One spectrum row from the engine (spectrum.h encoding). Does NOT queue a
 * redraw — the caller draws once per event batch. */
void skim_wf_view_push(SkimWfView *v, const guint8 *row, guint nbins,
                       double center_hz, double bin_hz);

/* The radio's tuned frequency (marker + follow). */
void skim_wf_view_set_vfo(SkimWfView *v, double hz);

/* The callsign column: a snapshot of the station table, one entry per
 * tracked station (CQ and S&P alike — the column IS the station list in
 * another shape: kHz, speed, dB, heard and age sit in the label's tooltip;
 * a dB after the call was tried and taken out on Richard's live look). The
 * widget copies the array; call again on every change. Colours follow the logbook verdict exactly as the pane and the
 * panadapter spots do (gray = DUP/B4/INV); the pane's fixed station is drawn
 * bold. */
typedef struct {
  char     call[16];
  double   hz;
  gboolean cq;         /* heard CALLING → "CQ " prefix                       */
  gboolean gray;       /* the logbook says do not call (skim_dup_verdict_gray)*/
  gboolean tuned;      /* the decode pane's fixed station                    */
  double   snr_db;     /* tooltip; seat priority when over capacity          */
  char     mode[8];    /* "CW" / "RTTY" — picks WPM or Bd in the tooltip     */
  double   speed;      /* WPM or baud                                        */
  guint    reports;    /* decode events folded into the record ("heard")    */
  gint64   last_heard; /* station table clock — the tooltip's age           */
} SkimWfStation;

void skim_wf_view_set_stations(SkimWfView *v, const SkimWfStation *st, guint n);

/* A click on a callsign label — the panadapter-spot gesture. Fires with the
 * station's tracked frequency (its exact carrier, not the pixel's Hz). */
typedef void (*SkimWfViewClickCb)(const char *call, double hz, gpointer user);
void skim_wf_view_set_click_cb(SkimWfView *v, SkimWfViewClickCb cb, gpointer user);

/* Colour scheme: index into wf_compose's palette table (sdr-for-linux's
 * set); recolours the whole picture at once. */
void skim_wf_view_set_palette(SkimWfView *v, int idx);

/* Visible window: centre and span in Hz (clamped to the band). */
void   skim_wf_view_set_span(SkimWfView *v, double span_hz);
double skim_wf_view_span(const SkimWfView *v);

G_END_DECLS

#endif /* SKIM_WF_VIEW_H */
