#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    const uint8_t *start;
    const uint8_t *next;
    const uint8_t *end;             /* enclosing request boundary */
    uint8_t major;
    uint64_t value;
} fido2_cbor_item_t;

bool fido2_cbor_read(const uint8_t **cursor, const uint8_t *end, fido2_cbor_item_t *item);
bool fido2_cbor_skip(const uint8_t **cursor, const uint8_t *end);
bool fido2_cbor_map_find(const fido2_cbor_item_t *map, int key, fido2_cbor_item_t *out);
bool fido2_cbor_bytes(const fido2_cbor_item_t *item, const uint8_t **data, size_t *len);
bool fido2_cbor_text(const fido2_cbor_item_t *item, const char **data, size_t *len);
bool fido2_cbor_bool(const fido2_cbor_item_t *item, bool *value);
bool fido2_cbor_int(const fido2_cbor_item_t *item, int64_t *value);
bool fido2_cbor_array_at(const fido2_cbor_item_t *array, size_t wanted, fido2_cbor_item_t *out);

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool overflow;
} fido2_cbor_writer_t;

void fido2_cbor_writer_init(fido2_cbor_writer_t *w, uint8_t *data, size_t cap);
void fido2_cbor_put_head(fido2_cbor_writer_t *w, uint8_t major, uint64_t value);
void fido2_cbor_put_uint(fido2_cbor_writer_t *w, uint64_t value);
void fido2_cbor_put_int(fido2_cbor_writer_t *w, int64_t value);
void fido2_cbor_put_bool(fido2_cbor_writer_t *w, bool value);
void fido2_cbor_put_map(fido2_cbor_writer_t *w, size_t count);
void fido2_cbor_put_array(fido2_cbor_writer_t *w, size_t count);
void fido2_cbor_put_bytes(fido2_cbor_writer_t *w, const uint8_t *data, size_t len);
void fido2_cbor_put_text(fido2_cbor_writer_t *w, const char *text);
void fido2_cbor_put_raw(fido2_cbor_writer_t *w, const uint8_t *data, size_t len);
