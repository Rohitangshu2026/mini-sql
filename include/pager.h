#ifndef PAGER_H
#define PAGER_H

#include<stdint.h>

/*
 * A database is an array of fixed-size pages. PAGE_SIZE matches a typical OS
 * page so one of ours maps to one of the kernel's. It lives here because the
 * pager is what reads and writes pages (table.h includes this header).
 */
extern const uint32_t PAGE_SIZE;

/*
 * Owns the database file and an in-memory cache of its pages. A page is loaded
 * lazily on first access and only written back on close. `num_pages` is the
 * number of whole pages the database currently spans (grown as new pages are
 * touched); every node the b-tree stores occupies exactly one page.
 *
 * The cache is a growable array of page pointers, one slot per page number,
 * with `capacity` never less than `num_pages`. Growing it moves only the array
 * of pointers: each page is its own allocation and never moves, so a pointer
 * returned by pager_get_page stays valid until the pager is closed. The
 * b-tree relies on that when it holds a node while fetching others.
 */
typedef struct{
    int file_descriptor;    /* open fd for the database file */
    uint32_t file_length;   /* file size in bytes at open time */
    uint32_t num_pages;     /* pages the database spans */
    uint32_t capacity;      /* slots in `pages`; never less than num_pages */
    void** pages;           /* page cache; NULL == not resident */
}Pager;

/*
 * Opens (creating if absent) the database file and initializes the cache to
 * empty. Exits if the file's length is not a whole number of pages, since that
 * means the file is corrupt.
 */
Pager* pager_open(const char* filename);

/*
 * Returns a pointer to page `page_num`, reading it from disk on a cache miss.
 * The page must already exist or be the very next one (page_num == num_pages),
 * which a new node's page always is; fetching that next page adds it to the
 * database, zero-filled. Anything further out can only come from a bug or a
 * corrupt page pointer, so it's fatal.
 */
void* pager_get_page(Pager* pager, uint32_t page_num);

/*
 * Returns the page number a newly created node should occupy: the first page
 * past the current end of the database. Nothing is reserved — num_pages only
 * advances once that page is fetched with pager_get_page — so a caller must
 * fetch the page it was given before asking for another, or it will be handed
 * the same number twice.
 */
uint32_t get_unused_page_num(Pager* pager);

/* Writes one whole page back to its offset in the file. Exits on I/O error. */
void pager_flush(Pager* pager, uint32_t page_num);

/* Closes the file and frees every cached page, the cache and the pager itself. */
void pager_close(Pager* pager);

#endif
