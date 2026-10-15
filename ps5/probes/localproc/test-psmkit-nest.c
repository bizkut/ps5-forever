/* HOST ONLY. Sizes/relative ranges are 5.50 ELF LOAD values, not measured
 * runtime page rounding. Bases and fingerprint are deliberately test fixtures.
 * 0x1280 is an offline export fixture only; runtime capture never uses it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "psmkit-nest.h"

static struct nest_module layout(uintptr_t base)
{
    struct nest_module module = {.size = sizeof(module), .segment_count = 4};
    strcpy(module.name, "libSceSystemService.sprx");
    module.segments[0] = (struct nest_segment){(void *)base, 0x2cc52, NEST_EXEC};
    module.segments[1] = (struct nest_segment){(void *)(base + 0x30000), 0x10a5d, 1};
    module.segments[2] = (struct nest_segment){(void *)(base + 0x44000), 0x7c0, 3};
    module.segments[3] = (struct nest_segment){(void *)(base + 0x48000), 0x11638, 3};
    for (unsigned i = 0; i < 20; i++) module.fingerprint[i] = (uint8_t)(i * 13 + 7);
    return module;
}
static void rejected(const struct nest_export *descriptor, const struct nest_module *module)
{
    uintptr_t target = UINTPTR_MAX;
    assert(!nest_export_resolve(descriptor, module, &target, NULL)); assert(target == 0);
}
static void diagnostic_reasons(void)
{
    struct nest_module module = layout(UINT64_C(0x200000000)), changed;
    struct nest_export descriptor, bad;
    struct nest_validation validation;
    uintptr_t target;
    assert(nest_export_capture(&descriptor, &module, UINT64_C(0x200001280), &validation));
    assert(!validation.reason && validation.matches == 1 && validation.segment == 0);
    changed = module; changed.size = 0;
    assert(!nest_module_gate(&changed));
    assert(nest_export_capture(&bad, &changed, UINT64_C(0x200001280), &validation));
    changed.size = UINT64_MAX;
    assert(nest_export_capture(&bad, &changed, UINT64_C(0x200001280), &validation));
    changed = module; changed.segment_count = 0;
    assert(!strcmp(nest_module_gate(&changed), "MODULE_COUNT"));
    changed = module; memset(changed.name, 'x', sizeof(changed.name));
    assert(!strcmp(nest_module_gate(&changed), "MODULE_NAME_UNTERMINATED"));
    changed = module; strcpy(changed.name, "another.prx");
    assert(!strcmp(nest_module_gate(&changed), "MODULE_NAME"));
#define DESC_REASON(field, value, reason) do { bad = descriptor; bad.field = (value); assert(!strcmp(nest_descriptor_gate(&bad), (reason))); } while (0)
    DESC_REASON(reserved, 1, "DESCRIPTOR_RESERVED");
    DESC_REASON(segment_count, 0, "DESCRIPTOR_COUNT");
    DESC_REASON(segment_index, 4, "DESCRIPTOR_INDEX");
    DESC_REASON(segment_size, 0, "DESCRIPTOR_SIZE");
    DESC_REASON(offset, descriptor.segment_size, "DESCRIPTOR_OFFSET");
    DESC_REASON(parent_base, 0, "DESCRIPTOR_BASE");
    DESC_REASON(parent_base, UINT64_MAX - 1, "DESCRIPTOR_OVERFLOW");
    DESC_REASON(segment_protection, 1, "DESCRIPTOR_NONEXEC");
#undef DESC_REASON
    bad = descriptor; memset(bad.name, 'x', sizeof(bad.name));
    assert(!strcmp(nest_descriptor_gate(&bad), "DESCRIPTOR_NAME_UNTERMINATED"));
    bad = descriptor; strcpy(bad.name, "another.prx");
    assert(!strcmp(nest_descriptor_gate(&bad), "DESCRIPTOR_NAME"));
    assert(!nest_export_capture(&bad, &module, 0, &validation));
    assert(!strcmp(validation.reason, "LINKED_ADDRESS_ZERO"));
    assert(!nest_export_capture(&bad, &module, UINT64_C(0x400000), &validation));
    assert(!strcmp(validation.reason, "EXEC_CONTAINMENT_COUNT") && validation.matches == 0);
    changed = module; changed.segments[1] = changed.segments[0];
    assert(!nest_export_capture(&bad, &changed, UINT64_C(0x200001280), &validation));
    assert(!strcmp(validation.reason, "EXEC_CONTAINMENT_COUNT") && validation.matches == 2);
    changed = module; changed.segments[2].address = NULL;
    assert(!nest_export_capture(&bad, &changed, UINT64_C(0x200001280), &validation));
    assert(!strcmp(validation.reason, "SEGMENT_BASE") && validation.segment == 2);
    changed = module; changed.segments[2].size = 0;
    assert(!nest_export_capture(&bad, &changed, UINT64_C(0x200001280), &validation));
    assert(!strcmp(validation.reason, "SEGMENT_SIZE") && validation.segment == 2);
    changed = module; changed.segments[2].address = (void *)(UINTPTR_MAX - 1);
    assert(!nest_export_capture(&bad, &changed, UINT64_C(0x200001280), &validation));
    assert(!strcmp(validation.reason, "SEGMENT_OVERFLOW") && validation.segment == 2);
#define RESOLVE_REASON(change, expected) do { changed = module; change; assert(!nest_export_resolve(&descriptor, &changed, &target, &validation)); assert(!strcmp(validation.reason, (expected)) && target == 0); } while (0)
    RESOLVE_REASON(changed.segment_count = 3, "COUNT_MISMATCH");
    RESOLVE_REASON(strcpy(changed.name, "libSceSystemService.prx"), "NAME_MISMATCH");
    RESOLVE_REASON(changed.fingerprint[0] ^= 1, "FINGERPRINT_MISMATCH");
    RESOLVE_REASON(changed.segments[0].size++, "SEGMENT_SIZE_MISMATCH");
    RESOLVE_REASON(changed.segments[0].protection = 1, "SEGMENT_PROTECTION_MISMATCH");
    RESOLVE_REASON(changed.segments[1] = changed.segments[0], "EXEC_CONTAINMENT_COUNT");
#undef RESOLVE_REASON
}
static void runtime_layout_repro(void)
{
    /* Additional diagnostic launch: actual public output, not ELF memsz.
     * Child addresses are relocated fixtures; no child query was made yet. */
    const uintptr_t parent_base = UINT64_C(0x800b54000);
    const uintptr_t linked = UINT64_C(0x800b55280);
    struct nest_module parent = layout(parent_base);
    parent.size = 0;
    parent.segments[0].size = 0x30000;
    parent.segments[1].size = 0x14000;
    parent.segments[2].size = 0x4000;
    parent.segments[2].protection = 1;
    parent.segments[3].size = 0x14000;
    const uint8_t fingerprint[20] = {0x1e,0x18,0x7e,0x42,0xd8,0x57,0x35,0x97,
        0x33,0x4a,0x15,0x83,0x68,0xcd,0x10,0x1c,0,0,0,0};
    memcpy(parent.fingerprint, fingerprint, sizeof(fingerprint));
    struct nest_module child = parent;
    const uintptr_t child_base = UINT64_C(0x900000000);
    for (unsigned i = 0; i < 4; i++)
        child.segments[i].address = (void *)(child_base + (uintptr_t)parent.segments[i].address - parent_base);
    struct nest_export descriptor, decoded;
    struct nest_validation validation;
    struct cap_frame request, echo;
    uint8_t wire[NEST_FRAME_BYTES];
    uintptr_t target;
    assert(nest_export_capture(&descriptor, &parent, linked, &validation));
    assert(!validation.reason && validation.matches == 1);
    assert(descriptor.parent_base == parent_base && descriptor.offset == linked - parent_base);
    cap_request(&request, 0, 1234, 1860, 0x2016);
    nest_frame_encode(wire, &request, &descriptor);
    assert(nest_frame_decode(&echo, &decoded, wire));
    assert(nest_export_resolve(&decoded, &child, &target, &validation));
    assert(target == child_base + (linked - parent_base));
    child.fingerprint[19] = 1; rejected(&decoded, &child);
    child.fingerprint[19] = 0; child.segments[0].size++; rejected(&decoded, &child);
    child.segments[0].size--; child.segments[0].protection = 1; rejected(&decoded, &child);
    child.segments[0].protection = NEST_EXEC;
    decoded.offset = decoded.segment_size; rejected(&decoded, &child);
}
int main(void)
{
    const uintptr_t parent_base = UINT64_C(0x200000000), child_base = UINT64_C(0x240000000);
    struct nest_module parent = layout(parent_base), child = layout(child_base), changed;
    struct nest_export descriptor, decoded, bad;
    struct cap_frame request, echo;
    uintptr_t target = 0;
    uint8_t wire[NEST_FRAME_BYTES], canonical[NEST_FRAME_BYTES];
    assert(nest_export_capture(&descriptor, &parent, parent_base + 0x1280, NULL));
    assert(descriptor.offset == 0x1280 && descriptor.parent_base == parent_base &&
           descriptor.segment_size == 0x2cc52 && descriptor.segment_index == 0);
    assert(nest_export_resolve(&descriptor, &child, &target, NULL)); assert(target == child_base + 0x1280);
    cap_request(&request, 0, UINT64_C(0x0123456789abcdef), 1855, 0xe016);
    nest_frame_encode(wire, &request, &descriptor); memcpy(canonical, wire, sizeof(wire));
    assert(wire[0] == 'T' && wire[1] == 'S' && wire[2] == 'E' && wire[3] == 'N');
    assert(wire[4] == 2 && wire[5] == 0 && wire[64] == 0x80 && wire[65] == 0x12);
    assert(nest_frame_decode(&echo, &decoded, wire));
    assert(!memcmp(&echo, &request, sizeof(request))); assert(!memcmp(&decoded, &descriptor, sizeof(descriptor)));
    assert(nest_export_resolve(&decoded, &child, &target, NULL) && target == child_base + 0x1280);
    nest_frame_encode(wire, &echo, &decoded); assert(!memcmp(wire, canonical, sizeof(wire)));
    char fingerprint[41]; nest_fingerprint(fingerprint, descriptor.fingerprint);
    assert(!strcmp(fingerprint, "0714212e3b4855626f7c8996a3b0bdcad7e4f1fe"));
    changed = child; changed.fingerprint[19] ^= 1; rejected(&descriptor, &changed);
    changed = child; changed.segments[0].size++; rejected(&descriptor, &changed);
    changed = child; changed.segments[0].protection = 1; rejected(&descriptor, &changed);
    changed = child; changed.segment_count = 3; rejected(&descriptor, &changed);
    changed = child; strcpy(changed.name, "libSceSystemService.prx"); rejected(&descriptor, &changed);
    changed = child; memset(changed.name, 'x', sizeof(changed.name)); rejected(&descriptor, &changed);
    changed = child; changed.size = 0;
    assert(nest_export_resolve(&descriptor, &changed, &target, NULL));
    changed = child; changed.segments[0].address = (void *)(UINTPTR_MAX - 0x100); rejected(&descriptor, &changed);
    changed = child; changed.segments[1] = changed.segments[0]; rejected(&descriptor, &changed);
    bad = descriptor; bad.offset = bad.segment_size; rejected(&bad, &child);
    bad = descriptor; bad.offset = UINT64_MAX; rejected(&bad, &child);
    bad = descriptor; bad.segment_index = 4; rejected(&bad, &child);
    bad = descriptor; bad.segment_count = 5; rejected(&bad, &child);
    bad = descriptor; bad.segment_protection = 1; rejected(&bad, &child);
    bad = descriptor; bad.reserved = 1; rejected(&bad, &child);
    assert(!nest_export_capture(&bad, &parent, parent_base + 0x30000, NULL)); /* non-executable */
    assert(!nest_export_capture(&bad, &parent, parent_base + 0x2cc52, NULL)); /* end exclusive */
    assert(!nest_export_capture(&bad, &parent, UINT64_C(0x400000), NULL)); /* title PLT/stub */
    changed = parent; changed.segments[1] = changed.segments[0];
    assert(!nest_export_capture(&bad, &changed, parent_base + 0x1280, NULL)); /* ambiguous containment */
    changed = parent; changed.segment_count = 5;
    assert(!nest_export_capture(&bad, &changed, parent_base + 0x1280, NULL));
    memcpy(wire, canonical, sizeof(wire)); wire[4] = 1; assert(!nest_frame_decode(&echo, &decoded, wire));
    memcpy(wire, canonical, sizeof(wire)); wire[0] ^= 1; assert(!nest_frame_decode(&echo, &decoded, wire));
    memcpy(wire, canonical, sizeof(wire)); wire[376] = 1; assert(!nest_frame_decode(&echo, &decoded, wire));
    memcpy(wire, canonical, sizeof(wire)); wire[32] ^= 1; assert(!nest_frame_decode(&echo, &decoded, wire));
    memcpy(wire, canonical, sizeof(wire)); wire[352] ^= 1;
    assert(nest_frame_decode(&echo, &decoded, wire)); rejected(&decoded, &child);
    bad = descriptor; bad.segment_size++; nest_frame_encode(wire, &request, &bad);
    assert(nest_frame_decode(&echo, &decoded, wire)); rejected(&decoded, &child);
    bad = descriptor; bad.offset = bad.segment_size; nest_frame_encode(wire, &request, &bad);
    assert(!nest_frame_decode(&echo, &decoded, wire));
    bad = descriptor; bad.segment_protection = 1; nest_frame_encode(wire, &request, &bad);
    assert(!nest_frame_decode(&echo, &decoded, wire));
    nest_frame_encode(wire, &request, NULL); assert(nest_frame_decode(&echo, &decoded, wire));
    rejected(&decoded, &child); /* absent descriptor cannot resolve */
    diagnostic_reasons();
    runtime_layout_repro();
    puts("psmkit-nest host PASS: v2 LE frame; 5.50 ELF and observed runtime layouts; request-size overwrite; exact gates and fingerprint/size/offset/exec/name/count/overflow/overlap/stub/version/cookie/reserved rejects (no console)");
    return 0;
}
