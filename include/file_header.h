#ifndef FILE_HEADER_H
#define FILE_HEADER_H

#include<stdbool.h>
#include<stddef.h>
#include<stdint.h>

/*
 * Page 0 of every database file is its header, and never a b-tree node. The
 * header says what the file is and how to read it: a magic string, the file
 * format version, the page size, and the root page of the catalog — the table
 * of tables, from which every other table is found.
 *
 *   bytes 0-15   magic "mini-sql format\0" (after SQLite's "SQLite format 3\0")
 *   bytes 16-19  file format version
 *   bytes 20-23  page size
 *   bytes 24-27  root page of the catalog
 *   bytes 28-31  first page of the free list, 0 when it's empty
 *   the rest     zero, reserved for fields later formats add
 *
 * Fields are stored in the machine's byte order, like every node field. Any
 * change to what a file holds bumps FILE_FORMAT_VERSION, so a file written by
 * another version is refused with a message instead of being misread.
 */
#define FILE_FORMAT_VERSION 3u

/*
 * Turns `page` into a fresh header recording `catalog_root_page_num` as the
 * catalog's root and an empty free list. The whole page is rewritten, so the
 * reserved bytes end up zero.
 */
void file_header_initialize(void* page, uint32_t catalog_root_page_num);

/*
 * Checks that `page` is the header of a file this build can read, and that its
 * catalog root page — and its free-list head, unless the list is empty — is
 * one of the file's `num_pages` pages other than the header itself. On failure writes a ready-to-print message naming `filename` into
 * `message` and returns false. Never modifies the page.
 */
bool file_header_validate(const void* page, uint32_t num_pages, const char* filename,
                          char* message, size_t message_size);

/* The catalog's root page, as recorded in the header. */
uint32_t file_header_catalog_root_page(const void* page);

/* The first page of the free list, as recorded in the header; 0 if it's empty. */
uint32_t file_header_free_head(const void* page);

/* Records the first page of the free list in the header. */
void file_header_set_free_head(void* page, uint32_t free_head);

#endif
