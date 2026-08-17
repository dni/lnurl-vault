#ifndef LNURLVAULT_BASE64_H
#define LNURLVAULT_BASE64_H

#include <stddef.h>
#include <stdbool.h>

/* Standard (RFC 4648 section 4) base64 with '+'/'/' and '=' padding —
 * only consumer today is ota_chunk's `data` field (see dispatcher.c):
 * raw binary firmware bytes don't fit directly in a JSON string, and this
 * keeps OTA on the same newline-delimited-JSON wire format as every other
 * command instead of introducing a second, binary-framed sub-protocol. */

/* Output buffer must be at least base64_encoded_len(in_len) + 1 (NUL). */
size_t base64_encoded_len(size_t in_len);
void base64_encode(const unsigned char *in, size_t in_len, char *out);

/* Output buffer must be at least base64_decoded_len(in_len) bytes — an
 * upper bound; the actual decoded length (accounting for padding) is
 * written to *out_len. Returns false on malformed input (bad character,
 * wrong length, misplaced padding) without writing partial output. */
size_t base64_decoded_len(size_t in_len);
bool base64_decode(const char *in, size_t in_len, unsigned char *out, size_t *out_len);

#endif
