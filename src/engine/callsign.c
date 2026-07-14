/* callsign.c — callsign extraction + validation. M0: skeleton (rejects all).
 * Real regex + dictionary + plausibility scoring land in M4 (docs/SCOPE.md).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "callsign.h"

gboolean skim_callsign_is_valid(const char *s) {
  (void)s;
  /* M4: prefix/suffix regex + known-call dictionary. */
  return FALSE;
}

double skim_callsign_extract(const char *text, char *out, gsize out_size) {
  (void)text;
  if (out && out_size > 0)
    out[0] = '\0';
  /* M4: scan tokens, score, return the best candidate. */
  return 0.0;
}
