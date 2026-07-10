#ifndef PAGER_H
#define PAGER_H

#include<stdint.h>

/*
 * A database is an array of fixed-size pages. TABLE_MAX_PAGES caps how many we
 * cache in memory; PAGE_SIZE matches a typical OS page so one of ours maps to
 * one of the kernel's. The cache lives in the pager, so these constants live
 * here too (table.h includes this header).
 */
#define TABLE_MAX_PAGES 100
extern const uint32_t PAGE_SIZE;

/*
 * Owns the database file and an in-memory cache of its pages. A page is loaded
 * lazily on first access and only written back on close. `num_pages` is the
 * number of whole pages the database currently spans (grown as new pages are
 * touched); every node the b-tree stores occupies exactly one page.
 */
typedef struct{
    int file_descriptor;            /* open fd for the database file */
    uint32_t file_length;           /* file size in bytes at open time */
    uint32_t num_pages;             /* pages the database spans */
    void* pages[TABLE_MAX_PAGES];   /* page cache; NULL == not resident */
}Pager;

/*
 * Opens (creating if absent) the database file and initializes the cache to
 * empty. Exits if the file's length is not a whole number of pages, since that
 * means the file is corrupt.
 */
Pager* pager_open(const char* filename);

/*
 * Returns a pointer to page `page_num`, reading it from disk on a cache miss
 * and zero-extending past the end of the file. Growing into a new page bumps
 * num_pages. Exits if page_num is out of range.
 */
void* pager_get_page(Pager* pager, uint32_t page_num);

/* Writes one whole page back to its offset in the file. Exits on I/O error. */
void pager_flush(Pager* pager, uint32_t page_num);

/* Closes the file and frees every cached page and the pager itself. */
void pager_close(Pager* pager);

#endif
