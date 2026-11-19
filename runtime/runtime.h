/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/device.h>
#include <zmk/behavior.h>
#include <stdint.h>
#include <stddef.h>

#define RT_MAX_COMBOS 64
#define RT_MAX_COMBO_KEYS 8
#define RT_MAX_CONDITIONALS 32
#define RT_MAX_BEHAVIORS 96
#define RT_POOL_BYTES 24576
#define RT_PAYLOAD_BYTES 65472

struct rt_reader { const uint8_t *data; size_t size, pos, allocation; int error; bool dry; };
struct rt_writer {uint8_t *data;size_t size,pos;int error;};
void rt_w8(struct rt_writer *w,uint8_t value);
void rt_w16(struct rt_writer *w,uint16_t value);
void rt_w32(struct rt_writer *w,uint32_t value);
void rt_wbehavior(struct rt_writer *w,const char *name);
void rt_wbinding(struct rt_writer *w,const struct zmk_behavior_binding *binding);
uint8_t rt_u8(struct rt_reader *r);
uint16_t rt_u16(struct rt_reader *r);
uint32_t rt_u32(struct rt_reader *r);
const char *rt_behavior(struct rt_reader *r);
struct zmk_behavior_binding rt_binding(struct rt_reader *r);
void *rt_allocate(size_t size);
void *rt_reader_allocate(struct rt_reader *r,size_t size);
const void *rt_config(const struct device *dev);
int rt_override(const struct device *dev, const void *config);
void rt_remote_override(const struct device *dev,uint32_t param1,uint32_t param2);
void rt_prepare_binding(struct zmk_behavior_binding *binding);
int rt_set_combos(struct rt_reader *r);
int rt_set_conditionals(struct rt_reader *r);
int rt_set_hold_tap(const struct device *dev, struct rt_reader *r);
int rt_set_macro(const struct device *dev, struct rt_reader *r);
int rt_set_mod_morph(const struct device *dev, struct rt_reader *r);
int rt_set_sticky_key(const struct device *dev, struct rt_reader *r);
int rt_set_tap_dance(const struct device *dev, struct rt_reader *r);
int rt_boot(void);
int rt_export_hold_tap(const struct device *dev,struct rt_writer *w);
int rt_export_macro(const struct device *dev,struct rt_writer *w);
int rt_export_mod_morph(const struct device *dev,struct rt_writer *w);
int rt_export_sticky_key(const struct device *dev,struct rt_writer *w);
int rt_export_tap_dance(const struct device *dev,struct rt_writer *w);
#define RT_EXTRA_FAMILY(name) int rt_set_##name(const struct device *,struct rt_reader *); int rt_export_##name(const struct device *,struct rt_writer *);
RT_EXTRA_FAMILY(caps_word)
RT_EXTRA_FAMILY(key_repeat)
RT_EXTRA_FAMILY(key_toggle)
RT_EXTRA_FAMILY(toggle_layer)
RT_EXTRA_FAMILY(soft_off)
RT_EXTRA_FAMILY(reset)
