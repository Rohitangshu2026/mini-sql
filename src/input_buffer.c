#include "input_buffer.h"

#include<stdio.h>
#include<stdlib.h>

/*
 * Allocates an InputBuffer with no line storage yet. getline() in read_input()
 * does the first allocation, so buffer/buffer_length start as NULL/0.
 */
InputBuffer* new_input_buffer(void){
    InputBuffer* input_buffer = malloc(sizeof(InputBuffer));
    input_buffer->buffer = NULL;
    input_buffer->buffer_length = 0;
    input_buffer->input_length = 0;
    return input_buffer;
}

/*
 * Reads a line into the buffer (getline grows it as needed) and drops the
 * trailing newline by overwriting it with a NUL. Treats EOF or error as fatal,
 * so the REPL loop never has to check for a short read.
 */
void read_input(InputBuffer* input_buffer){
    ssize_t bytes_read = getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);
    if(bytes_read <= 0){
        printf("Error reading Input\n");
        exit(EXIT_FAILURE);
    }

    /* strip the trailing newline getline leaves in place */
    input_buffer->input_length = bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = 0;
}

/* Frees the getline-owned line storage, then the struct. */
void close_input_buffer(InputBuffer* input_buffer){
    free(input_buffer->buffer);
    free(input_buffer);
}
