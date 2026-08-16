#ifndef LNURLVAULT_JSON_H
#define LNURLVAULT_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal, no-heap JSON reader/writer tailored to the fixed, flat command
 * schema in docs/PROTOCOL.md. Not a general-purpose JSON library: the reader
 * only looks at the *top-level* fields of the outermost object (nested
 * objects/arrays are skipped over wholesale, not descended into for key
 * matching) and the writer only supports the shapes our responses actually
 * need (objects, arrays of strings or objects, string/uint64/bool fields).
 * No ESP-IDF dependency, so it compiles identically into the firmware and
 * the native test binary. */

/* ---- Reader --------------------------------------------------------- */

/* True if `key` is present as a top-level field of the outermost object. */
bool json_has(const char *json, const char *key);

/* Extracts a top-level string field, unescaped, NUL-terminated into out.
 * Fails (returns false, out untouched) if the key is absent, not a string,
 * malformed, or doesn't fit in outcap. */
bool json_get_str(const char *json, const char *key, char *out, size_t outcap);

/* Extracts a top-level non-negative integer field (e.g. amount_msat). */
bool json_get_u64(const char *json, const char *key, uint64_t *out);

bool json_get_bool(const char *json, const char *key, bool *out);

/* Extracts a top-level array-of-strings field (e.g. "parent_ids":["a","b"])
 * into a flat buffer: out_flat + i*item_stride holds the i-th NUL-terminated
 * string. Items beyond max_items are ignored (not an error); *count is set
 * to the number actually written. Fails if the field is present but not an
 * array of strings, or any item doesn't fit in item_stride. */
bool json_get_str_array(const char *json, const char *key, char *out_flat,
                         size_t item_stride, size_t max_items, size_t *count);

/* ---- Writer ----------------------------------------------------------- */

#define JW_MAX_DEPTH 8

/* Builds a cJSON tree internally (stack holds opaque cJSON* container
 * pointers — kept as void* so this header doesn't need cJSON.h on its
 * include path) and serializes it into buf/cap the moment the outermost
 * jw_begin_obj/jw_begin_arr closes. See json.c for details. */
typedef struct {
    char *buf;
    size_t cap;
    bool ok; /* false once malformed, over-nested, or the buffer would overflow */
    void *root;
    void *stack[JW_MAX_DEPTH];
    int depth;
} json_writer_t;

void jw_init(json_writer_t *w, char *buf, size_t cap);

/* key may be NULL for a top-level object or an object nested directly in an
 * array (i.e. no key, just a value). */
void jw_begin_obj(json_writer_t *w, const char *key);
void jw_end_obj(json_writer_t *w);
void jw_begin_arr(json_writer_t *w, const char *key);
void jw_end_arr(json_writer_t *w);

void jw_str(json_writer_t *w, const char *key, const char *val);
void jw_str_item(json_writer_t *w, const char *val); /* array element, no key */
void jw_uint64(json_writer_t *w, const char *key, uint64_t val);
void jw_bool(json_writer_t *w, const char *key, bool val);

/* NUL-terminated buffer built so far. Only meaningful if jw_ok() is true. */
const char *jw_cstr(json_writer_t *w);
bool jw_ok(json_writer_t *w);

#endif
