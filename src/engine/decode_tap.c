/* decode_tap.c — decode tap line format (decode_tap.h).
 *
 *   B <frames> <now_us> <rate> <nchan> <chan_bw>
 *   D <frames> <now_us> <chan> <slot> <sig_hz> <conf> <speed> <snr> <c> <text>
 *   R <frames> <now_us> <chan> <slot>
 *   F <frames> <now_us>
 *   P <frames> <now_us>
 *
 * Fields are tab-separated. Doubles are written with %.17g through
 * g_ascii_formatd and read with g_ascii_strtod — exact round trip, no
 * locale. <c> is 0/1 (contested). <text> is g_strescape()d, so it holds
 * no tab or newline and reads back with g_strcompress().
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_tap.h"

#include <stdlib.h>
#include <string.h>

static void put_d(GString *s, double v) {
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(buf, sizeof(buf), "%.17g", v);
  g_string_append_c(s, '\t');
  g_string_append(s, buf);
}

char *skim_tap_event_to_line(const SkimTapEvent *ev) {
  static const char KIND[] = { 'B', 'D', 'R', 'F', 'P' };
  g_return_val_if_fail(ev && (guint)ev->kind < G_N_ELEMENTS(KIND), NULL);
  GString *s = g_string_sized_new(96);
  g_string_append_printf(s, "%c\t%" G_GUINT64_FORMAT "\t%" G_GINT64_FORMAT,
                         KIND[ev->kind], ev->frames, ev->now_us);
  switch (ev->kind) {
    case SKIM_TAP_BANK:
      put_d(s, ev->rate);
      g_string_append_printf(s, "\t%u", ev->nchan);
      put_d(s, ev->chan_bw_hz);
      break;
    case SKIM_TAP_DECODE: {
      g_string_append_printf(s, "\t%u\t%u", ev->chan, ev->slot);
      put_d(s, ev->sig_hz);
      put_d(s, ev->d.confidence);
      put_d(s, ev->d.speed);
      put_d(s, ev->d.snr_db);
      char *esc = g_strescape(ev->d.text, NULL);
      g_string_append_printf(s, "\t%d\t%s", ev->contested ? 1 : 0, esc);
      g_free(esc);
      break;
    }
    case SKIM_TAP_RESET:
      g_string_append_printf(s, "\t%u\t%u", ev->chan, ev->slot);
      break;
    case SKIM_TAP_FLUSH:
    case SKIM_TAP_PRUNE:
      break;
  }
  return g_string_free(s, FALSE);
}

gboolean skim_tap_line_parse(const char *line, SkimTapEvent *ev) {
  if (!line || !ev)
    return FALSE;
  while (*line == ' ')
    line++;
  if (!*line || *line == '#' || *line == '\n')
    return FALSE;
  SkimTapKind kind;
  guint need;                          /* fields after the kind letter      */
  switch (line[0]) {
    case 'B': kind = SKIM_TAP_BANK;   need = 5;  break;
    case 'D': kind = SKIM_TAP_DECODE; need = 10; break;
    case 'R': kind = SKIM_TAP_RESET;  need = 4;  break;
    case 'F': kind = SKIM_TAP_FLUSH;  need = 2;  break;
    case 'P': kind = SKIM_TAP_PRUNE;  need = 2;  break;
    default:  return FALSE;
  }
  if (line[1] != '\t')
    return FALSE;
  /* Split on tabs — the text (last field of a D line) carries none. */
  char **f = g_strsplit(line + 2, "\t", -1);
  guint n = g_strv_length(f);
  /* A trailing newline rides on the last field. */
  if (n) {
    char *last = f[n - 1];
    size_t L = strlen(last);
    while (L && (last[L - 1] == '\n' || last[L - 1] == '\r')) { last[--L] = '\0'; }
  }
  if (n < need) {
    g_strfreev(f);
    return FALSE;
  }
  SkimTapEvent e;
  memset(&e, 0, sizeof(e));
  e.kind   = kind;
  e.frames = g_ascii_strtoull(f[0], NULL, 10);
  e.now_us = g_ascii_strtoll(f[1], NULL, 10);
  switch (kind) {
    case SKIM_TAP_BANK:
      e.rate       = g_ascii_strtod(f[2], NULL);
      e.nchan      = (guint)strtoul(f[3], NULL, 10);
      e.chan_bw_hz = g_ascii_strtod(f[4], NULL);
      break;
    case SKIM_TAP_DECODE: {
      e.chan         = (guint)strtoul(f[2], NULL, 10);
      e.slot         = (guint)strtoul(f[3], NULL, 10);
      e.sig_hz       = g_ascii_strtod(f[4], NULL);
      e.d.confidence = g_ascii_strtod(f[5], NULL);
      e.d.speed      = g_ascii_strtod(f[6], NULL);
      e.d.snr_db     = g_ascii_strtod(f[7], NULL);
      e.contested    = strtol(f[8], NULL, 10) != 0;
      char *text = g_strcompress(f[9]);
      g_strlcpy(e.d.text, text, sizeof(e.d.text));
      g_free(text);
      break;
    }
    case SKIM_TAP_RESET:
      e.chan = (guint)strtoul(f[2], NULL, 10);
      e.slot = (guint)strtoul(f[3], NULL, 10);
      break;
    case SKIM_TAP_FLUSH:
    case SKIM_TAP_PRUNE:
      break;
  }
  g_strfreev(f);
  *ev = e;
  return TRUE;
}
