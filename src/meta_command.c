#include "meta_command.h"
#include "btree.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>

/* Most words any meta-command takes: the command and one table name. */
#define MAX_META_WORDS 2u

/*
 * Splits `line` in place at spaces and tabs into at most MAX_META_WORDS words.
 * Returns how many words there were, counting past the limit, so a command
 * given too many arguments can be told apart from one given the right number.
 */
static uint32_t split_words(char* line, char* words[]){
    uint32_t count = 0;
    for(char* word = strtok(line, " \t"); word != NULL; word = strtok(NULL, " \t")){
        if(count < MAX_META_WORDS)
            words[count] = word;
        count++;
    }
    return count;
}

/* The table named by a command's argument, or NULL after printing an error. */
static Table* find_table_or_report(Database* db, const char* name){
    Table* table = database_find_table(db, name);
    if(table == NULL)
        printf("Error: no such table: %s.\n", name);
    return table;
}

/* .schema [TABLE] — the canonical CREATE TABLE of one table, or of all of them. */
static void show_schema(Database* db, uint32_t num_words, char* words[]){
    if(num_words > 2){
        printf("Usage: .schema [TABLE]\n");
        return;
    }
    if(num_words == 2){
        Table* table = find_table_or_report(db, words[1]);
        if(table != NULL)
            printf("%s;\n", table->sql);
        return;
    }
    for(uint32_t i = 0; i < db->num_tables; ++i)
        printf("%s;\n", db->tables[i]->sql);
}

/* .btree TABLE — the table's whole tree, one node per line. */
static void show_btree(Database* db, uint32_t num_words, char* words[]){
    if(num_words != 2){
        printf("Usage: .btree TABLE\n");
        return;
    }
    Table* table = find_table_or_report(db, words[1]);
    if(table == NULL)
        return;
    printf("Tree:\n");
    print_tree(table->pager, table->schema, table->root_page_num, 0);
}

/* .constants [TABLE] — the layout sizes, including the table's own if one is named. */
static void show_constants(Database* db, uint32_t num_words, char* words[]){
    if(num_words > 2){
        printf("Usage: .constants [TABLE]\n");
        return;
    }
    const Schema* schema = NULL;
    if(num_words == 2){
        Table* table = find_table_or_report(db, words[1]);
        if(table == NULL)
            return;
        schema = table->schema;
    }
    printf("Constants:\n");
    print_constants(schema);
}

/*
 * Handles the "." commands.
 *
 *   .exit              flush and close the database, then terminate the process.
 *   .tables            list the tables, in the order they were created.
 *   .stats             how many rows the last SELECT read, matching or not.
 *   .schema [TABLE]    print the CREATE TABLE text of one table or all.
 *   .btree TABLE       print a table's tree, one node per line, indented by depth.
 *   .constants [TABLE] print the on-page layout sizes, a table's included.
 *
 * The line is split into words on a copy, so an unrecognized command can
 * still be echoed back whole. The informational commands return
 * META_COMMAND_SUCCESS so the REPL resumes; ".exit" never returns.
 */
MetaCommandResult do_meta_command(InputBuffer* input_buffer, Database* db){
    size_t length = strlen(input_buffer->buffer);
    char* line = malloc(length + 1);
    memcpy(line, input_buffer->buffer, length + 1);

    char* words[MAX_META_WORDS];
    uint32_t num_words = split_words(line, words);
    MetaCommandResult result = META_COMMAND_SUCCESS;

    if(num_words == 1 && strcmp(words[0], ".exit") == 0){
        free(line);
        close_input_buffer(input_buffer);
        db_close(db);   /* flush pages, close file, free every table and the database */
        exit(EXIT_SUCCESS);
    }
    else if(num_words == 1 && strcmp(words[0], ".tables") == 0){
        for(uint32_t i = 0; i < db->num_tables; ++i)
            printf("%s\n", db->tables[i]->name);
    }
    else if(num_words == 1 && strcmp(words[0], ".stats") == 0)
        printf("Rows examined: %llu\n", (unsigned long long)db->last_rows_examined);
    else if(num_words >= 1 && strcmp(words[0], ".schema") == 0)
        show_schema(db, num_words, words);
    else if(num_words >= 1 && strcmp(words[0], ".btree") == 0)
        show_btree(db, num_words, words);
    else if(num_words >= 1 && strcmp(words[0], ".constants") == 0)
        show_constants(db, num_words, words);
    else
        result = META_COMMAND_UNRECOGNIZED_COMMAND;

    free(line);
    return result;
}
