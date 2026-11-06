#include "fido2_cbor.h"
#include <string.h>

bool fido2_cbor_read(const uint8_t **cursor, const uint8_t *end, fido2_cbor_item_t *item)
{
    if (!cursor || !*cursor || *cursor >= end || !item) return false;
    item->start = *cursor;
    item->end = end;
    uint8_t initial = *(*cursor)++;
    item->major = initial >> 5;
    uint8_t ai = initial & 31;
    item->value = 0;
    if (ai < 24) item->value = ai;
    else {
        int bytes = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : 0;
        if (!bytes || (size_t)(end - *cursor) < (size_t)bytes) return false;
        for (int i = 0; i < bytes; i++) item->value = (item->value << 8) | *(*cursor)++;
    }
    item->next = *cursor;
    return true;
}

bool fido2_cbor_skip(const uint8_t **cursor, const uint8_t *end)
{
    fido2_cbor_item_t item;
    if (!fido2_cbor_read(cursor, end, &item)) return false;
    if (item.major == 2 || item.major == 3) {
        if (item.value > (uint64_t)(end - *cursor)) return false;
        *cursor += item.value;
    } else if (item.major == 4 || item.major == 5) {
        uint64_t count = item.value * (item.major == 5 ? 2 : 1);
        if (item.value > 1024) return false;
        for (uint64_t i = 0; i < count; i++) if (!fido2_cbor_skip(cursor, end)) return false;
    } else if (item.major == 6) {
        return fido2_cbor_skip(cursor, end);
    }
    return true;
}

static bool item_key(const fido2_cbor_item_t *item, int *key)
{
    if (!item || item->value > 0x7fffffff) return false;
    if (item->major == 0) *key = (int)item->value;
    else if (item->major == 1) *key = -1 - (int)item->value;
    else return false;
    return true;
}

bool fido2_cbor_map_find(const fido2_cbor_item_t *map, int key, fido2_cbor_item_t *out)
{
    if (!map || map->major != 5 || map->value > 256) return false;
    const uint8_t *cursor = map->next, *end = map->end;
    const uint8_t *scan = map->next;
    for (uint64_t i = 0; i < map->value * 2; i++) {
        if (!fido2_cbor_skip(&scan, end)) return false;
    }
    for (uint64_t i = 0; i < map->value; i++) {
        fido2_cbor_item_t k;
        if (!fido2_cbor_read(&cursor, end, &k)) return false;
        int found_key;
        if (!item_key(&k, &found_key)) return false;
        const uint8_t *value_start = cursor;
        if (!fido2_cbor_skip(&cursor, end)) return false;
        if (found_key == key && out) {
            const uint8_t *tmp = value_start;
            return fido2_cbor_read(&tmp, end, out);
        }
    }
    return false;
}

bool fido2_cbor_bytes(const fido2_cbor_item_t *item, const uint8_t **data, size_t *len)
{
    if (!item || item->major != 2 || item->value > SIZE_MAX) return false;
    *data = item->next;
    *len = (size_t)item->value;
    return true;
}
bool fido2_cbor_text(const fido2_cbor_item_t *item, const char **data, size_t *len)
{
    if (!item || item->major != 3 || item->value > SIZE_MAX) return false;
    *data = (const char *)item->next;
    *len = (size_t)item->value;
    return true;
}
bool fido2_cbor_bool(const fido2_cbor_item_t *item, bool *value)
{
    if (!item || item->major != 7 || (item->value != 20 && item->value != 21)) return false;
    *value = item->value == 21;
    return true;
}
bool fido2_cbor_int(const fido2_cbor_item_t *item, int64_t *value)
{
    if (!item || item->value > INT64_MAX || (item->major != 0 && item->major != 1)) return false;
    *value = item->major == 0 ? (int64_t)item->value : -1 - (int64_t)item->value;
    return true;
}
bool fido2_cbor_array_at(const fido2_cbor_item_t *array, size_t wanted, fido2_cbor_item_t *out)
{
    if (!array || array->major != 4 || wanted >= array->value) return false;
    const uint8_t *cursor = array->next;
    for (size_t i = 0; i <= wanted; i++) {
        const uint8_t *start = cursor;
        if (!fido2_cbor_skip(&cursor, array->end)) return false;
        if (i == wanted && out) {
            const uint8_t *tmp = start;
            return fido2_cbor_read(&tmp, cursor, out);
        }
    }
    return false;
}

static void put(fido2_cbor_writer_t *w, uint8_t value)
{
    if (w->len >= w->cap) { w->overflow = true; return; }
    w->data[w->len++] = value;
}
static void raw(fido2_cbor_writer_t *w, const uint8_t *data, size_t len)
{
    if (len > w->cap - w->len) { w->overflow = true; return; }
    memcpy(w->data + w->len, data, len); w->len += len;
}
void fido2_cbor_writer_init(fido2_cbor_writer_t *w, uint8_t *data, size_t cap)
{
    w->data = data; w->len = 0; w->cap = cap; w->overflow = false;
}
void fido2_cbor_put_head(fido2_cbor_writer_t *w, uint8_t major, uint64_t value)
{
    uint8_t ai = value < 24 ? 24 : value <= 0xff ? 24 : value <= 0xffff ? 25 : value <= 0xffffffffULL ? 26 : 27;
    if (value < 24) { put(w, (major << 5) | value); return; }
    put(w, (major << 5) | ai);
    int bytes = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : 8;
    for (int i = bytes - 1; i >= 0; i--) put(w, (value >> (i * 8)) & 0xff);
}
void fido2_cbor_put_uint(fido2_cbor_writer_t *w, uint64_t value) { fido2_cbor_put_head(w, 0, value); }
void fido2_cbor_put_int(fido2_cbor_writer_t *w, int64_t value)
{
    if (value >= 0) fido2_cbor_put_head(w, 0, value);
    else fido2_cbor_put_head(w, 1, (uint64_t)(-1 - value));
}
void fido2_cbor_put_bool(fido2_cbor_writer_t *w, bool value) { put(w, value ? 0xf5 : 0xf4); }
void fido2_cbor_put_map(fido2_cbor_writer_t *w, size_t count) { fido2_cbor_put_head(w, 5, count); }
void fido2_cbor_put_array(fido2_cbor_writer_t *w, size_t count) { fido2_cbor_put_head(w, 4, count); }
void fido2_cbor_put_bytes(fido2_cbor_writer_t *w, const uint8_t *data, size_t len)
{
    fido2_cbor_put_head(w, 2, len); raw(w, data, len);
}
void fido2_cbor_put_text(fido2_cbor_writer_t *w, const char *text)
{
    size_t len = text ? strlen(text) : 0;
    fido2_cbor_put_head(w, 3, len); raw(w, (const uint8_t *)text, len);
}
void fido2_cbor_put_raw(fido2_cbor_writer_t *w, const uint8_t *data, size_t len) { raw(w, data, len); }
