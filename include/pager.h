#ifndef PAGER_H
#define PAGER_H

#include<stdint.h>

/* The page cache lives in the pager now, so the paging constants live here
 * too (table.h includes this header). */
#define TABLE_MAX_PAGES 100
extern const uint32_t PAGE_SIZE;

typedef struct{
    int file_descriptor;
    uint32_t file_length;
    void* pages[TABLE_MAX_PAGES];
}Pager;

Pager* pager_open(const char* filename);
void* pager_get_page(Pager* pager, uint32_t page_num);
void pager_flush(Pager* pager, uint32_t page_num, uint32_t size);
void pager_close(Pager* pager);

#endif
