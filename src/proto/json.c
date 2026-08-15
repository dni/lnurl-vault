#include "json.h"

#include <stdio.h>
#include <string.h>

#include "hex.h"

/* ================= shared scanning helpers ============================ */

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
        (*p)++;
    }
}

/* *p must point at the opening '"'. Returns a pointer just past the matching
 * (unescaped) closing '"'. Does not validate escape sequences, just skips
 * past them so bracket/quote matching stays correct. */
static const char *skip_json_string(const char *p) {
    p++; /* opening quote */
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p += 2;
            continue;
        }
        p++;
    }
    if (*p == '"') {
        p++;
    }
    return p;
}

/* *p must point at `open`. Returns a pointer just past the matching `close`,
 * skipping over any nested strings/brackets in between. */
static const char *skip_json_bracketed(const char *p, char open, char close) {
    int depth = 0;
    for (;;) {
        if (*p == '"') {
            p = skip_json_string(p);
            continue;
        }
        if (*p == '\0') {
            return p;
        }
        if (*p == open) {
            depth++;
            p++;
        } else if (*p == close) {
            depth--;
            p++;
            if (depth == 0) {
                return p;
            }
        } else {
            p++;
        }
    }
}

/* Finds `key` as a field of the outermost JSON object and returns the raw
 * (still-encoded) span of its value — e.g. for a string value the span
 * includes the surrounding quotes. Nested objects/arrays belonging to other
 * fields are skipped wholesale, never descended into. */
static bool json_find_raw(const char *json, const char *key, const char **val_start,
                           const char **val_end) {
    const char *p = json;
    while (*p && *p != '{') {
        p++;
    }
    if (*p != '{') {
        return false;
    }
    p++;

    size_t keylen = strlen(key);

    for (;;) {
        skip_ws(&p);
        if (*p == '}' || *p == '\0') {
            return false;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            return false; /* malformed input */
        }

        const char *kstart = p + 1;
        const char *after_key = skip_json_string(p);
        const char *kend = after_key - 1; /* at the closing quote */
        p = after_key;

        skip_ws(&p);
        if (*p != ':') {
            return false;
        }
        p++;
        skip_ws(&p);

        const char *vstart = p;
        const char *vend;
        if (*p == '"') {
            vend = skip_json_string(p);
        } else if (*p == '{') {
            vend = skip_json_bracketed(p, '{', '}');
        } else if (*p == '[') {
            vend = skip_json_bracketed(p, '[', ']');
        } else {
            while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' &&
                   *p != '\r') {
                p++;
            }
            vend = p;
        }

        size_t this_keylen = (size_t)(kend - kstart);
        if (this_keylen == keylen && strncmp(kstart, key, keylen) == 0) {
            *val_start = vstart;
            *val_end = vend;
            return true;
        }

        p = vend;
    }
}

static bool json_unescape_str(const char *src, size_t srclen, char *dst, size_t dstcap) {
    size_t di = 0;
    size_t i = 0;
    while (i < srclen) {
        char c = src[i];
        if (c != '\\') {
            if (di + 1 >= dstcap) {
                return false;
            }
            dst[di++] = c;
            i++;
            continue;
        }

        if (i + 1 >= srclen) {
            return false;
        }
        char e = src[i + 1];
        if (e == 'u') {
            if (i + 6 > srclen) {
                return false;
            }
            uint8_t codeunit[2];
            if (!hex_decode(src + i + 2, 4, codeunit, sizeof(codeunit))) {
                return false;
            }
            unsigned cp = ((unsigned)codeunit[0] << 8) | codeunit[1];
            i += 6;
            if (cp < 0x80) {
                if (di + 1 >= dstcap) {
                    return false;
                }
                dst[di++] = (char)cp;
            } else if (cp < 0x800) {
                if (di + 2 >= dstcap) {
                    return false;
                }
                dst[di++] = (char)(0xC0 | (cp >> 6));
                dst[di++] = (char)(0x80 | (cp & 0x3F));
            } else {
                if (di + 3 >= dstcap) {
                    return false;
                }
                dst[di++] = (char)(0xE0 | (cp >> 12));
                dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[di++] = (char)(0x80 | (cp & 0x3F));
            }
            continue;
        }

        char out_c;
        switch (e) {
            case '"':
                out_c = '"';
                break;
            case '\\':
                out_c = '\\';
                break;
            case '/':
                out_c = '/';
                break;
            case 'b':
                out_c = '\b';
                break;
            case 'f':
                out_c = '\f';
                break;
            case 'n':
                out_c = '\n';
                break;
            case 'r':
                out_c = '\r';
                break;
            case 't':
                out_c = '\t';
                break;
            default:
                return false;
        }
        if (di + 1 >= dstcap) {
            return false;
        }
        dst[di++] = out_c;
        i += 2;
    }
    dst[di] = '\0';
    return true;
}

/* ================= reader public API =================================== */

bool json_has(const char *json, const char *key) {
    const char *vs, *ve;
    return json_find_raw(json, key, &vs, &ve);
}

bool json_get_str(const char *json, const char *key, char *out, size_t outcap) {
    const char *vs, *ve;
    if (!json_find_raw(json, key, &vs, &ve)) {
        return false;
    }
    if (ve - vs < 2 || *vs != '"' || *(ve - 1) != '"') {
        return false;
    }
    return json_unescape_str(vs + 1, (size_t)((ve - 1) - (vs + 1)), out, outcap);
}

bool json_get_u64(const char *json, const char *key, uint64_t *out) {
    const char *vs, *ve;
    if (!json_find_raw(json, key, &vs, &ve)) {
        return false;
    }
    if (vs >= ve) {
        return false;
    }
    uint64_t val = 0;
    for (const char *p = vs; p < ve; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        val = val * 10 + (uint64_t)(*p - '0');
    }
    *out = val;
    return true;
}

bool json_get_bool(const char *json, const char *key, bool *out) {
    const char *vs, *ve;
    if (!json_find_raw(json, key, &vs, &ve)) {
        return false;
    }
    size_t len = (size_t)(ve - vs);
    if (len == 4 && strncmp(vs, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (len == 5 && strncmp(vs, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool json_get_str_array(const char *json, const char *key, char *out_flat, size_t item_stride,
                         size_t max_items, size_t *count) {
    const char *vs, *ve;
    if (!json_find_raw(json, key, &vs, &ve)) {
        return false;
    }
    if (vs >= ve || *vs != '[' || *(ve - 1) != ']') {
        return false;
    }

    const char *p = vs + 1;
    const char *end = ve - 1;
    *count = 0;

    skip_ws(&p);
    while (p < end) {
        if (*p == ',') {
            p++;
            skip_ws(&p);
            continue;
        }
        if (*p != '"') {
            return false; /* only string arrays are supported */
        }
        const char *item_end = skip_json_string(p); /* past closing quote */
        size_t content_len = (size_t)((item_end - 1) - (p + 1));
        if (*count < max_items) {
            if (!json_unescape_str(p + 1, content_len, out_flat + (*count) * item_stride,
                                    item_stride)) {
                return false;
            }
            (*count)++;
        }
        p = item_end;
        skip_ws(&p);
    }
    return true;
}

/* ================= writer =============================================== */

void jw_init(json_writer_t *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = cap > 0;
    w->depth = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void jw_raw(json_writer_t *w, const char *s, size_t n) {
    if (!w->ok) {
        return;
    }
    if (w->len + n + 1 > w->cap) {
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_raw_cstr(json_writer_t *w, const char *s) {
    jw_raw(w, s, strlen(s));
}

static void jw_pre_value(json_writer_t *w) {
    if (w->depth > 0) {
        if (w->need_comma[w->depth - 1]) {
            jw_raw(w, ",", 1);
        }
        w->need_comma[w->depth - 1] = true;
    }
}

static void jw_escaped_str(json_writer_t *w, const char *s) {
    jw_raw(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':
                jw_raw(w, "\\\"", 2);
                break;
            case '\\':
                jw_raw(w, "\\\\", 2);
                break;
            case '\n':
                jw_raw(w, "\\n", 2);
                break;
            case '\r':
                jw_raw(w, "\\r", 2);
                break;
            case '\t':
                jw_raw(w, "\\t", 2);
                break;
            default:
                if (*p < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", *p);
                    jw_raw_cstr(w, buf);
                } else {
                    char c = (char)*p;
                    jw_raw(w, &c, 1);
                }
        }
    }
    jw_raw(w, "\"", 1);
}

static void jw_push(json_writer_t *w) {
    if (w->depth < JW_MAX_DEPTH) {
        w->need_comma[w->depth] = false;
        w->depth++;
    } else {
        w->ok = false;
    }
}

static void jw_pop(json_writer_t *w) {
    if (w->depth > 0) {
        w->depth--;
    }
}

void jw_begin_obj(json_writer_t *w, const char *key) {
    jw_pre_value(w);
    if (key) {
        jw_escaped_str(w, key);
        jw_raw(w, ":", 1);
    }
    jw_raw(w, "{", 1);
    jw_push(w);
}

void jw_end_obj(json_writer_t *w) {
    jw_pop(w);
    jw_raw(w, "}", 1);
}

void jw_begin_arr(json_writer_t *w, const char *key) {
    jw_pre_value(w);
    if (key) {
        jw_escaped_str(w, key);
        jw_raw(w, ":", 1);
    }
    jw_raw(w, "[", 1);
    jw_push(w);
}

void jw_end_arr(json_writer_t *w) {
    jw_pop(w);
    jw_raw(w, "]", 1);
}

void jw_str(json_writer_t *w, const char *key, const char *val) {
    jw_pre_value(w);
    jw_escaped_str(w, key);
    jw_raw(w, ":", 1);
    jw_escaped_str(w, val);
}

void jw_str_item(json_writer_t *w, const char *val) {
    jw_pre_value(w);
    jw_escaped_str(w, val);
}

void jw_uint64(json_writer_t *w, const char *key, uint64_t val) {
    jw_pre_value(w);
    jw_escaped_str(w, key);
    jw_raw(w, ":", 1);
    char buf[21];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    jw_raw_cstr(w, buf);
}

void jw_bool(json_writer_t *w, const char *key, bool val) {
    jw_pre_value(w);
    jw_escaped_str(w, key);
    jw_raw(w, ":", 1);
    jw_raw_cstr(w, val ? "true" : "false");
}

const char *jw_cstr(json_writer_t *w) {
    return w->buf;
}

bool jw_ok(json_writer_t *w) {
    return w->ok;
}
