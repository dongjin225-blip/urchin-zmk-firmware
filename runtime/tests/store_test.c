#include "../rt_store.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static unsigned char flash[2*RT_BANK_SIZE],baseline[sizeof(flash)],readback[RT_BANK_MAX_PAYLOAD];
static long fail_after=-1;
static int read_flash(void *ctx,size_t offset,void *out,size_t length){(void)ctx;if(offset+length>sizeof(flash))return -EINVAL;memcpy(out,flash+offset,length);return 0;}
static int write_flash(void *ctx,size_t offset,const void *data,size_t length){
 (void)ctx;assert(!(offset%4));assert(!(length%4));if(offset+length>sizeof(flash))return -EINVAL;
 const unsigned char *input=data;
 for(size_t i=0;i<length;i++){if(fail_after==0)return -EIO;if(fail_after>0)fail_after--;assert((flash[offset+i]&input[i])==input[i]);flash[offset+i]&=input[i];}return 0;
}
static int erase_flash(void *ctx,size_t offset,size_t length){(void)ctx;assert(!(offset%4096));assert(!(length%4096));if(offset+length>sizeof(flash))return -EINVAL;memset(flash+offset,255,length);return 0;}
static struct rt_store store(void){return (struct rt_store){.read=read_flash,.write=write_flash,.erase=erase_flash,.schema=1234,.active_bank=-1};}
static int recover(struct rt_store *s){
 struct rt_bank_header a,b;int va=rt_store_read_bank(s,0,&a,readback,sizeof(readback)),vb=rt_store_read_bank(s,1,&b,readback,sizeof(readback));
 if(va&&vb){s->active_bank=-1;s->generation=0;return -1;}
 unsigned bank=!vb&&(va||(int32_t)(b.generation-a.generation)>0)?1:0;
 struct rt_bank_header h;assert(!rt_store_read_bank(s,bank,&h,readback,sizeof(readback)));s->active_bank=bank;s->generation=h.generation;return 0;
}
int main(void){
 assert(rt_store_crc32(0,"123456789",9)==0xcbf43926u);
 assert(rt_store_crc32(rt_store_crc32(0,"1234",4),"56789",5)==0xcbf43926u);
 memset(flash,255,sizeof(flash));struct rt_store s=store();assert(recover(&s)==-1);
 unsigned char first[101],second[517];memset(first,11,sizeof(first));memset(second,22,sizeof(second));
 assert(!rt_store_commit(&s,first,sizeof(first),0));assert(!recover(&s));assert(s.generation==1);assert(!memcmp(readback,first,sizeof(first)));
 memcpy(baseline,flash,sizeof(flash));
 // Interrupt every programmed byte in body, padding and final commit header.
 for(long cut=0;cut<((long)sizeof(second)+3)/4*4+64;cut++){
  memcpy(flash,baseline,sizeof(flash));s=store();assert(!recover(&s));fail_after=cut;
  assert(rt_store_commit(&s,second,sizeof(second),1)<0);fail_after=-1;
  s=store();assert(!recover(&s));assert(s.generation==1);assert(!memcmp(readback,first,sizeof(first)));
 }
 memcpy(flash,baseline,sizeof(flash));s=store();assert(!recover(&s));assert(rt_store_commit(&s,second,sizeof(second),0)==-ESTALE);assert(!memcmp(flash,baseline,sizeof(flash)));
 assert(!rt_store_commit(&s,second,sizeof(second),1));assert(!recover(&s));assert(s.generation==2);assert(!memcmp(readback,second,sizeof(second)));
 // Corrupt the newest body and recover the older complete generation.
 flash[s.active_bank*RT_BANK_SIZE+RT_BANK_HEADER_SIZE+3]^=1;s=store();assert(!recover(&s));assert(s.generation==1);assert(!memcmp(readback,first,sizeof(first)));
 s=store();s.schema++;assert(recover(&s)==-1);
 puts("dual-bank power-loss, corruption, stale-write and schema tests passed");return 0;
}
