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
 * The first byte of a page on the free list. Node pages start with their type,
 * 0 (internal) or 1 (leaf), so a free page can never be mistaken for a node: a
 * stray pointer into one reads as a corrupt node and is refused.
 */
#define FREE_PAGE_MARKER 0xFFu

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
    uint32_t free_head;     /* first page of the free list; 0 when it's empty */
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
 * Allocates a page for a new node and returns its number. The page comes off
 * the free list when there is one, and is otherwise appended past the end of
 * the file; either way it's resident and zero-filled by the time this returns,
 * so the caller can build on it straight away.
 */
uint32_t pager_allocate_page(Pager* pager);

/*
 * Puts a page no node uses any more onto the free list, for a later allocation
 * to reuse. The page is zeroed — nothing it held lingers in the file — then
 * marked free and linked to the previous head of the list.
 */
void pager_free_page(Pager* pager, uint32_t page_num);

/* Writes one whole page back to its offset in the file. Exits on I/O error. */
void pager_flush(Pager* pager, uint32_t page_num);

/* Closes the file and frees every cached page, the cache and the pager itself. */
void pager_close(Pager* pager);

#endif
