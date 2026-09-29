#include "file_header.h"
#include "pager.h"

#include<stdio.h>
#include<string.h>

#define HEADER_MAGIC_SIZE          16u
#define HEADER_VERSION_OFFSET      16u
#define HEADER_PAGE_SIZE_OFFSET    20u
#define HEADER_CATALOG_ROOT_OFFSET 24u

/* Exactly 16 bytes: fifteen characters and the terminating NUL. */
static const char HEADER_MAGIC[HEADER_MAGIC_SIZE] = "mini-sql format";

/*
 * Reads a 4-byte field. Copied out with memcpy rather than read through a
 * uint32_t*, so the header's layout can't make a read misaligned.
 */
static uint32_t read_field(const void* page, uint32_t offset){
    uint32_t value;
    memcpy(&value, (const char*)page + offset, sizeof(value));
    return value;
}

/* Writes a 4-byte field. Mirror of read_field. */
static void write_field(void* page, uint32_t offset, uint32_t value){
    memcpy((char*)page + offset, &value, sizeof(value));
}

/*
 * Writes a fresh header. The page is cleared first so the bytes reserved for
 * future fields are zero on disk rather than whatever the page held before.
 */
void file_header_initialize(void* page, uint32_t catalog_root_page_num){
    memset(page, 0, PAGE_SIZE);
    memcpy(page, HEADER_MAGIC, HEADER_MAGIC_SIZE);
    write_field(page, HEADER_VERSION_OFFSET, FILE_FORMAT_VERSION);
    write_field(page, HEADER_PAGE_SIZE_OFFSET, PAGE_SIZE);
    write_field(page, HEADER_CATALOG_ROOT_OFFSET, catalog_root_page_num);
}

/*
 * Checks the header field by field, from what the file is to what it
 * contains, so the message names the most basic problem:
 *
 *   - the magic: anything else isn't a mini-sql database at all. Every file
 *     written before headers existed has a node at byte 0 instead, so it's
 *     caught here too;
 *   - the format version: a mini-sql file this build doesn't know how to read;
 *   - the page size: node layouts are computed from it, so any other size
 *     would misplace every cell;
 *   - the catalog root page: it must be a page of the file, and not page 0,
 *     which is this header. A root anywhere else would send the first lookup
 *     into garbage or past the end of the file.
 */
bool file_header_validate(const void* page, uint32_t num_pages, const char* filename,
                          char* message, size_t message_size){
    if(memcmp(page, HEADER_MAGIC, HEADER_MAGIC_SIZE) != 0){
        snprintf(message, message_size,
                 "Error: %s is not a mini-sql database, or was written by an older build.", filename);
        return false;
    }

    uint32_t version = read_field(page, HEADER_VERSION_OFFSET);
    if(version != FILE_FORMAT_VERSION){
        snprintf(message, message_size,
                 "Error: %s uses file format %u; this build reads format %u.",
                 filename, version, FILE_FORMAT_VERSION);
        return false;
    }

    uint32_t page_size = read_field(page, HEADER_PAGE_SIZE_OFFSET);
    if(page_size != PAGE_SIZE){
        snprintf(message, message_size,
                 "Error: %s uses %u-byte pages; this build uses %u.", filename, page_size, PAGE_SIZE);
        return false;
    }

    uint32_t catalog_root_page_num = read_field(page, HEADER_CATALOG_ROOT_OFFSET);
    if(catalog_root_page_num == 0 || catalog_root_page_num >= num_pages){
        snprintf(message, message_size,
                 "Error: %s is corrupt: its catalog root page %u is not a node page of the file.",
                 filename, catalog_root_page_num);
        return false;
    }

    return true;
}

/* The catalog's root page, as the header records it. */
uint32_t file_header_catalog_root_page(const void* page){
    return read_field(page, HEADER_CATALOG_ROOT_OFFSET);
}
