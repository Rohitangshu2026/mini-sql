#ifndef INPUT_BUFFER_H
#define INPUT_BUFFER_H

#include<stddef.h>       /* size_t  */
#include<sys/types.h>    /* ssize_t */

/*
 * A growable line buffer wrapped around getline(). It owns its `buffer`, which
 * getline() reallocates as needed, so the same InputBuffer can be reused for
 * every line of the REPL without freeing in between.
 */
typedef struct{
    char* buffer;           /* the line text, NUL-terminated (owned) */
    size_t buffer_length;   /* current capacity getline() has allocated */
    ssize_t input_length;   /* length of the line, excluding the newline */
}InputBuffer;

/* Allocates an empty InputBuffer with no backing storage yet. */
InputBuffer* new_input_buffer(void);

/*
 * Reads one line from stdin into the buffer, stripping the trailing newline.
 * Exits the process on EOF or read error, so callers never see a failure.
 */
void read_input(InputBuffer* input_buffer);

/* Frees the line storage and the InputBuffer itself. */
void close_input_buffer(InputBuffer* input_buffer);

#endif
