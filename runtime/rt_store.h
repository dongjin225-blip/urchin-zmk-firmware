/* SPDX-License-Identifier: MIT - portable dual-bank persistence core. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define RT_BANK_SIZE 65536u
#define RT_BANK_HEADER_SIZE 64u
#define RT_BANK_MAX_PAYLOAD 65472u
struct rt_bank_header {uint32_t magic,version,generation,length,crc,document_crc,header_crc,reserved[9];};
struct rt_store {
 void *context;
 int (*read)(void *,size_t,void *,size_t);
 int (*write)(void *,size_t,const void *,size_t);
 int (*erase)(void *,size_t,size_t);
 uint32_t schema,generation;
 int active_bank;
};
uint32_t rt_store_crc32(uint32_t previous,const void *bytes,size_t length);
int rt_store_read_bank(struct rt_store *store,unsigned bank,struct rt_bank_header *header,void *payload,size_t capacity);
int rt_store_commit(struct rt_store *store,const void *payload,size_t length,uint32_t expected_generation);
