#define _POSIX_C_SOURCE 200809L   /* open/read/write/lseek under strict -std=c11 */

#include "pager.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<errno.h>

const uint32_t PAGE_SIZE = 4096;

/* Slots the page cache starts with, before it has to grow. */
#define INITIAL_PAGE_CAPACITY 16u

/*
 * Grows the page cache to at least `needed` slots, doubling so that a table
 * growing one page at a time costs amortized O(1) per page. New slots start
 * NULL (not resident). Only the array of pointers is reallocated; the pages
 * they point to stay where they are.
 */
static void grow_page_cache(Pager* pager, uint32_t needed){
    uint32_t new_capacity = pager->capacity > UINT32_MAX / 2 ? UINT32_MAX : pager->capacity * 2;
    if(new_capacity < needed)
        new_capacity = needed;

    pager->pages = realloc(pager->pages, (size_t)new_capacity * sizeof(void*));
    if(pager->pages == NULL){
        printf("Out of memory growing the page cache to %u pages\n", new_capacity);
        exit(EXIT_FAILURE);
    }
    for(uint32_t i = pager->capacity; i < new_capacity; ++i)
        pager->pages[i] = NULL;
    pager->capacity = new_capacity;
}

/*
 * Opens the database file (creating it if missing, read/write, owner-only) and
 * sets up an empty page cache with room for every page the file already has.
 * num_pages is derived from the file size; a file whose length is not a whole
 * number of pages can only be corrupt, so we bail. Any I/O failure here is
 * fatal.
 */
Pager* pager_open(const char* filename){
    int fd = open(filename,
                  O_RDWR |      /* read/write */
                  O_CREAT,      /* create if missing */
                  S_IWUSR |     /* user write */
                  S_IRUSR);     /* user read  */
    if(fd == -1){
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    off_t file_length = lseek(fd, 0, SEEK_END);

    Pager* pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = (uint32_t)file_length;
    pager->num_pages = (uint32_t)file_length / PAGE_SIZE;

    if(file_length % PAGE_SIZE != 0){
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    pager->capacity = 0;
    pager->pages = NULL;
    pager->free_head = 0;
    grow_page_cache(pager, pager->num_pages > INITIAL_PAGE_CAPACITY ? pager->num_pages : INITIAL_PAGE_CAPACITY);

    return pager;
}

/*
 * Returns page `page_num` from the cache, loading it on a miss.
 *
 * Only pages that exist, or the one page just past the end, can be fetched:
 * new pages are always allocated at num_pages and fetched straight away, so a
 * request further out means a bug or a corrupt page pointer — including the
 * b-tree's INVALID_PAGE_NUM marker — and is fatal rather than silently
 * creating a page in the middle of nowhere.
 *
 * A miss allocates a zero-filled page and, if that page already exists on disk
 * (page_num < num_pages), reads it in. A new page stays zero-filled for the
 * caller to build on, so bytes it never writes — padding, unused cells — reach
 * the disk as zeros rather than leftover memory, and the same inserts always
 * produce the same file. Fetching the next page grows num_pages.
 */
void* pager_get_page(Pager* pager, uint32_t page_num){
    if(page_num > pager->num_pages){
        printf("Tried to fetch page %u, but the database has only %u pages\n",
               page_num, pager->num_pages);
        exit(EXIT_FAILURE);
    }

    if(page_num >= pager->capacity)
        grow_page_cache(pager, page_num + 1);

    if(pager->pages[page_num] == NULL){
        void* page = calloc(1, PAGE_SIZE);

        if(page_num < pager->num_pages){
            lseek(pager->file_descriptor, (off_t)page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if(bytes_read == -1){
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }

        pager->pages[page_num] = page;

        if(page_num >= pager->num_pages)
            pager->num_pages = page_num + 1;
    }

    return pager->pages[page_num];
}

/* Where a free page keeps the number of the next page on the free list. */
#define FREE_PAGE_NEXT_OFFSET 4u

/*
 * Takes the first page off the free list if there is one, after checking it
 * really is marked free — a list pointing at a live node would hand that node
 * out a second time, so it's treated as corruption. Otherwise the page is the
 * next one past the end of the file, which fetching adds to the database. The
 * page is zero-filled either way: a reused page is cleared here, a new one is
 * allocated zeroed.
 */
uint32_t pager_allocate_page(Pager* pager){
    if(pager->free_head != 0){
        uint32_t page_num = pager->free_head;
        uint8_t* page = pager_get_page(pager, page_num);
        if(page[0] != FREE_PAGE_MARKER){
            printf("Corrupt free list: page %u is not a free page\n", page_num);
            exit(EXIT_FAILURE);
        }
        memcpy(&pager->free_head, page + FREE_PAGE_NEXT_OFFSET, sizeof(uint32_t));
        memset(page, 0, PAGE_SIZE);
        return page_num;
    }

    uint32_t page_num = pager->num_pages;
    pager_get_page(pager, page_num);
    return page_num;
}

/*
 * Pushes a page onto the front of the free list: cleared, marked with
 * FREE_PAGE_MARKER, and pointing at the old head.
 */
void pager_free_page(Pager* pager, uint32_t page_num){
    uint8_t* page = pager_get_page(pager, page_num);
    memset(page, 0, PAGE_SIZE);
    page[0] = FREE_PAGE_MARKER;
    memcpy(page + FREE_PAGE_NEXT_OFFSET, &pager->free_head, sizeof(uint32_t));
    pager->free_head = page_num;
}

/*
 * Writes one whole page back to its offset in the file. A node always fills a
 * page, so unlike the pre-b-tree version there is no partial-page size to pass.
 * Flushing an unresident page or any I/O failure is fatal.
 */
void pager_flush(Pager* pager, uint32_t page_num){
    if(pager->pages[page_num] == NULL){
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = lseek(pager->file_descriptor, (off_t)page_num * PAGE_SIZE, SEEK_SET);
    if(offset == -1){
        printf("Error seeking: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written = write(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE);
    if(bytes_written == -1){
        printf("Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

/*
 * Closes the file and frees the cache and the pager. Callers flush any dirty
 * pages before this; here we only release memory (already-freed slots are NULL
 * and skipped).
 */
void pager_close(Pager* pager){
    int result = close(pager->file_descriptor);
    if(result == -1){
        printf("Error closing db file.\n");
        exit(EXIT_FAILURE);
    }

    for(uint32_t i = 0; i < pager->capacity; ++i){
        if(pager->pages[i]){
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }

    free(pager->pages);
    free(pager);
}
