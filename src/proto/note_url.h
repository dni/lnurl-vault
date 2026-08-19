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

/* What the on-screen QR encodes when a note is unveiled for someone to take.
 *
 * NOTE_URL_LUD17 is the lnurlw:// form above. It is the smallest code and the
 * one LNURL-native wallets want, and on a stock phone nothing opens it -- the
 * codes render and decode, and no handler exists (issue #26). That is the
 * whole problem: a bearer note you cannot hand to someone with a phone is not
 * a bearer note.
 *
 * NOTE_URL_CLAIM is a normal https:// link into a wallet, which any camera
 * opens. It costs one QR version (~138 chars against ~113, v7 against v6) and
 * that is the trade: a code a stranger can actually use.
 *
 * The secret rides in the FRAGMENT, never the query. A fragment is not sent
 * in the request line, so it stays out of server logs, referrers and proxies
 * -- the wallet is a hash-router SPA, so this is also just its normal route
 * shape. Putting k1 in a query string would hand every note to the first
 * access log it touched. */
typedef enum {
    NOTE_URL_LUD17,
    NOTE_URL_CLAIM,
} note_url_format_t;

/* Base of the claim link, up to and including the fragment marker. Override
 * at build time to point at a different wallet. */
#ifndef LNURLVAULT_CLAIM_BASE
#define LNURLVAULT_CLAIM_BASE "https://wallet.lnurlcash.com/#/claim"
#endif

/* Which format the unveil screen uses. Claim by default: see above. */
#ifndef LNURLVAULT_QR_FORMAT
#define LNURLVAULT_QR_FORMAT NOTE_URL_CLAIM
#endif

/* `claim_base` may be NULL for LNURLVAULT_CLAIM_BASE. Same failure contract
 * as note_url_build: false, and out emptied, on anything that does not fit. */
bool note_url_build_as(note_url_format_t format, const char *claim_base, const char *host,
                        const char *k1_hex, uint64_t amount_msat, char *out, size_t outcap);

#endif
