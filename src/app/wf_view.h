/*
 * wf_view.h — the M8 waterfall widget (GTK4).
 *
 * Frequency vertical (highest at the top), time flowing to the right, a kHz
 * scale strip to the right of the picture, the tuned frequency as a green
 * marker, and a column for the callsigns (filled in by the station model —
 * see skim_wf_view_set_stations). Pixels come from wf_compose.c; this file
 * owns the texture, the scale, the marker and the mouse: the wheel pans the
 * frequency window, Ctrl+wheel zooms it, and the scale strip can be GRABBED
 * and dragged. A retune of the radio moves ONLY the green marker — the
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

/* Colour scheme: index into wf_compose's palette table (sdr-for-linux's
 * set); recolours the whole picture at once. */
void skim_wf_view_set_palette(SkimWfView *v, int idx);

/* Visible window: centre and span in Hz (clamped to the band). */
void   skim_wf_view_set_span(SkimWfView *v, double span_hz);
double skim_wf_view_span(const SkimWfView *v);

G_END_DECLS

#endif /* SKIM_WF_VIEW_H */
