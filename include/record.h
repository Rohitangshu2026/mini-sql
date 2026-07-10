#ifndef RECORD_H
#define RECORD_H

#include "schema.h"

#include<stdint.h>

/*
 * One row's worth of bytes. A Record is deliberately opaque: it is just a
 * `payload` blob plus its length. All meaning lives in the Schema, so the same
 * Record code serves any table. The payload is laid out exactly as the row is
 * stored on disk, which is why serialization is a single memcpy.
 */
typedef struct{
    void* payload;          /* row_size bytes, owned (or NULL when empty) */
    uint32_t payload_size;  /* == schema->row_size when initialized */
}Record;

/* Allocates a zeroed payload sized for `schema`. */
void record_init(Record* record, const Schema* schema);

/*
 * Frees the payload and resets the Record to empty. Safe on a zero-initialized
 * Record (NULL payload), which is how the SELECT path leaves it.
 */
void record_free(Record* record);

/* Writes an integer into the given column's slot. No-op if the id is unknown. */
void record_set_int(Record* record, const Schema* schema, uint32_t column_id, int32_t value);

/* Copies text into the given column's fixed-width slot. No-op if id unknown. */
void record_set_text(Record* record, const Schema* schema, uint32_t column_id, const char* value);

/* Reads an integer column. Returns 0 if the id is unknown. */
int32_t record_get_int(const Record* record, const Schema* schema, uint32_t column_id);

/*
 * Returns a pointer to a text column's bytes inside the payload (not a copy,
 * and not guaranteed NUL-terminated). NULL if the id is unknown.
 */
const char* record_get_text(const Record* record, const Schema* schema, uint32_t column_id);

/* Copies the payload to `destination` (which must hold payload_size bytes). */
void serialize_record(const Record* record, void* destination);

/* Allocates a Record and fills it from `source` (row_size bytes read). */
void deserialize_record(const void* source, Record* record, const Schema* schema);

/* Prints the row as "(col1, col2, ...)" using each column's declared type. */
void print_record(const Record* record, const Schema* schema);

#endif
