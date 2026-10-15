#ifndef PS5_PSMKIT_NEST_H
#define PS5_PSMKIT_NEST_H
#include "psmkit-capacity.h"
#include "probe.h"
#define NEST_MS 30000
#define NEST_WORK_MS 22000
#define NEST_CHILDREN 7
#define NEST_GRANDCHILDREN 8
#define NEST_SOCKET 3
#define NEST_FRAME_MAGIC UINT32_C(0x4e455354)
#define NEST_FRAME_VERSION 2
#define NEST_FRAME_BYTES 384
#define NEST_MODULE_ID UINT32_C(0x80000010)
#define NEST_EXEC 4 /* Public ModuleInfo uses VM_PROT_EXECUTE, not ELF PF_X. */
struct nest_segment { void *address; uint32_t size; int32_t protection; };
struct nest_module {
    size_t size;
    char name[256];
    struct nest_segment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
_Static_assert(sizeof(struct nest_segment) == 0x10, "public segment ABI");
_Static_assert(sizeof(struct nest_module) == 0x160, "public ModuleInfo ABI");
_Static_assert(offsetof(struct nest_module, segments) == 0x108, "segments ABI");
_Static_assert(offsetof(struct nest_module, fingerprint) == 0x14c, "fingerprint ABI");
struct nest_export {
    uint64_t offset, parent_base;
    uint32_t segment_count, segment_index, segment_size;
    int32_t segment_protection;
    char name[256];
    uint8_t fingerprint[20];
    uint32_t reserved;
};
_Static_assert(sizeof(struct nest_export) == 312, "export descriptor size");
typedef int32_t (*nest_launch_fn)(const char *, const char *const [],
                                 const struct local_process_param *);
int sceSysmoduleLoadModuleInternal(uint32_t module_id);
int sceSysmoduleGetModuleHandleInternal(uint32_t module_id, int32_t *handle);
int sceKernelGetModuleInfo(int32_t handle, struct nest_module *info);
/* Firmware-verified ABI: address, text/data flags (1/2), 0x1a8-byte output.
 * Diagnostic-only raw buffer; no guessed extended-structure field layout. */
int sceKernelGetModuleInfoFromAddr(const void *address, int32_t flags, void *info);
struct nest_validation { const char *reason; unsigned segment, matches; };
const char *nest_module_gate(const struct nest_module *module);
const char *nest_descriptor_gate(const struct nest_export *descriptor);
void nest_log_module(const char *role, const struct nest_module *module);
void nest_log_hex(const char *role, const void *bytes, size_t size);
int nest_export_capture(struct nest_export *descriptor,
                        const struct nest_module *module, uintptr_t address,
                        struct nest_validation *validation);
int nest_export_resolve(const struct nest_export *descriptor,
                        const struct nest_module *module, uintptr_t *address,
                        struct nest_validation *validation);
void nest_frame_encode(uint8_t wire[NEST_FRAME_BYTES], const struct cap_frame *request,
                       const struct nest_export *descriptor);
int nest_frame_decode(struct cap_frame *request, struct nest_export *descriptor,
                      const uint8_t wire[NEST_FRAME_BYTES]);
void nest_fingerprint(char text[41], const uint8_t fingerprint[20]);
enum nest_kind { NEST_A = 10, NEST_B, NEST_RELEASE, NEST_REPORT };
struct nest_report {
    struct cap_frame echo;
    uint32_t calls, ready, peak;
    int32_t status, error, cleanup;
};
struct nest_slot { struct cap_slot cap; int queue, called; };
void nest_init(struct nest_slot *slot);
int nest_add(struct nest_slot *slot, const char *path, const char *phase,
             unsigned index, uint64_t seed, uint32_t app, int64_t deadline,
             nest_launch_fn launch, const struct nest_export *descriptor);
int nest_finish(struct nest_slot *slot, const char *phase, int64_t deadline);
int nest_wait(int fd, int64_t deadline);
int nest_command(int fd, const struct cap_frame *request, unsigned kind,
                 struct nest_report *report, int64_t deadline);
#endif
