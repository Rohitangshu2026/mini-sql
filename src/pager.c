#define _POSIX_C_SOURCE 200809L   /* open/read/write/lseek under strict -std=c11 */

#include "pager.h"

#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<errno.h>

const uint32_t PAGE_SIZE = 4096;

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

    for(uint32_t i = 0; i < TABLE_MAX_PAGES; ++i)
        pager->pages[i] = NULL;

    return pager;
}

void* pager_get_page(Pager* pager, uint32_t page_num){
    if(page_num >= TABLE_MAX_PAGES){
        printf("Tried to fetch page number out of bounds. %u >= %u\n",
               page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if(pager->pages[page_num] == NULL){
        /* Cache miss: allocate a page and, if it exists on disk, read it in. */
        void* page = malloc(PAGE_SIZE);
        uint32_t num_pages = pager->file_length / PAGE_SIZE;

        /* a partial page at the end of the file still counts as a page */
        if(pager->file_length % PAGE_SIZE)
            num_pages += 1;

        if(page_num < num_pages){
            lseek(pager->file_descriptor, (off_t)page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if(bytes_read == -1){
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }

        pager->pages[page_num] = page;
    }

    return pager->pages[page_num];
}

void pager_flush(Pager* pager, uint32_t page_num, uint32_t size){
    if(pager->pages[page_num] == NULL){
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = lseek(pager->file_descriptor, (off_t)page_num * PAGE_SIZE, SEEK_SET);
    if(offset == -1){
        printf("Error seeking: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written = write(pager->file_descriptor, pager->pages[page_num], size);
    if(bytes_written == -1){
        printf("Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

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
