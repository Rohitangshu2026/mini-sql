#define _POSIX_C_SOURCE 200809L   /* open/read/write/lseek under strict -std=c11 */

#include "pager.h"

#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<errno.h>

const uint32_t PAGE_SIZE = 4096;

/*
 * Opens the database file (creating it if missing, read/write, owner-only) and
 * sets up an empty page cache. num_pages is derived from the file size; a file
 * whose length is not a whole number of pages can only be corrupt, so we bail.
 * Any I/O failure here is fatal.
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

    for(uint32_t i = 0; i < TABLE_MAX_PAGES; ++i)
        pager->pages[i] = NULL;

    return pager;
}

/*
 * Returns page `page_num` from the cache, loading it on a miss.
 *
 * A miss allocates a fresh page and, if that page already exists on disk
 * (page_num < num_pages), reads it in; a page past the end of the file is left
 * as freshly allocated memory to be filled by the caller and written out
 * later. Touching a page beyond the current extent grows num_pages. Out-of
 * -range requests are fatal.
 */
void* pager_get_page(Pager* pager, uint32_t page_num){
    if(page_num >= TABLE_MAX_PAGES){
        printf("Tried to fetch page number out of bounds. %u >= %u\n",
               page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if(pager->pages[page_num] == NULL){
        void* page = malloc(PAGE_SIZE);

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

    for(uint32_t i = 0; i < TABLE_MAX_PAGES; ++i){
        if(pager->pages[i]){
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }

    free(pager);
}
