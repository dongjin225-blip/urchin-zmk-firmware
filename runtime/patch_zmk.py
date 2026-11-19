"""Version-checked build-time adaptation of pinned ZMK v0.3.

No installed firmware is changed by this script. All patches are applied to the
isolated west checkout used by the experimental build, never to upstream refs.
"""
import json
import pathlib
import pickle
import re
import shutil
import subprocess
import sys

root, messages, here, binary = map(pathlib.Path, sys.argv[1:5])
app = root / "app"
enabled=len(sys.argv)<6 or sys.argv[5]=="enabled"
if not (app / "src/studio/rpc.c").exists():
    raise SystemExit("Expected the pinned ZMK v0.3 source tree")

def edit(relative, transform):
    target = app / relative
    original = subprocess.check_output(["git","show","HEAD:app/"+relative],cwd=root).decode("utf-8")
    result = transform(original)
    target.write_text("/* RUNTIME_EDITOR_PATCH_V1 */\n" + result, encoding="utf-8")

def pristine(repository,relative):
    return subprocess.check_output(["git","show","HEAD:"+relative],cwd=repository).decode("utf-8")

# The official workflow caches west repositories, including dirty build outputs.
# Always reconstruct our transformations from the pinned Git objects. Peripheral
# builds also remove our cached central-only changes before compiling.
changed_files=["src/combo.c","src/conditional_layer.c","src/main.c"]+[f"src/behaviors/behavior_{kind}.c" for kind in ["hold_tap","macro","mod_morph","sticky_key","tap_dance"]]
if not enabled:
    for relative in changed_files:
        target=app/relative
        if "/* RUNTIME_EDITOR_PATCH_V1 */" in target.read_text(encoding="utf-8"):
            target.write_text(pristine(root,"app/"+relative),encoding="utf-8")
    cmake=app/"src/studio/CMakeLists.txt"
    if "runtime.c" in cmake.read_text(encoding="utf-8"):
        cmake.write_text(pristine(root,"app/src/studio/CMakeLists.txt"),encoding="utf-8")
    for relative in ["proto/zmk/keymap.proto","proto/zmk/keymap.options.in"]:
        target=messages/relative
        if "runtime" in target.read_text(encoding="utf-8"):
            target.write_text(pristine(messages,relative),encoding="utf-8")
    raise SystemExit(0)
head=subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip()
if head!="edf5c0814fd3ea202e43aad2d68fd32e882a518c":
    raise RuntimeError("Runtime source adapters require the pinned ZMK v0.3 commit")

def once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("Pinned source did not match: " + old[:100])
    return text.replace(old, new, 1)

header = '#include <zmk/runtime_editor.h>\n'
shutil.copyfile(here / "runtime.h", app / "include/zmk/runtime_editor.h")

proto = messages / "proto/zmk/keymap.proto"
p = pristine(messages,"proto/zmk/keymap.proto")
if "bytes runtime = 126" not in p:
    p = once(p, "oneof request_type {", "oneof request_type {\n        bytes runtime = 126;")
    p = once(p, "oneof response_type {", "oneof response_type {\n        bytes runtime = 126;")
    proto.write_text(p, encoding="utf-8")
options = messages / "proto/zmk/keymap.options.in"
o = pristine(messages,"proto/zmk/keymap.options.in")
if "Request.runtime" not in o:
    options.write_text(o + "\nzmk.keymap.Request.runtime max_size:512\nzmk.keymap.Response.runtime max_size:512\n", encoding="utf-8")

# Preserve the source document independently of its runtime representation.
source = (here.parent / "config/urchin.keymap").read_bytes()
if len(source) > 32768:
    raise RuntimeError("Default source document exceeds the 32 KiB limit")
generated = here / "generated_defaults.h"
generated.write_text("static const unsigned char rt_default_document[] = {" +
                     ",".join(str(b) for b in source) + "};\n", encoding="utf-8")

zephyr = root.parent / "zephyr"
sys.path.insert(0, str(zephyr / "scripts/dts/python-devicetree/src"))
with (binary / "zephyr/edt.pickle").open("rb") as handle:
    edt = pickle.load(handle)
entries = []
for node in edt.nodes:
    if "#binding-cells" not in node.props or not node.labels or node.status != "okay":
        continue
    for label in node.labels:
        entries.append("{%s,%s,DEVICE_DT_NAME(DT_NODELABEL(%s))}," %
                       (json.dumps(label), json.dumps(node.compats[0] if node.compats else ""), label))
with generated.open("a", encoding="utf-8") as handle:
    handle.write("struct rt_label {const char *label,*compatible,*device;};\n")
    handle.write("static const struct rt_label rt_labels[] = {\n" + "\n".join(entries) + "\n};\n")

def combo_patch(s):
    s = header + s
    s = re.sub(r"#define MAX_COMBO_KEYS sizeof\(union \{[^\n]+", "#define MAX_COMBO_KEYS RT_MAX_COMBO_KEYS", s)
    s = once(s, "static const struct combo_cfg combos[] = {", "static const struct combo_cfg stock_combos[] = {")
    s = once(s, "#define COMBO_CHILDREN_COUNT (0 DT_INST_FOREACH_CHILD(0, COMBO_ONE))", "#define COMBO_CHILDREN_COUNT RT_MAX_COMBOS")
    s = s.replace("ARRAY_SIZE(combos)", "runtime_combo_count")
    anchor = "#define COMBO_ONE(n) +1"
    s = once(s, anchor, "static struct combo_cfg combos[RT_MAX_COMBOS];\nstatic size_t runtime_combo_count;\n" + anchor)
    s = once(s, "static int combo_init(void) {", "static int combo_init(void) {\n    BUILD_ASSERT(ARRAY_SIZE(stock_combos) <= RT_MAX_COMBOS);\n    runtime_combo_count = ARRAY_SIZE(stock_combos);\n    memcpy(combos, stock_combos, sizeof(stock_combos));")
    addition = '''
int rt_set_combos(struct rt_reader *r) {
    unsigned count = rt_u16(r);
    if (count > RT_MAX_COMBOS) return -E2BIG;
    if (!r->dry) { memset(combos,0,sizeof(combos)); memset(combo_lookup,0,sizeof(combo_lookup)); }
    unsigned previous_len=0;
    for(unsigned i=0;i<count;i++) {
        struct combo_cfg c={0};
        c.layer_mask=rt_u32(r); c.timeout_ms=rt_u16(r); c.require_prior_idle_ms=rt_u16(r);
        c.slow_release=rt_u8(r); c.key_position_len=rt_u8(r);
        if(c.key_position_len<2 || c.key_position_len>RT_MAX_COMBO_KEYS || c.key_position_len<previous_len || !c.timeout_ms) return -EINVAL;
        previous_len=c.key_position_len;
        for(unsigned j=0;j<c.key_position_len;j++) {
            c.key_positions[j]=rt_u8(r);
            if(c.key_positions[j]>=ZMK_KEYMAP_LEN) return -EINVAL;
            for(unsigned k=0;k<j;k++) if(c.key_positions[j]==c.key_positions[k]) return -EINVAL;
        }
        c.behavior=rt_binding(r);
        if(r->error) return r->error;
        if(!r->dry) combos[i]=c;
    }
    if(!r->dry) {
        runtime_combo_count=count;
        for(unsigned i=0;i<count;i++) initialize_combo(i);
    }
    return r->error;
}
'''
    return s.rsplit("#endif", 1)[0] + addition + "\n#endif\n"
edit("src/combo.c", combo_patch)

def conditional_patch(s):
    s=header+s
    s=once(s,"static const struct conditional_layer_cfg CONDITIONAL_LAYER_CFGS[] = {", "static const struct conditional_layer_cfg STOCK_CONDITIONALS[] = {")
    s=once(s,"static const int32_t NUM_CONDITIONAL_LAYER_CFGS =\n    sizeof(CONDITIONAL_LAYER_CFGS) / sizeof(*CONDITIONAL_LAYER_CFGS);", "static struct conditional_layer_cfg runtime_conditionals[RT_MAX_CONDITIONALS];\nstatic const struct conditional_layer_cfg *CONDITIONAL_LAYER_CFGS=STOCK_CONDITIONALS;\nstatic int32_t NUM_CONDITIONAL_LAYER_CFGS=ARRAY_SIZE(STOCK_CONDITIONALS);")
    addition='''
int rt_set_conditionals(struct rt_reader *r) {
    unsigned count=rt_u8(r);
    if(count>RT_MAX_CONDITIONALS) return -E2BIG;
    uint32_t edges[ZMK_KEYMAP_LAYERS_LEN]={0};
    for(unsigned i=0;i<count;i++) {
        uint32_t mask=rt_u32(r); uint8_t target=rt_u8(r);
        if(target>=ZMK_KEYMAP_LAYERS_LEN || !mask || (mask & BIT(target)) || (mask >> ZMK_KEYMAP_LAYERS_LEN)) return -EINVAL;
        edges[target] |= mask;
        if(!r->dry) runtime_conditionals[i]=(struct conditional_layer_cfg){mask,target};
    }
    for(unsigned k=0;k<ZMK_KEYMAP_LAYERS_LEN;k++)for(unsigned i=0;i<ZMK_KEYMAP_LAYERS_LEN;i++)if(edges[i]&BIT(k))edges[i]|=edges[k];
    for(unsigned i=0;i<ZMK_KEYMAP_LAYERS_LEN;i++)if(edges[i]&BIT(i))return -ELOOP;
    if(!r->dry){CONDITIONAL_LAYER_CFGS=runtime_conditionals;NUM_CONDITIONAL_LAYER_CFGS=count;}
    return r->error;
}
'''
    return s.rsplit("#endif",1)[0]+addition+"\n#endif\n"
edit("src/conditional_layer.c",conditional_patch)
edit("src/main.c",lambda s: header+once(s,"    settings_load();","    settings_load();\n    rt_boot();"))

setters = {
"hold_tap": '''
    struct behavior_hold_tap_config base=*(const struct behavior_hold_tap_config *)dev->config;
    base.tapping_term_ms=rt_u16(r); base.quick_tap_ms=rt_u16(r); base.require_prior_idle_ms=rt_u16(r);
    base.flavor=rt_u8(r); unsigned flags=rt_u8(r);
    base.hold_while_undecided=!!(flags&1);base.hold_while_undecided_linger=!!(flags&2);
    base.retro_tap=!!(flags&4);base.hold_trigger_on_release=!!(flags&8);
    base.hold_behavior_dev=(char *)rt_behavior(r);base.tap_behavior_dev=(char *)rt_behavior(r);
    unsigned count=rt_u8(r);if(count>ZMK_KEYMAP_LEN || base.flavor>3 || !base.tapping_term_ms) return -EINVAL;
    size_t size=sizeof(base)+count*sizeof(int32_t);
    struct behavior_hold_tap_config *cfg=r->dry?NULL:rt_allocate(size);
    if(!r->dry && !cfg)return -ENOMEM;
    if(cfg){memcpy(cfg,&base,sizeof(base));cfg->hold_trigger_key_positions_len=count;}
    for(unsigned i=0;i<count;i++){unsigned position=rt_u8(r);if(position>=ZMK_KEYMAP_LEN)return -EINVAL;if(cfg)cfg->hold_trigger_key_positions[i]=position;}
''',
"mod_morph": '''
    struct behavior_mod_morph_config base={0};
    base.mods=rt_u8(r);unsigned keep=rt_u8(r);base.masked_mods=base.mods & ~keep;
    base.normal_binding=rt_binding(r);base.morph_binding=rt_binding(r);
    struct behavior_mod_morph_config *cfg=r->dry?NULL:rt_allocate(sizeof(base));
    if(!r->dry && !cfg)return -ENOMEM;if(cfg)*cfg=base;
''',
"sticky_key": '''
    struct behavior_sticky_key_config base={0};
    base.release_after_ms=rt_u16(r);unsigned flags=rt_u8(r);
    base.quick_release=!!(flags&1);base.lazy=!!(flags&2);base.ignore_modifiers=!!(flags&4);
    base.behavior=rt_binding(r);
    struct behavior_sticky_key_config *cfg=r->dry?NULL:rt_allocate(sizeof(base));
    if(!r->dry && !cfg)return -ENOMEM;if(cfg)*cfg=base;
''',
"tap_dance": '''
    unsigned term=rt_u16(r),count=rt_u8(r);
    if(!term || !count || count>32)return -EINVAL;
    struct behavior_tap_dance_config *cfg=r->dry?NULL:rt_allocate(sizeof(*cfg)+count*sizeof(struct zmk_behavior_binding));
    if(!r->dry && !cfg)return -ENOMEM;
    if(cfg){cfg->tapping_term_ms=term;cfg->behavior_count=count;cfg->behaviors=(void *)(cfg+1);}
    for(unsigned i=0;i<count;i++){struct zmk_behavior_binding b=rt_binding(r);if(cfg)cfg->behaviors[i]=b;}
''',
"macro": '''
    unsigned wait=rt_u16(r),tap=rt_u16(r),count=rt_u16(r);
    if(count>128)return -E2BIG;
    struct behavior_macro_config *cfg=r->dry?NULL:rt_allocate(sizeof(*cfg)+count*sizeof(struct zmk_behavior_binding));
    if(!r->dry && !cfg)return -ENOMEM;
    if(cfg){cfg->default_wait_ms=wait;cfg->default_tap_ms=tap;cfg->count=count;}
    for(unsigned i=0;i<count;i++){struct zmk_behavior_binding b=rt_binding(r);if(cfg)cfg->bindings[i]=b;}
'''
}
exports={
"hold_tap": '''
 rt_w16(w,cfg->tapping_term_ms);rt_w16(w,cfg->quick_tap_ms);rt_w16(w,cfg->require_prior_idle_ms);
 rt_w8(w,cfg->flavor);rt_w8(w,(cfg->hold_while_undecided?1:0)|(cfg->hold_while_undecided_linger?2:0)|(cfg->retro_tap?4:0)|(cfg->hold_trigger_on_release?8:0));
 rt_wbehavior(w,cfg->hold_behavior_dev);rt_wbehavior(w,cfg->tap_behavior_dev);
 rt_w8(w,cfg->hold_trigger_key_positions_len);
 for(unsigned i=0;i<cfg->hold_trigger_key_positions_len;i++)rt_w8(w,cfg->hold_trigger_key_positions[i]);
''',
"macro": '''
 rt_w16(w,cfg->default_wait_ms);rt_w16(w,cfg->default_tap_ms);rt_w16(w,cfg->count);
 for(unsigned i=0;i<cfg->count;i++)rt_wbinding(w,&cfg->bindings[i]);
''',
"mod_morph": '''
 rt_w8(w,cfg->mods);rt_w8(w,cfg->mods & ~cfg->masked_mods);
 rt_wbinding(w,&cfg->normal_binding);rt_wbinding(w,&cfg->morph_binding);
''',
"sticky_key": '''
 rt_w16(w,cfg->release_after_ms);rt_w8(w,(cfg->quick_release?1:0)|(cfg->lazy?2:0)|(cfg->ignore_modifiers?4:0));
 rt_wbinding(w,&cfg->behavior);
''',
"tap_dance": '''
 rt_w16(w,cfg->tapping_term_ms);rt_w8(w,cfg->behavior_count);
 for(unsigned i=0;i<cfg->behavior_count;i++)rt_wbinding(w,&cfg->behaviors[i]);
'''
}
for kind,body in setters.items():
    def family_patch(s,kind=kind,body=body):
        body=body.replace("r->dry?NULL:rt_allocate(","rt_reader_allocate(r,")
        s=header+s.replace("dev->config", "rt_config(dev)")
        function=f"\nint rt_set_{kind}(const struct device *dev,struct rt_reader *r) {{\n"
        function+=f"    if(dev->api!=&behavior_{kind}_driver_api)return -EINVAL;\n"
        function+=body+"\n    if(r->error)return r->error;\n    if(!r->dry) {int err=rt_override(dev,cfg);if(err)return err;"
        if kind=="macro": function+="behavior_macro_init(dev);"
        function+="}\n    return 0;\n}\n"
        function+=f"\nint rt_export_{kind}(const struct device *dev,struct rt_writer *w) {{\n"
        function+=f" if(dev->api!=&behavior_{kind}_driver_api)return -ENOTSUP;\n"
        function+=f" const struct behavior_{kind}_config *cfg=dev->config;\n"
        function+=exports[kind]+"\n return w->error;\n}\n"
        if kind=="macro":return s+function
        return s.rsplit("#endif",1)[0]+function+"\n#endif\n"
    edit(f"src/behaviors/behavior_{kind}.c",family_patch)

cmake=app / "src/studio/CMakeLists.txt"
c=pristine(root,"app/src/studio/CMakeLists.txt")
runtime_path=str(here / "runtime.c").replace("\\","/")
if runtime_path not in c:
    cmake.write_text(c+f'\ntarget_sources(app PRIVATE "{runtime_path}")\n',encoding="utf-8")
