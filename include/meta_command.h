#ifndef META_COMMAND_H
#define META_COMMAND_H

#include "database.h"
#include "input_buffer.h"

/* Outcome of handling a "." command. */
typedef enum{
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
}MetaCommandResult;

/*
 * Handles a "."-prefixed command (.exit, .tables, .stats, .schema, .btree,
 * .constants).
 * ".exit" closes the database and terminates the process; the others print
 * what they describe, or a usage line or error, and return control to the
 * REPL. Returns UNRECOGNIZED for anything else.
 */
MetaCommandResult do_meta_command(InputBuffer* input_buffer, Database* db);

#endif
