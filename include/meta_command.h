#ifndef META_COMMAND_H
#define META_COMMAND_H

#include "input_buffer.h"
#include "table.h"

/* Outcome of handling a "." command. */
typedef enum{
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
}MetaCommandResult;

/*
 * Handles a "."-prefixed command (.exit, .btree, .constants). ".exit" tears the
 * connection down and terminates the process; the others print diagnostics and
 * return control to the REPL. Returns UNRECOGNIZED for anything else.
 */
MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table);

#endif
