#ifndef LNURLVAULT_BECH32_H
#define LNURLVAULT_BECH32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bech32 encoding (BIP-173), for turning a withdraw LNURL into the
 * `LNURL1...` string that every LNURL wallet has understood for years.
 *
 * LUD-25 names exactly two ways a bearer note can be handed over: "prefixed
 * with the lnurlw:// scheme (LUD-17) or bech32-encoded as an ordinary LNURL".
 * This is the second one, and it is the one an ordinary wallet's scanner
 * accepts -- which is the whole backward-compatibility promise the spec opens
 * with, that a wallet knowing nothing about LNURLcash still sees a normal
 * withdraw link.
 *
 * ENCODE ONLY. Nothing on this device ever needs to read one: a vault emits
 * notes, it does not scan them.
 *
 * Two deliberate departures from BIP-173, both of them LNURL convention
 * (LUD-01) rather than liberties:
 *
 *   - No 90-character limit. BIP-173 caps a bech32 string at 90 because its
 *     checksum's error-detection guarantees are only proven that far. LNURL
 *     ignores that cap and always has -- a withdraw URL with a 64-character
 *     k1 blows past it before anything else is added. Enforcing it here would
 *     mean refusing to encode every note this device holds.
 *
 *   - Uppercase output. A bech32 string must be all one case, and uppercase
 *     is what LNURL uses, because a QR encoder can then pack it in
 *     alphanumeric mode at 5.5 bits per character instead of byte mode's 8.
 *     Decoders lowercase before checking, so this costs nothing and buys most
 *     of a QR version back.
 *
 * No heap, no large stack: the 5-bit groups are streamed rather than
 * buffered, so encoding a 256-byte URL costs a few dozen bytes of frame
 * rather than the 410-byte intermediate the obvious implementation wants.
 * That matters here -- this runs on ui_task, whose whole stack is 4096. */

/* Longest bech32 string bech32_encoded_len() will report a length for, and
 * the ceiling every caller should size its buffer against. */
#define BECH32_MAX_OUT 512

/* How many characters bech32_encode_upper() would write for `data_len` bytes
 * under `hrp`, NOT counting the NUL. 0 if it would exceed BECH32_MAX_OUT.
 * Exposed so a caller can pick a buffer, and so a test can assert the
 * arithmetic without encoding anything. */
size_t bech32_encoded_len(const char *hrp, size_t data_len);

/* Encodes `data` as an UPPERCASE bech32 string with the given human-readable
 * part, NUL-terminated.
 *
 * `hrp` must be non-empty and printable ASCII in the range BIP-173 allows
 * (33..126); it is emitted uppercased alongside the rest.
 *
 * Returns false -- having written an empty string, never a partial one -- if
 * anything does not fit or any argument is bad. A truncated bech32 string is
 * a plausible-looking LNURL carrying a shortened secret, which is a different
 * note or none, and the one failure mode that must never reach a QR code
 * somebody is handed as money. Same contract as note_url.c, for the same
 * reason. */
bool bech32_encode_upper(const char *hrp, const uint8_t *data, size_t data_len, char *out,
                          size_t outcap);

#endif
