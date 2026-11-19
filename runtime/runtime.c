/* SPDX-License-Identifier: MIT
 * Experimental USB runtime definitions. Persistent commits use two flash banks;
 * a new document becomes visible only after its body has been read back and its
 * final commit header has been programmed. The previous bank is never erased
 * until a later successful transaction. Definitions activate after reboot.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zmk/studio/rpc.h>
#include <zmk/runtime_editor.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <pb_encode.h>
#include <string.h>
#include <stdio.h>
#include "generated_defaults.h"
#include "rt_store.h"

LOG_MODULE_REGISTER(runtime_editor, LOG_LEVEL_INF);
#define BANK_SIZE 65536u
#define HEADER_SIZE 64u
#define BANK_MAGIC 0x3154524bu
#define PROTOCOL_VERSION 1u
#define MAX_DOCUMENT 32768u
#define MAX_COMMAND 512u
#define RESPONSE_CHUNK 448u

#define bank_header rt_bank_header
BUILD_ASSERT(sizeof(struct bank_header)==HEADER_SIZE);
static const struct flash_area *area;
static int active_bank=-1;
static struct bank_header active;
static uint8_t payload[RT_PAYLOAD_BYTES] __aligned(4);
static uint32_t upload_length,upload_position,upload_crc,upload_generation;
static bool uploading;
static bool reboot_pending;
static struct rt_store store;
static uint8_t pool[RT_POOL_BYTES] __aligned(8);
static size_t pool_position;
static struct {const struct device *dev;const void *config;bool remote;uint32_t param1,param2;} overrides[RT_MAX_BEHAVIORS];
static unsigned override_count;
static uint8_t response_bytes[512];
static size_t response_length;
static char capability_json[16384];
static uint8_t exported_config[2048];
static size_t capability_length;
static K_MUTEX_DEFINE(runtime_mutex);
static uint16_t source_behavior=UINT16_MAX;
static struct {uint16_t from,to;} dependencies[512];
static unsigned dependency_count;
static struct {uint16_t id;uint8_t state,height;} dependency_nodes[RT_MAX_BEHAVIORS];
static unsigned dependency_nodes_count;

uint8_t rt_u8(struct rt_reader *r) {
    if(r->pos>=r->size){r->error=-EBADMSG;return 0;}
    return r->data[r->pos++];
}
uint16_t rt_u16(struct rt_reader *r){uint16_t a=rt_u8(r);return a|((uint16_t)rt_u8(r)<<8);}
uint32_t rt_u32(struct rt_reader *r){uint32_t a=rt_u16(r);return a|((uint32_t)rt_u16(r)<<16);}
void rt_w8(struct rt_writer *w,uint8_t value){if(w->pos>=w->size){w->error=-E2BIG;return;}w->data[w->pos++]=value;}
void rt_w16(struct rt_writer *w,uint16_t value){rt_w8(w,value);rt_w8(w,value>>8);}
void rt_w32(struct rt_writer *w,uint32_t value){rt_w16(w,value);rt_w16(w,value>>16);}
void rt_wbehavior(struct rt_writer *w,const char *name){uint16_t id=zmk_behavior_get_local_id(name);if(id==UINT16_MAX)w->error=-EINVAL;rt_w16(w,id);}
void rt_wbinding(struct rt_writer *w,const struct zmk_behavior_binding *b){rt_wbehavior(w,b->behavior_dev);rt_w32(w,b->param1);rt_w32(w,b->param2);}
const char *rt_behavior(struct rt_reader *r) {
    uint16_t id=rt_u16(r);
    const char *name=zmk_behavior_find_behavior_name_from_local_id(id);
    if(!name){r->error=-EINVAL;return "";}
    if(source_behavior!=UINT16_MAX && r->dry){
        if(dependency_count>=ARRAY_SIZE(dependencies)){r->error=-E2BIG;return name;}
        dependencies[dependency_count].from=source_behavior;
        dependencies[dependency_count++].to=id;
    }
    return name;
}
struct zmk_behavior_binding rt_binding(struct rt_reader *r) {
    const char *name=rt_behavior(r);uint32_t p1=rt_u32(r),p2=rt_u32(r);
    return (struct zmk_behavior_binding){.behavior_dev=name,.param1=p1,.param2=p2};
}
void *rt_allocate(size_t size) {
    size=(size+7)&~7u;
    if(size>RT_POOL_BYTES-pool_position)return NULL;
    void *result=pool+pool_position;pool_position+=size;memset(result,0,size);return result;
}
void *rt_reader_allocate(struct rt_reader *r,size_t size) {
    size=(size+7)&~7u;r->allocation+=size;
    if(r->allocation>RT_POOL_BYTES){r->error=-ENOMEM;return NULL;}
    return r->dry?NULL:rt_allocate(size);
}
const void *rt_config(const struct device *dev) {
    for(unsigned i=0;i<override_count;i++)if(overrides[i].dev==dev)return overrides[i].config;
    return dev->config;
}
int rt_override(const struct device *dev,const void *config) {
    if(!config)return -ENOMEM;
    for(unsigned i=0;i<override_count;i++)if(overrides[i].dev==dev)return -EEXIST;
    if(override_count>=RT_MAX_BEHAVIORS)return -E2BIG;
    overrides[override_count].dev=dev;overrides[override_count++].config=config;return 0;
}
void rt_remote_override(const struct device *dev,uint32_t param1,uint32_t param2){
    for(unsigned i=0;i<override_count;i++)if(overrides[i].dev==dev){overrides[i].remote=true;overrides[i].param1=param1;overrides[i].param2=param2;return;}
}
void rt_prepare_binding(struct zmk_behavior_binding *binding){
    const struct device *dev=zmk_behavior_get_binding(binding->behavior_dev);
    for(unsigned i=0;i<override_count;i++)if(overrides[i].dev==dev && overrides[i].remote){binding->param1=overrides[i].param1;binding->param2=overrides[i].param2;return;}
}

__weak int rt_set_hold_tap(const struct device *d,struct rt_reader *r){return -ENOTSUP;}
__weak int rt_set_macro(const struct device *d,struct rt_reader *r){return -ENOTSUP;}
__weak int rt_set_mod_morph(const struct device *d,struct rt_reader *r){return -ENOTSUP;}
__weak int rt_set_sticky_key(const struct device *d,struct rt_reader *r){return -ENOTSUP;}
__weak int rt_set_tap_dance(const struct device *d,struct rt_reader *r){return -ENOTSUP;}
__weak int rt_export_hold_tap(const struct device *d,struct rt_writer *w){return -ENOTSUP;}
__weak int rt_export_macro(const struct device *d,struct rt_writer *w){return -ENOTSUP;}
__weak int rt_export_mod_morph(const struct device *d,struct rt_writer *w){return -ENOTSUP;}
__weak int rt_export_sticky_key(const struct device *d,struct rt_writer *w){return -ENOTSUP;}
__weak int rt_export_tap_dance(const struct device *d,struct rt_writer *w){return -ENOTSUP;}
#define RT_WEAK_FAMILY(name) __weak int rt_set_##name(const struct device *d,struct rt_reader *r){return -ENOTSUP;} __weak int rt_export_##name(const struct device *d,struct rt_writer *w){return -ENOTSUP;}
RT_WEAK_FAMILY(caps_word)
RT_WEAK_FAMILY(key_repeat)
RT_WEAK_FAMILY(key_toggle)
RT_WEAK_FAMILY(toggle_layer)
RT_WEAK_FAMILY(soft_off)
RT_WEAK_FAMILY(reset)

static int validate_dependency(uint16_t node,uint16_t *stack,unsigned depth) {
    if(depth>=16)return -ELOOP;
    for(unsigned i=0;i<depth;i++)if(stack[i]==node)return -ELOOP;
    unsigned entry=0;
    while(entry<dependency_nodes_count && dependency_nodes[entry].id!=node)entry++;
    if(entry==dependency_nodes_count)return 0;
    if(dependency_nodes[entry].state==2)return dependency_nodes[entry].height+depth>=16?-ELOOP:0;
    stack[depth]=node;
    unsigned height=0;
    for(unsigned i=0;i<dependency_count;i++)if(dependencies[i].from==node){
        int err=validate_dependency(dependencies[i].to,stack,depth+1);if(err)return err;
        unsigned child=0;while(child<dependency_nodes_count && dependency_nodes[child].id!=dependencies[i].to)child++;
        height=MAX(height,child<dependency_nodes_count?dependency_nodes[child].height+1:1);
    }
    dependency_nodes[entry].height=height;dependency_nodes[entry].state=2;
    return 0;
}

static int process_layers(struct rt_reader *r) {
    unsigned count=rt_u8(r);
    if(!count || count>ZMK_KEYMAP_LAYERS_LEN)return -EINVAL;
    uint32_t seen=0;
    for(unsigned index=0;index<count;index++){
        uint8_t id=rt_u8(r),name_length=rt_u8(r);
        if(id>=ZMK_KEYMAP_LAYERS_LEN || (seen&BIT(id)) || name_length>=CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN)return -EINVAL;
        seen|=BIT(id);
        char name[CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN]={0};
        for(unsigned c=0;c<name_length;c++)name[c]=rt_u8(r);
        unsigned bindings=rt_u16(r);if(bindings!=ZMK_KEYMAP_LEN)return -EINVAL;
        if(!r->dry){
            bool found=false;
            for(unsigned j=0;j<ZMK_KEYMAP_LAYERS_LEN;j++)if(zmk_keymap_layer_index_to_id(j)==id)found=true;
            int err=found?0:zmk_keymap_restore_layer(id,index);if(err<0)return err;
            for(unsigned j=0;j<ZMK_KEYMAP_LAYERS_LEN;j++)if(zmk_keymap_layer_index_to_id(j)==id && j!=index){err=zmk_keymap_move_layer(j,index);if(err<0)return err;break;}
            err=zmk_keymap_set_layer_name(id,name,name_length);if(err<0)return err;
        }
        for(unsigned position=0;position<bindings;position++){
            struct zmk_behavior_binding binding=rt_binding(r);
            if(r->error)return r->error;
            if(!r->dry){int err=zmk_keymap_set_layer_binding_at_idx(id,position,binding);if(err<0)return err;}
        }
    }
    if(!(seen&BIT(0)))return -EINVAL;
    if(!r->dry){
        for(int j=ZMK_KEYMAP_LAYERS_LEN-1;j>=0;j--){uint8_t id=zmk_keymap_layer_index_to_id(j);if(id!=UINT8_MAX && !(seen&BIT(id))){int err=zmk_keymap_remove_layer(j);if(err<0)return err;}}
    }
    return r->error;
}

static int process_plan(const uint8_t *bytes,size_t length,bool dry) {
    struct rt_reader r={.data=bytes,.size=length,.dry=dry};
    if(rt_u32(&r)!=0x31504d4bu)return -EPROTO;
    if(dry){dependency_count=0;dependency_nodes_count=0;memset(dependency_nodes,0,sizeof(dependency_nodes));}
    bool seen[5]={0};
    while(r.pos<r.size && !r.error){
        uint8_t op=rt_u8(&r);if(op<1 || op>4 || seen[op])return -EINVAL;seen[op]=true;
        int err=0;
        switch(op){
        case 1:err=rt_set_combos(&r);break;
        case 2:err=rt_set_conditionals(&r);break;
        case 3:{
            unsigned count=rt_u8(&r);if(count>RT_MAX_BEHAVIORS)return -E2BIG;
            uint16_t ids[RT_MAX_BEHAVIORS];
            for(unsigned i=0;i<count;i++){
                uint8_t kind=rt_u8(&r);uint16_t id=rt_u16(&r),size=rt_u16(&r);
                for(unsigned j=0;j<i;j++)if(ids[j]==id)return -EEXIST;ids[i]=id;
                const char *name=zmk_behavior_find_behavior_name_from_local_id(id);
                const struct device *dev=name?zmk_behavior_get_binding(name):NULL;
                if(!dev || size>r.size-r.pos)return -EINVAL;
                if(dry){dependency_nodes[dependency_nodes_count++].id=id;}
                struct rt_reader child={.data=r.data+r.pos,.size=size,.dry=dry,.allocation=r.allocation};
                source_behavior=id;
                switch(kind){
                case 1:err=rt_set_hold_tap(dev,&child);break;
                case 2:err=rt_set_macro(dev,&child);break;
                case 3:err=rt_set_mod_morph(dev,&child);break;
                case 4:err=rt_set_sticky_key(dev,&child);break;
                case 5:err=rt_set_tap_dance(dev,&child);break;
                case 6:err=rt_set_caps_word(dev,&child);break;
                case 7:err=rt_set_key_repeat(dev,&child);break;
                case 8:err=rt_set_key_toggle(dev,&child);break;
                case 9:err=rt_set_toggle_layer(dev,&child);break;
                case 10:err=rt_set_soft_off(dev,&child);break;
                case 11:err=rt_set_reset(dev,&child);break;
                default:err=-ENOTSUP;
                }
                source_behavior=UINT16_MAX;
                if(err || child.error || child.pos!=child.size)return err?err:(child.error?child.error:-EBADMSG);
                r.pos+=size;r.allocation=child.allocation;
            }
            break;
        }
        case 4:err=process_layers(&r);break;
        }
        if(err)return err;
    }
    if(r.error)return r.error;
    if(!seen[1] || !seen[2] || !seen[3] || !seen[4])return -EINVAL;
    if(dry){uint16_t stack[16];for(unsigned i=0;i<dependency_count;i++){int err=validate_dependency(dependencies[i].from,stack,0);if(err)return err;}}
    return 0;
}

static uint32_t default_crc(void){return crc32_ieee(rt_default_document,sizeof(rt_default_document));}
static int storage_read(void *context,size_t offset,void *out,size_t size){return flash_area_read(context,offset,out,size);}
static int storage_write(void *context,size_t offset,const void *bytes,size_t size){return flash_area_write(context,offset,bytes,size);}
static int storage_erase(void *context,size_t offset,size_t size){return flash_area_erase(context,offset,size);}
static bool read_bank(unsigned bank,struct bank_header *header) {
    return rt_store_read_bank(&store,bank,header,payload,sizeof(payload))==0;
}
static int payload_plan(bool apply) {
    uint32_t plan=sys_get_le32(payload),doc=sys_get_le32(payload+4);
    if(plan>active.length-8 || doc!=active.length-8-plan || doc>MAX_DOCUMENT)return -EBADMSG;
    return process_plan(payload+8,plan,!apply);
}

int rt_boot(void) {
    if(flash_area_open(FIXED_PARTITION_ID(runtime_partition),&area))return 0;
    store=(struct rt_store){.context=(void *)area,.read=storage_read,.write=storage_write,.erase=storage_erase,.schema=default_crc(),.active_bank=-1};
    struct bank_header h[2];bool valid[2];
    valid[0]=read_bank(0,&h[0]);valid[1]=read_bank(1,&h[1]);
    int first=valid[1] && (!valid[0] || (int32_t)(h[1].generation-h[0].generation)>0)?1:0;
    for(unsigned attempt=0;attempt<2;attempt++){
        unsigned bank=attempt?1-first:first;if(!valid[bank])continue;
        if(!read_bank(bank,&active))continue;
        if(payload_plan(false))continue;
        pool_position=0;override_count=0;
        if(payload_plan(true)){LOG_ERR("Runtime activation failed; default firmware must be restored");return -EINVAL;}
        active_bank=bank;LOG_INF("Runtime document generation %u loaded",active.generation);return 0;
    }
    memset(&active,0,sizeof(active));return 0;
}

static void reboot_work(struct k_work *work){sys_reboot(SYS_REBOOT_COLD);}
static K_WORK_DELAYABLE_DEFINE(reboot_task,reboot_work);
static void build_capabilities(void) {
    capability_length=snprintk(capability_json,sizeof(capability_json),"{\"version\":1,\"schema\":%u,\"generation\":%u,\"maxPayload\":%u,\"maxDocument\":%u,\"maxLayers\":%u,\"maxCombos\":64,\"maxComboKeys\":8,\"maxConditionals\":32,\"activation\":\"reboot\",\"behaviors\":[",default_crc(),active_bank<0?0:active.generation,RT_PAYLOAD_BYTES,MAX_DOCUMENT,ZMK_KEYMAP_LAYERS_LEN);
    bool comma=false;
    for(unsigned i=0;i<ARRAY_SIZE(rt_labels);i++){
        uint16_t id=zmk_behavior_get_local_id(rt_labels[i].device);if(id==UINT16_MAX)continue;
        int n=snprintk(capability_json+capability_length,sizeof(capability_json)-capability_length,"%s{\"id\":%u,\"label\":\"%s\",\"compatible\":\"%s\"}",comma?",":"",id,rt_labels[i].label,rt_labels[i].compatible);
        if(n<0 || n>=sizeof(capability_json)-capability_length){capability_length=0;return;}
        capability_length+=n;comma=true;
    }
    if(capability_length+3>=sizeof(capability_json)){capability_length=0;return;}
    memcpy(capability_json+capability_length,"]}",3);capability_length+=2;
}

static int commit_upload(void) {
    if(!area || !uploading || upload_position!=upload_length)return -EINVAL;
    if(crc32_ieee(payload,upload_length)!=upload_crc)return -EBADMSG;
    struct bank_header saved=active;
    active.length=upload_length;
    int err=payload_plan(false);active=saved;if(err)return err;
    store.active_bank=active_bank;store.generation=active_bank<0?0:active.generation;
    err=rt_store_commit(&store,payload,upload_length,upload_generation);if(err)return err;
    uploading=false;reboot_pending=true;k_work_schedule(&reboot_task,K_MSEC(1200));return 0;
}

/* Commands: INFO, READ_CAPS, READ_SOURCE, BEGIN, WRITE, VALIDATE, COMMIT, ABORT,
 * RESTORE. Every response starts with signed little-endian errno and generation.
 * This is a secured keymap RPC, so stock Studio lock rules also protect uploads. */
static zmk_studio_Response runtime(const zmk_studio_Request *request) {
    k_mutex_lock(&runtime_mutex,K_FOREVER);
    const uint8_t *command=request->subsystem.keymap.request_type.runtime.bytes;
    size_t length=request->subsystem.keymap.request_type.runtime.size;
    struct rt_reader r={.data=command,.size=length,.dry=true};
    unsigned op=rt_u8(&r);int error=0;response_length=8;
    uint32_t generation=active_bank<0?0:active.generation;
    if(reboot_pending)error=-EBUSY;
    else if(!area)error=-ENODEV;
    else switch(op){
    case 1:
        if(uploading){error=-EBUSY;break;}
        build_capabilities();
        sys_put_le32(PROTOCOL_VERSION,response_bytes+8);sys_put_le32(default_crc(),response_bytes+12);
        sys_put_le32(capability_length,response_bytes+16);
        sys_put_le32(active_bank<0?sizeof(rt_default_document):sys_get_le32(payload+4),response_bytes+20);
        response_length=24;break;
    case 2:{
        uint32_t offset=rt_u32(&r);if(!capability_length || offset>capability_length){error=-EINVAL;break;}
        size_t n=MIN(RESPONSE_CHUNK,capability_length-offset);memcpy(response_bytes+8,capability_json+offset,n);response_length+=n;break;
    }
    case 3:{
        uint32_t offset=rt_u32(&r);if(uploading){error=-EBUSY;break;}
        const uint8_t *source=active_bank<0?rt_default_document:payload+8+sys_get_le32(payload);
        size_t size=active_bank<0?sizeof(rt_default_document):sys_get_le32(payload+4);
        if(offset>size){error=-EINVAL;break;}size_t n=MIN(RESPONSE_CHUNK,size-offset);memcpy(response_bytes+8,source+offset,n);response_length+=n;break;
    }
    case 4:{
        if(uploading){error=-EBUSY;break;}
        uint32_t schema=rt_u32(&r),expected=rt_u32(&r),size=rt_u32(&r),crc=rt_u32(&r);
        if(schema!=default_crc() || expected!=generation){error=-ESTALE;break;}
        if(size<8 || size>RT_PAYLOAD_BYTES){error=-E2BIG;break;}
        uploading=true;upload_length=size;upload_crc=crc;upload_generation=expected;upload_position=0;break;
    }
    case 5:{
        uint32_t offset=rt_u32(&r);size_t n=r.size-r.pos;
        if(!uploading || offset!=upload_position || n>upload_length-upload_position){error=-EINVAL;break;}
        memcpy(payload+upload_position,r.data+r.pos,n);upload_position+=n;r.pos=r.size;break;
    }
    case 6:
        if(!uploading || upload_position!=upload_length){error=-EINVAL;break;}
        if(crc32_ieee(payload,upload_length)!=upload_crc){error=-EBADMSG;break;}
        {struct bank_header saved=active;active.length=upload_length;error=payload_plan(false);active=saved;}break;
    case 7:error=commit_upload();break;
    case 8:
        uploading=false;if(active_bank>=0 && !read_bank(active_bank,&active))error=-EIO;break;
    case 9:
        error=flash_area_erase(area,0,2*BANK_SIZE);if(!error){reboot_pending=true;k_work_schedule(&reboot_task,K_MSEC(1200));}break;
    case 10:{
        uint16_t id=rt_u16(&r);uint32_t offset=rt_u32(&r);
        const char *name=zmk_behavior_find_behavior_name_from_local_id(id);
        const struct device *dev=name?zmk_behavior_get_binding(name):NULL;
        if(!dev){error=-EINVAL;break;}
        struct rt_writer w={.data=exported_config,.size=sizeof(exported_config)};
        int (*exporters[])(const struct device *,struct rt_writer *)={rt_export_hold_tap,rt_export_macro,rt_export_mod_morph,rt_export_sticky_key,rt_export_tap_dance,rt_export_caps_word,rt_export_key_repeat,rt_export_key_toggle,rt_export_toggle_layer,rt_export_soft_off,rt_export_reset};
        unsigned kind=0;
        for(unsigned i=0;i<ARRAY_SIZE(exporters);i++){
            w.pos=0;w.error=0;int result=exporters[i](dev,&w);
            if(!result){kind=i+1;break;}
            if(result!=-ENOTSUP){error=result;break;}
        }
        if(!kind || w.error || offset>w.pos){error=error?error:(w.error?w.error:-ENOTSUP);break;}
        size_t n=MIN(RESPONSE_CHUNK,w.pos-offset);
        response_bytes[8]=kind;sys_put_le16(w.pos,response_bytes+9);
        memcpy(response_bytes+11,exported_config+offset,n);response_length=11+n;break;
    }
    default:error=-ENOTSUP;
    }
    if(r.error)error=r.error;
    if(error)response_length=8;
    sys_put_le32((uint32_t)error,response_bytes);sys_put_le32(generation,response_bytes+4);
    zmk_studio_Response response=ZMK_RPC_RESPONSE(keymap,runtime,{.size=response_length});
    memcpy(response.type.request_response.subsystem.keymap.response_type.runtime.bytes,response_bytes,response_length);
    k_mutex_unlock(&runtime_mutex);return response;
}
ZMK_RPC_SUBSYSTEM_HANDLER(keymap,runtime,ZMK_STUDIO_RPC_HANDLER_SECURED);
