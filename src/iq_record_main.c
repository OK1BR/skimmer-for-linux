/*
 * skimmer-iq-record — record a TCI IQ session to disk beside live decoding
 * (gh#28: the bench that replays recorded air against RBN).
 *
 *   skimmer-iq-record [-s server] [-r rate] [-t seconds] <out.cf32>
 *     -s server   unix:/path, ws://host[:port] or host[:port]; default = the
 *                 default order (sdr-for-linux's socket, then 127.0.0.1:40001)
 *     -r rate     48/96/192/384[000]; default 0 = do NOT send iq_samplerate,
 *                 take the rate the server serves (the running skimmer set
 *                 it) — recording beside the app changes nothing on the radio
 *     -t seconds  stop after this long; default: until SIGINT/SIGTERM
 *
 * A second TCI client beside the app: sdr-for-linux keeps the IQ
 * subscription and the rate per client (iq_start/iq_stop/iq_samplerate,
 * tci_server.c), so this process neither stops nor resamples the stream
 * the app is decoding. It sends iq_start, iq_stamp and — on exit — iq_stop,
 * nothing else; never a vfo, never a spot.
 *
 * Writes, beside <out.cf32> (raw IQ, float32 interleaved I/Q, TRUE
 * orientation as on the wire — what skimmer-replay reads):
 *   <out>.meta    text sidecar, written at the FIRST IQ block (the start
 *                 time RBN spots are aligned to) and rewritten at the end
 *                 with the frame count: start_utc, start_unix_us, rate_hz,
 *                 center_hz (the first block's), source, device …
 *   <out>.events  one line per event, at the frame offset it applies from:
 *                   <frame> centre <hz>   the DDC centre from this frame on
 *                                         (the first line is frame 0; a
 *                                         retune inside a block arrives as
 *                                         its own split — tci_client.c)
 *                   <frame> gap <frames>  frames the server never sent
 *                                         before this one (packet counter)
 *                   <frame> tx 1|0        the radio keyed / unkeyed
 *                   <frame> closed        the server dropped the link
 *                 skimmer-replay applies them all, so a replay sees the
 *                 retunes, holes and TX holds the live pipeline saw.
 * An existing <out.cf32> is never overwritten.
 *
 * 192 kHz cf32 is 1.5 MB/s — 5.5 GB an hour.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "engine/tci_client.h"

/* ---- recording state (callbacks run on the client's LWS thread) ----------- */

static GMutex   r_lock;
static FILE    *r_iq;
static FILE    *r_ev;
static char    *r_meta_path;
static char    *r_link;
static char    *r_device;
static guint64  r_frames;          /* frames written so far = next frame index */
static double   r_rate;
static double   r_centre0;
static double   r_centre;          /* last centre written to .events           */
static gint64   r_start_us;        /* wall clock at the first block (µs)       */
static guint64  r_gaps, r_gap_frames, r_tx_edges;
static gboolean r_io_error;
static gboolean r_closed;
static GMainLoop *r_loop;

static void meta_write(gboolean final) {
  FILE *m = fopen(r_meta_path, "w");
  if (!m) {
    g_printerr("cannot write %s: %s\n", r_meta_path, g_strerror(errno));
    return;
  }
  GDateTime *t = g_date_time_new_from_unix_utc((gint64)(r_start_us / G_USEC_PER_SEC));
  GDateTime *tl = g_date_time_to_local(t);
  char *utc = g_date_time_format(t, "%Y-%m-%dT%H:%M:%S");
  char *loc = g_date_time_format(tl, "%Y-%m-%d %H:%M:%S %z");
  fprintf(m,
          "start_utc: %s.%06" G_GINT64_FORMAT "Z\n"
          "start_unix_us: %" G_GINT64_FORMAT "\n"
          "start: %s\n"
          "source: %s\n"
          "device: %s\n"
          "format: cf32 interleaved I,Q — TRUE spectrum orientation "
          "(as on the wire, no conjugation)\n"
          "center_hz: %.0f\n"
          "rate_hz: %.0f\n"
          "events: %s\n",
          utc, r_start_us % G_USEC_PER_SEC, r_start_us, loc,
          r_link ? r_link : "?", r_device ? r_device : "?",
          r_centre0, r_rate, final ? "complete" : "recording");
  if (final) {
    fprintf(m,
            "frames: %" G_GUINT64_FORMAT "\n"
            "duration_s: %.1f\n"
            "gaps: %" G_GUINT64_FORMAT " (%" G_GUINT64_FORMAT " frames)\n"
            "tx_edges: %" G_GUINT64_FORMAT "\n",
            r_frames, r_rate > 0 ? (double)r_frames / r_rate : 0.0,
            r_gaps, r_gap_frames, r_tx_edges);
  }
  fclose(m);
  g_free(utc);
  g_free(loc);
  g_date_time_unref(tl);
  g_date_time_unref(t);
}

static void iq_cb(const float *iq, guint nframes, double rate, double center,
                  guint gap_frames, gpointer user) {
  (void)user;
  g_mutex_lock(&r_lock);
  if (!r_iq || r_io_error) { g_mutex_unlock(&r_lock); return; }
  if (!r_start_us) {
    r_start_us = g_get_real_time();
    r_rate = rate;
    r_centre0 = r_centre = center;
    fprintf(r_ev, "0 centre %.0f\n", center);
    meta_write(FALSE);
  } else {
    if (gap_frames) {
      fprintf(r_ev, "%" G_GUINT64_FORMAT " gap %u\n", r_frames, gap_frames);
      r_gaps++;
      r_gap_frames += gap_frames;
    }
    if (center != r_centre) {
      fprintf(r_ev, "%" G_GUINT64_FORMAT " centre %.0f\n", r_frames, center);
      r_centre = center;
    }
    if (rate != r_rate) {
      /* A rate change mid-file would make every later frame index wrong;
       * nothing replays that — end the recording here. */
      fprintf(r_ev, "%" G_GUINT64_FORMAT " closed\n", r_frames);
      g_printerr("IQ rate changed %.0f → %.0f Hz — recording stopped\n",
                 r_rate, rate);
      r_io_error = TRUE;
      g_mutex_unlock(&r_lock);
      if (r_loop) { g_main_loop_quit(r_loop); }
      return;
    }
  }
  if (fwrite(iq, 2 * sizeof(float), nframes, r_iq) != nframes) {
    g_printerr("write failed: %s — recording stopped\n", g_strerror(errno));
    r_io_error = TRUE;
    g_mutex_unlock(&r_lock);
    if (r_loop) { g_main_loop_quit(r_loop); }
    return;
  }
  r_frames += nframes;
  g_mutex_unlock(&r_lock);
}

static void tx_cb(gboolean tx, gpointer user) {
  (void)user;
  g_mutex_lock(&r_lock);
  if (r_ev && r_start_us) {
    fprintf(r_ev, "%" G_GUINT64_FORMAT " tx %d\n", r_frames, tx ? 1 : 0);
    r_tx_edges++;
  }
  g_mutex_unlock(&r_lock);
}

static void closed_cb(gpointer user) {
  (void)user;
  g_mutex_lock(&r_lock);
  if (r_ev) { fprintf(r_ev, "%" G_GUINT64_FORMAT " closed\n", r_frames); }
  r_closed = TRUE;
  g_mutex_unlock(&r_lock);
  if (r_loop) { g_main_loop_quit(r_loop); }
}

static gboolean on_signal(gpointer user) {
  (void)user;
  g_main_loop_quit(r_loop);
  return G_SOURCE_REMOVE;
}

static gboolean on_deadline(gpointer user) {
  (void)user;
  g_main_loop_quit(r_loop);
  return G_SOURCE_REMOVE;
}

/* Once a minute: flush, and one status line (the file grows; a stalled
 * stream shows as a flat frame count). */
static gboolean on_tick(gpointer user) {
  (void)user;
  g_mutex_lock(&r_lock);
  if (r_iq) { fflush(r_iq); }
  if (r_ev) { fflush(r_ev); }
  const guint64 fr = r_frames;
  const double rate = r_rate;
  const double centre = r_centre;
  const guint64 gaps = r_gaps, txe = r_tx_edges;
  g_mutex_unlock(&r_lock);
  GDateTime *now = g_date_time_new_now_local();
  char *ts = g_date_time_format(now, "%H:%M:%S");
  g_print("%s  %.1f min recorded, %.2f GB, centre %.3f kHz, %" G_GUINT64_FORMAT
          " gaps, %" G_GUINT64_FORMAT " tx edges\n",
          ts, rate > 0 ? (double)fr / rate / 60.0 : 0.0,
          (double)fr * 8.0 / 1e9, centre / 1000.0, gaps, txe);
  g_free(ts);
  g_date_time_unref(now);
  return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
  char *server = NULL;
  int rate = 0;
  int secs = 0;
  GOptionEntry entries[] = {
    { "server", 's', 0, G_OPTION_ARG_STRING, &server,
      "TCI server (unix:/path, ws://host[:port], host[:port]); default order", "S" },
    { "rate", 'r', 0, G_OPTION_ARG_INT, &rate,
      "IQ rate 48/96/192/384[000]; default: keep the served rate", "HZ" },
    { "time", 't', 0, G_OPTION_ARG_INT, &secs,
      "stop after this many seconds; default: until SIGINT/SIGTERM", "S" },
    G_OPTION_ENTRY_NULL
  };
  GOptionContext *oc = g_option_context_new("<out.cf32> — record TCI IQ for skimmer-replay");
  g_option_context_add_main_entries(oc, entries, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(oc, &argc, &argv, &err) || argc != 2) {
    g_printerr("%s\n", err ? err->message : "usage: skimmer-iq-record [-s S] [-r HZ] [-t S] <out.cf32>");
    g_clear_error(&err);
    g_option_context_free(oc);
    return 2;
  }
  g_option_context_free(oc);
  if (rate > 0 && rate < 1000) { rate *= 1000; }   /* 192 → 192000 */
  const char *out = argv[1];

  /* O_EXCL: a recording is never overwritten by the next one. */
  int fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    g_printerr("cannot create %s: %s (an existing recording is never overwritten)\n",
               out, g_strerror(errno));
    return 1;
  }
  r_iq = fdopen(fd, "wb");
  char *ev_path = g_strdup_printf("%s.events", out);
  r_ev = fopen(ev_path, "w");
  r_meta_path = g_strdup_printf("%s.meta", out);
  if (!r_iq || !r_ev) {
    g_printerr("cannot open the output files: %s\n", g_strerror(errno));
    return 1;
  }

  SkimTciClient *c = skim_tci_client_new(server);
  skim_tci_client_set_iq_cb(c, iq_cb, NULL);
  skim_tci_client_set_tx_cb(c, tx_cb, NULL);
  skim_tci_client_set_closed_cb(c, closed_cb, NULL);
  r_loop = g_main_loop_new(NULL, FALSE);
  if (!skim_tci_client_start(c, (guint)rate, &err)) {
    g_printerr("FAIL — %s\n", err ? err->message : "?");
    g_clear_error(&err);
    skim_tci_client_free(c);
    fclose(r_iq);
    fclose(r_ev);
    unlink(out);           /* nothing recorded: no empty file left behind */
    unlink(ev_path);
    return 1;
  }
  g_mutex_lock(&r_lock);
  r_link = g_strdup(skim_tci_client_endpoint(c));
  r_device = g_strdup(skim_tci_client_device(c));
  g_mutex_unlock(&r_lock);
  g_print("recording %s from %s (device \"%s\")%s — Ctrl+C / SIGTERM stops\n",
          out, r_link, r_device,
          rate ? "" : ", rate as served (no iq_samplerate sent)");

  g_unix_signal_add(SIGINT, on_signal, NULL);
  g_unix_signal_add(SIGTERM, on_signal, NULL);
  if (secs > 0) { g_timeout_add_seconds((guint)secs, on_deadline, NULL); }
  g_timeout_add_seconds(60, on_tick, NULL);
  g_main_loop_run(r_loop);

  const guint64 lost = skim_tci_client_lost_blocks(c);
  const int layout = skim_tci_client_stamp_layout(c);
  skim_tci_client_stop(c);          /* iq_stop for THIS client only */

  g_mutex_lock(&r_lock);
  fclose(r_iq);
  r_iq = NULL;
  fclose(r_ev);
  r_ev = NULL;
  if (r_start_us) { meta_write(TRUE); }
  g_mutex_unlock(&r_lock);

  g_print("stopped: %" G_GUINT64_FORMAT " frames (%.1f min) at %.0f Hz, header layout %d, "
          "%" G_GUINT64_FORMAT " gaps / %" G_GUINT64_FORMAT " lost blocks, "
          "%" G_GUINT64_FORMAT " tx edges%s%s\n",
          r_frames, r_rate > 0 ? (double)r_frames / r_rate / 60.0 : 0.0, r_rate,
          layout, r_gaps, lost, r_tx_edges,
          r_closed ? ", server closed the link" : "",
          r_io_error ? ", STOPPED ON ERROR" : "");
  if (!r_start_us) { g_print("no IQ block arrived — nothing recorded\n"); }
  skim_tci_client_free(c);
  g_main_loop_unref(r_loop);
  g_free(ev_path);
  g_free(server);
  return r_io_error || !r_start_us ? 1 : 0;
}
