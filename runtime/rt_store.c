/* SPDX-License-Identifier: MIT */
#include "rt_store.h"
#include <errno.h>
#include <string.h>
#define MAGIC 0x3154524bu
_Static_assert(sizeof(struct rt_bank_header)==RT_BANK_HEADER_SIZE,"Bank header ABI");
uint32_t rt_store_crc32(uint32_t previous,const void *data,size_t length){
 uint32_t crc=~previous;const uint8_t *bytes=data;
 for(size_t i=0;i<length;i++){crc^=bytes[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
 return ~crc;
}
int rt_store_read_bank(struct rt_store *s,unsigned bank,struct rt_bank_header *h,void *data,size_t capacity){
 if(bank>1)return -EINVAL;
 int error=s->read(s->context,bank*RT_BANK_SIZE,h,sizeof(*h));if(error)return error;
 if(h->magic!=MAGIC||h->version!=1||h->document_crc!=s->schema||h->length<8||h->length>RT_BANK_MAX_PAYLOAD||h->length>capacity)return -EBADMSG;
 uint32_t expected=h->header_crc;h->header_crc=0;uint32_t actual=rt_store_crc32(0,h,sizeof(*h));h->header_crc=expected;
 if(actual!=expected)return -EBADMSG;
 error=s->read(s->context,bank*RT_BANK_SIZE+RT_BANK_HEADER_SIZE,data,h->length);if(error)return error;
 return rt_store_crc32(0,data,h->length)==h->crc?0:-EBADMSG;
}
int rt_store_commit(struct rt_store *s,const void *data,size_t length,uint32_t expected_generation){
 if(length<8||length>RT_BANK_MAX_PAYLOAD)return -E2BIG;
 if(expected_generation!=s->generation)return -ESTALE;
 unsigned bank=s->active_bank<0?0u:1u-(unsigned)s->active_bank;
 if(bank>1)return -EINVAL;
 size_t address=bank*RT_BANK_SIZE;
 struct rt_bank_header h={.magic=MAGIC,.version=1,.generation=s->generation+1,.length=length,.crc=rt_store_crc32(0,data,length),.document_crc=s->schema};
 int error=s->erase(s->context,address,RT_BANK_SIZE);if(error)return error;
 size_t aligned=length&~3u;
 if(aligned){error=s->write(s->context,address+RT_BANK_HEADER_SIZE,data,aligned);if(error)return error;}
 if(aligned<length){uint8_t tail[4]={255,255,255,255};memcpy(tail,(const uint8_t *)data+aligned,length-aligned);error=s->write(s->context,address+RT_BANK_HEADER_SIZE+aligned,tail,4);if(error)return error;}
 uint8_t scratch[128];uint32_t checksum=0;
 for(size_t offset=0;offset<length;offset+=sizeof(scratch)){
  size_t count=length-offset;if(count>sizeof(scratch))count=sizeof(scratch);
  error=s->read(s->context,address+RT_BANK_HEADER_SIZE+offset,scratch,count);if(error)return error;
  checksum=rt_store_crc32(checksum,scratch,count);
 }
 if(checksum!=h.crc)return -EIO;
 h.header_crc=rt_store_crc32(0,&h,sizeof(h));
 error=s->write(s->context,address,&h,sizeof(h));if(error)return error;
 return 0;
}
