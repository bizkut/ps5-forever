#include <string.h>
#include <stdint.h>
#include "psmkit-nest.h"

const char *nest_module_gate(const struct nest_module *module)
{
    /* The caller supplies its buffer size before the public query. Successful
     * queries overwrite that slot; it is not an output-layout certificate.
     * Validate the returned fields, not preservation of a request field. */
    if (!module->segment_count || module->segment_count > 4) return "MODULE_COUNT";
    if (!memchr(module->name, 0, sizeof(module->name))) return "MODULE_NAME_UNTERMINATED";
    if (strcmp(module->name, "libSceSystemService.sprx") &&
        strcmp(module->name, "libSceSystemService.prx")) return "MODULE_NAME";
    return NULL;
}
const char *nest_descriptor_gate(const struct nest_export *descriptor)
{
    if (descriptor->reserved) return "DESCRIPTOR_RESERVED";
    if (!descriptor->segment_count || descriptor->segment_count > 4) return "DESCRIPTOR_COUNT";
    if (descriptor->segment_index >= descriptor->segment_count) return "DESCRIPTOR_INDEX";
    if (!descriptor->segment_size) return "DESCRIPTOR_SIZE";
    if (descriptor->offset >= descriptor->segment_size) return "DESCRIPTOR_OFFSET";
    if (!descriptor->parent_base) return "DESCRIPTOR_BASE";
    if (descriptor->parent_base > UINTPTR_MAX - descriptor->segment_size) return "DESCRIPTOR_OVERFLOW";
    if (!(descriptor->segment_protection & NEST_EXEC)) return "DESCRIPTOR_NONEXEC";
    if (!memchr(descriptor->name, 0, sizeof(descriptor->name))) return "DESCRIPTOR_NAME_UNTERMINATED";
    if (strcmp(descriptor->name, "libSceSystemService.sprx") &&
        strcmp(descriptor->name, "libSceSystemService.prx")) return "DESCRIPTOR_NAME";
    return NULL;
}
static const char *segment_gate(const struct nest_segment *segment)
{
    uintptr_t base = (uintptr_t)segment->address;
    if (!base) return "SEGMENT_BASE";
    if (!segment->size) return "SEGMENT_SIZE";
    if (base > UINTPTR_MAX - segment->size) return "SEGMENT_OVERFLOW";
    return NULL;
}
int nest_export_capture(struct nest_export *descriptor,
                        const struct nest_module *module, uintptr_t address,
                        struct nest_validation *validation)
{
    struct nest_validation local = {0};
    if (!validation) validation = &local;
    *validation = (struct nest_validation){.segment = UINT32_MAX};
    memset(descriptor, 0, sizeof(*descriptor));
    if ((validation->reason = nest_module_gate(module))) return 0;
    if (!address) { validation->reason = "LINKED_ADDRESS_ZERO"; return 0; }
    unsigned index = 0;
    for (unsigned i = 0; i < module->segment_count; i++) {
        const struct nest_segment *segment = &module->segments[i];
        uintptr_t base = (uintptr_t)segment->address;
        if ((validation->reason = segment_gate(segment))) { validation->segment = i; return 0; }
        if ((segment->protection & NEST_EXEC) && address >= base && address - base < segment->size) {
            validation->matches++; index = i;
        }
    }
    if (validation->matches != 1) { validation->reason = "EXEC_CONTAINMENT_COUNT"; return 0; }
    const struct nest_segment *segment = &module->segments[index];
    descriptor->offset = address - (uintptr_t)segment->address;
    descriptor->parent_base = (uintptr_t)segment->address;
    descriptor->segment_count = module->segment_count;
    descriptor->segment_index = index;
    descriptor->segment_size = segment->size;
    descriptor->segment_protection = segment->protection;
    memcpy(descriptor->name, module->name, sizeof(descriptor->name));
    memcpy(descriptor->fingerprint, module->fingerprint, sizeof(descriptor->fingerprint));
    validation->segment = index;
    validation->reason = nest_descriptor_gate(descriptor);
    return !validation->reason;
}
int nest_export_resolve(const struct nest_export *descriptor,
                        const struct nest_module *module, uintptr_t *address,
                        struct nest_validation *validation)
{
    struct nest_validation local = {0};
    if (!validation) validation = &local;
    *validation = (struct nest_validation){.segment = UINT32_MAX};
    *address = 0;
    if ((validation->reason = nest_descriptor_gate(descriptor))) return 0;
    if ((validation->reason = nest_module_gate(module))) return 0;
    if (descriptor->segment_count != module->segment_count) { validation->reason = "COUNT_MISMATCH"; return 0; }
    if (strcmp(descriptor->name, module->name)) { validation->reason = "NAME_MISMATCH"; return 0; }
    if (memcmp(descriptor->fingerprint, module->fingerprint, sizeof(descriptor->fingerprint))) {
        validation->reason = "FINGERPRINT_MISMATCH"; return 0;
    }
    validation->segment = descriptor->segment_index;
    const struct nest_segment *segment = &module->segments[descriptor->segment_index];
    uintptr_t base = (uintptr_t)segment->address;
    if ((validation->reason = segment_gate(segment))) return 0;
    if (segment->size != descriptor->segment_size) { validation->reason = "SEGMENT_SIZE_MISMATCH"; return 0; }
    if (segment->protection != descriptor->segment_protection) { validation->reason = "SEGMENT_PROTECTION_MISMATCH"; return 0; }
    uintptr_t target = base + descriptor->offset;
    for (unsigned i = 0; i < module->segment_count; i++) {
        const struct nest_segment *other = &module->segments[i];
        uintptr_t other_base = (uintptr_t)other->address;
        if ((validation->reason = segment_gate(other))) { validation->segment = i; return 0; }
        if ((other->protection & NEST_EXEC) && target >= other_base && target - other_base < other->size) validation->matches++;
    }
    if (validation->matches != 1) { validation->reason = "EXEC_CONTAINMENT_COUNT"; return 0; }
    *address = target;
    return 1;
}
static void put32(uint8_t *wire, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) wire[i] = (uint8_t)(value >> (8 * i));
}
static uint32_t get32(const uint8_t *wire)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value |= (uint32_t)wire[i] << (8 * i);
    return value;
}
static void put64(uint8_t *wire, uint64_t value)
{
    put32(wire, (uint32_t)value); put32(wire + 4, (uint32_t)(value >> 32));
}
static uint64_t get64(const uint8_t *wire)
{
    return get32(wire) | ((uint64_t)get32(wire + 4) << 32);
}
void nest_frame_encode(uint8_t wire[NEST_FRAME_BYTES], const struct cap_frame *request,
                       const struct nest_export *descriptor)
{
    memset(wire, 0, NEST_FRAME_BYTES);
    put32(wire, NEST_FRAME_MAGIC); put32(wire + 4, NEST_FRAME_VERSION);
    put32(wire + 8, request->magic); put32(wire + 12, request->version);
    put32(wire + 16, request->kind); put32(wire + 20, request->index);
    put64(wire + 24, request->nonce); memcpy(wire + 32, request->cookie, 16);
    put32(wire + 48, request->parent_pid); put32(wire + 52, request->app_id);
    put32(wire + 56, request->pid); put32(wire + 60, (uint32_t)request->status);
    if (!descriptor) return;
    put64(wire + 64, descriptor->offset); put64(wire + 72, descriptor->parent_base);
    put32(wire + 80, descriptor->segment_count); put32(wire + 84, descriptor->segment_index);
    put32(wire + 88, descriptor->segment_size); put32(wire + 92, (uint32_t)descriptor->segment_protection);
    memcpy(wire + 96, descriptor->name, 256); memcpy(wire + 352, descriptor->fingerprint, 20);
    put32(wire + 372, descriptor->reserved);
    /* Last eight bytes are reserved and must stay zero. */
}
int nest_frame_decode(struct cap_frame *request, struct nest_export *descriptor,
                      const uint8_t wire[NEST_FRAME_BYTES])
{
    memset(request, 0, sizeof(*request)); memset(descriptor, 0, sizeof(*descriptor));
    if (get32(wire) != NEST_FRAME_MAGIC || get32(wire + 4) != NEST_FRAME_VERSION || get64(wire + 376)) return 0;
    request->magic = get32(wire + 8); request->version = get32(wire + 12);
    request->kind = get32(wire + 16); request->index = get32(wire + 20);
    request->nonce = get64(wire + 24); memcpy(request->cookie, wire + 32, 16);
    request->parent_pid = get32(wire + 48); request->app_id = get32(wire + 52);
    request->pid = get32(wire + 56); request->status = (int32_t)get32(wire + 60);
    descriptor->offset = get64(wire + 64); descriptor->parent_base = get64(wire + 72);
    descriptor->segment_count = get32(wire + 80); descriptor->segment_index = get32(wire + 84);
    descriptor->segment_size = get32(wire + 88); descriptor->segment_protection = (int32_t)get32(wire + 92);
    memcpy(descriptor->name, wire + 96, 256); memcpy(descriptor->fingerprint, wire + 352, 20);
    descriptor->reserved = get32(wire + 372);
    const struct nest_export empty = {0};
    return cap_request_valid(request) &&
           (!memcmp(descriptor, &empty, sizeof(empty)) || !nest_descriptor_gate(descriptor));
}
void nest_fingerprint(char text[41], const uint8_t fingerprint[20])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 20; i++) {
        text[2*i] = hex[fingerprint[i] >> 4]; text[2*i+1] = hex[fingerprint[i] & 15];
    }
    text[40] = 0;
}
