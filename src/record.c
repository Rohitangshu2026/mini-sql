#include "record.h"

#include<stdlib.h>
#include<string.h>
#include<stdio.h>

/*
 * Allocates a zeroed payload big enough for one row of `schema`. Zeroing means
 * unset text fields read back as empty strings and unset ints as 0.
 */
void record_init(Record* record, const Schema* schema){
    record->payload_size = schema->row_size;
    record->payload = calloc(1, schema->row_size);
}

/*
 * Frees the payload and resets the Record to empty. The NULL guard makes this
 * safe to call on a zero-initialized Record, which is how the SELECT path
 * leaves record_to_insert.
 */
void record_free(Record* record){
    if(record == NULL || record->payload == NULL)
        return;
    free(record->payload);
    record->payload = NULL;
    record->payload_size = 0;
}

/*
 * Copies a 4-byte integer into the column's slot. Uses memcpy rather than a
 * cast-and-store because the slot may not be aligned for int32_t. Silently
 * ignores an unknown column id.
 */
void record_set_int(Record* record, const Schema* schema, uint32_t column_id, int32_t value){
    const ColumnDefinition* column = schema_find_column_by_id(schema, column_id);
    if(column == NULL)
        return;
    memcpy((char*)record->payload + column->offset, &value, sizeof(int32_t));
}

/*
 * Copies text into the column's fixed-width slot. strncpy zero-pads a short
 * value to fill the field and truncates one that is too long (the parser has
 * already rejected over-long values). Silently ignores an unknown column id.
 */
void record_set_text(Record* record, const Schema* schema, uint32_t column_id, const char* value){
    const ColumnDefinition* column = schema_find_column_by_id(schema, column_id);
    if(column == NULL)
        return;
    strncpy((char*)record->payload + column->offset, value, column->size);
}

/*
 * Reads a 4-byte integer out of the column's slot (via memcpy, for the same
 * alignment reason as the setter). Returns 0 for an unknown column id.
 */
int32_t record_get_int(const Record* record, const Schema* schema, uint32_t column_id){
    const ColumnDefinition* column = schema_find_column_by_id(schema, column_id);
    int32_t value = 0;
    if(column == NULL)
        return value;
    memcpy(&value, (const char*)record->payload + column->offset, sizeof(int32_t));
    return value;
}

/*
 * Returns a pointer to the text column's bytes inside the payload. It is not a
 * copy and, if the value fills the whole field, not NUL-terminated — callers
 * must respect column->size (print_record uses a width-limited "%.*s"). NULL
 * for an unknown column id.
 */
const char* record_get_text(const Record* record, const Schema* schema, uint32_t column_id){
    const ColumnDefinition* column = schema_find_column_by_id(schema, column_id);
    if(column == NULL)
        return NULL;
    return (const char*)record->payload + column->offset;
}

/*
 * Writes the row to `destination`. Because the payload is already in on-disk
 * layout, this is a single memcpy; `destination` must have room for
 * payload_size bytes.
 */
void serialize_record(const Record* record, void* destination){
    memcpy(destination, record->payload, record->payload_size);
}

/*
 * Builds a Record from raw bytes: allocates a payload for `schema` and copies
 * row_size bytes in. The caller owns the result and must record_free it.
 */
void deserialize_record(const void* source, Record* record, const Schema* schema){
    record_init(record, schema);
    memcpy(record->payload, source, schema->row_size);
}

/*
 * Prints the row as "(v1, v2, ...)", formatting each field by its column type.
 * Text is printed with a width limit so an unterminated full-width field can't
 * over-read past its slot.
 */
void print_record(const Record* record, const Schema* schema){
    printf("(");
    for(uint32_t i = 0; i < schema->num_columns; ++i){
        const ColumnDefinition* column = &schema->columns[i];
        if(i > 0)
            printf(", ");
        if(column->type == COLUMN_INT)
            printf("%d", record_get_int(record, schema, column->column_id));
        else if(column->type == COLUMN_TEXT)
            printf("%.*s", (int)column->size, record_get_text(record, schema, column->column_id));
    }
    printf(")\n");
}
