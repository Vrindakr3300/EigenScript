#ifndef EXT_STORE_INTERNAL_H
#define EXT_STORE_INTERNAL_H

#include "eigenscript.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STORE_MAGIC "EIGS"
#define STORE_VERSION 1
#define STORE_PAGE_SIZE 4096
#define STORE_HEADER_SIZE 64
#define STORE_PAGE_DATA_SIZE (STORE_PAGE_SIZE - 7) /* 7 bytes header per page */
#define STORE_MAX_KEY_LEN 256
#define STORE_MAX_RECORD_SIZE (STORE_PAGE_DATA_SIZE - 6) /* key_len(2) + key + json_len(4) + json */

/* Page types */
#define PAGE_FREE 0
#define PAGE_CATALOG 1
#define PAGE_RECORDS 2

typedef struct {
    FILE *fp;
    char path[4096];
    uint32_t page_count;
    uint32_t free_page;  /* first free page (0 = none) */
    /* Collection catalog: in-memory dict mapping name -> {root_page, next_id} */
    Value *catalog;  /* EigenScript dict */
    int dirty;
} Store;

/* Page header on disk: [type:1][count:2][next:4] = 7 bytes */
typedef struct {
    uint8_t type;
    uint16_t count;
    uint32_t next_page;
    char data[STORE_PAGE_DATA_SIZE];
} Page;

void register_store_builtins(Env *env);

#endif
