#include "json.h"

#include <string.h>

#include "cJSON.h"

/* Backed by cJSON (vendored for native tests at test/native/vendor/cjson/,
 * pulled as the espressif/cjson managed component for the firmware build —
 * see src/idf_component.yml). This file only adapts cJSON to the fixed,
 * bounded-buffer API in json.h that the rest of the firmware (dispatcher.c
 * etc.) depends on: no heap left dangling past any call here, and no
 * caller-visible allocation ever handed back to them.
 *
 * Note on integers: cJSON stores numbers as `double`. Values above 2^53 lose
 * exact precision; amount_msat values in this protocol are nowhere near that
 * range in practice (LNURLcash bearer notes cap out at a tiny fraction of
 * total BTC supply expressed in msat). */

/* ================= reader public API =================================== */

bool json_has(const char *json, const char *key) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    bool found = cJSON_GetObjectItemCaseSensitive(root, key) != NULL;
    cJSON_Delete(root);
    return found;
}

bool json_get_str(const char *json, const char *key, char *out, size_t outcap) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    bool ok = false;
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        size_t len = strlen(item->valuestring);
        if (len + 1 <= outcap) {
            memcpy(out, item->valuestring, len + 1);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool json_get_u64(const char *json, const char *key, uint64_t *out) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    bool ok = false;
    if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
        *out = (uint64_t)item->valuedouble;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

bool json_get_bool(const char *json, const char *key, bool *out) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    bool ok = false;
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

bool json_get_str_array(const char *json, const char *key, char *out_flat, size_t item_stride,
                         size_t max_items, size_t *count) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    bool ok = false;
    if (cJSON_IsArray(item)) {
        ok = true;
        size_t n = 0;
        cJSON *elem = NULL;
        cJSON_ArrayForEach(elem, item) {
            if (!cJSON_IsString(elem) || elem->valuestring == NULL) {
                ok = false;
                break;
            }
            if (n < max_items) {
                size_t len = strlen(elem->valuestring);
                if (len + 1 > item_stride) {
                    ok = false;
                    break;
                }
                memcpy(out_flat + n * item_stride, elem->valuestring, len + 1);
            }
            n++;
        }
        if (ok) {
            *count = n < max_items ? n : max_items;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* ================= writer =============================================== */

/* json_writer_t builds a cJSON tree as jw_* calls come in (mirroring the
 * caller's begin/end nesting via w->stack), then serializes it into the
 * caller-provided buf/cap the moment the outermost container closes — every
 * call site in dispatcher.c relies on `out` already holding the finished
 * response as soon as the matching top-level jw_end_obj()/jw_end_arr()
 * returns, with no separate "finalize" call. */

void jw_init(json_writer_t *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->ok = cap > 0;
    w->root = NULL;
    w->depth = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static cJSON *jw_current(json_writer_t *w) {
    return w->depth > 0 ? (cJSON *)w->stack[w->depth - 1] : NULL;
}

static void jw_push(json_writer_t *w, cJSON *container) {
    if (w->depth < JW_MAX_DEPTH) {
        w->stack[w->depth++] = container;
    } else {
        w->ok = false;
    }
}

/* Attaches `container` (a freshly created object/array) either as the tree
 * root, a keyed member of the current object, or an unkeyed element of the
 * current array — then pushes it as the new current container. */
static void jw_attach(json_writer_t *w, cJSON *container, const char *key) {
    if (!container) {
        w->ok = false;
        return;
    }
    if (!w->ok) {
        cJSON_Delete(container);
        return;
    }
    if (w->root == NULL) {
        w->root = container;
    } else {
        cJSON *parent = jw_current(w);
        cJSON_bool added =
            key ? cJSON_AddItemToObject(parent, key, container) : cJSON_AddItemToArray(parent, container);
        if (!parent || !added) {
            w->ok = false;
            cJSON_Delete(container);
            return;
        }
    }
    jw_push(w, container);
}

/* Serializes the finished tree into buf/cap and frees the tree — called the
 * moment jw_end_obj/jw_end_arr closes the outermost container. */
static void jw_finalize(json_writer_t *w) {
    cJSON *root = (cJSON *)w->root;
    w->root = NULL;
    if (!w->ok || !root) {
        cJSON_Delete(root);
        w->ok = false;
        return;
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        w->ok = false;
        return;
    }
    size_t len = strlen(printed);
    if (len + 1 > w->cap) {
        w->ok = false;
    } else {
        memcpy(w->buf, printed, len + 1);
    }
    cJSON_free(printed);
}

void jw_begin_obj(json_writer_t *w, const char *key) {
    if (!w->ok) {
        return;
    }
    jw_attach(w, cJSON_CreateObject(), key);
}

void jw_end_obj(json_writer_t *w) {
    if (w->depth > 0) {
        w->depth--;
    }
    if (w->depth == 0) {
        jw_finalize(w);
    }
}

void jw_begin_arr(json_writer_t *w, const char *key) {
    if (!w->ok) {
        return;
    }
    jw_attach(w, cJSON_CreateArray(), key);
}

void jw_end_arr(json_writer_t *w) {
    if (w->depth > 0) {
        w->depth--;
    }
    if (w->depth == 0) {
        jw_finalize(w);
    }
}

void jw_str(json_writer_t *w, const char *key, const char *val) {
    if (!w->ok) {
        return;
    }
    cJSON *parent = jw_current(w);
    if (!parent || !cJSON_AddStringToObject(parent, key, val)) {
        w->ok = false;
    }
}

void jw_str_item(json_writer_t *w, const char *val) {
    if (!w->ok) {
        return;
    }
    cJSON *parent = jw_current(w);
    cJSON *item = cJSON_CreateString(val);
    if (!parent || !item || !cJSON_AddItemToArray(parent, item)) {
        w->ok = false;
        cJSON_Delete(item);
    }
}

void jw_uint64(json_writer_t *w, const char *key, uint64_t val) {
    if (!w->ok) {
        return;
    }
    cJSON *parent = jw_current(w);
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!parent || !item || !cJSON_AddItemToObject(parent, key, item)) {
        w->ok = false;
        cJSON_Delete(item);
    }
}

void jw_bool(json_writer_t *w, const char *key, bool val) {
    if (!w->ok) {
        return;
    }
    cJSON *parent = jw_current(w);
    if (!parent || !cJSON_AddBoolToObject(parent, key, val)) {
        w->ok = false;
    }
}

const char *jw_cstr(json_writer_t *w) {
    return w->buf;
}

bool jw_ok(json_writer_t *w) {
    return w->ok;
}
