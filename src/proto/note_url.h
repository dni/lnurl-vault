#ifndef LNURLVAULT_NOTE_URL_H
#define LNURLVAULT_NOTE_URL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Builds the lnurlw:// bearer-note URL for a note, per LUD-25
 * (../../luds/25.md): appending `?k1=<secret>&amount=<msat>` to the
 * withdraw endpoint's base URL. `host` is the same string a note was
 * `confirm`ed or `import_secret`ed with (see docs/PROTOCOL.md) — despite
 * the field's name it's expected to be the full base URL including path
 * (e.g. "mint.example/w"), not just a bare hostname; this function embeds
 * it verbatim and does no validation of it.
 *
 * Returns false if host or k1_hex is empty, or the result wouldn't fit in
 * outcap. On failure out's contents are unspecified (may be partially
 * written) — check the return value before using it. */
bool note_url_build(const char *host, const char *k1_hex, uint64_t amount_msat, char *out,
                     size_t outcap);

#endif
