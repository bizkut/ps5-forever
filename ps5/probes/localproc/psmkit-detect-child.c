/* Firmware 5.50: supported module queries only; never call the resolved getter. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "psmkit-detect.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;
int sceKernelGetProcessType(pid_t pid);
int sceSysmoduleLoadModuleInternal(uint32_t module_id);
int sceSysmoduleIsLoadedInternal(uint32_t module_id);
int sceSysmoduleGetModuleHandleInternal(uint32_t module_id, int32_t *handle);

/* OpenOrbis public ModuleInfo, independently corroborated locally by
 * prospero-win/wine/ps5/pw_wine_prx.h. Not Ex/FromAddr or unwind info. */
struct module_segment
{
    void *address;
    uint32_t size;
    int32_t protection;
};
struct module_info
{
    size_t size;
    char name[256];
    struct module_segment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
_Static_assert(sizeof(struct module_segment) == 0x10, "module segment size");
_Static_assert(sizeof(struct module_info) == 0x160, "public module info size");
_Static_assert(offsetof(struct module_info, name) == 8, "public module name offset");
_Static_assert(offsetof(struct module_info, segments) == 0x108, "public segments offset");
_Static_assert(offsetof(struct module_info, segment_count) == 0x148, "public segment count offset");
_Static_assert(offsetof(struct module_info, fingerprint) == 0x14c, "public fingerprint offset");
int sceKernelGetModuleList(int32_t *handles, size_t capacity, size_t *count);
/* 5.50 0x33130 forwards the same three arguments to syscall 661, also
 * selected by GetModuleList for newer process SDKs. No private object ABI. */
int sceKernelGetModuleList2(int32_t *handles, size_t capacity, size_t *count);
int sceKernelGetModuleInfo(int32_t handle, struct module_info *info);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);

struct detection
{
    int32_t seen[1 + 2 * DETECT_CAPACITY];
    size_t count;
    int id_handle;
    int exact_name;
    int exact_symbol;
};

static void inspect_handle(struct detection *detection, int32_t handle,
                           const char *route, int by_id, int64_t deadline)
{
    for (size_t i = 0; i < detection->count; i++)
        if (detection->seen[i] == handle)
        {
            detect_log("psmkit-detect child info-skip route=%s handle=%#x reason=ALREADY_QUERIED\n",
                       route, (unsigned)handle);
            return;
        }
    if (detection->count >= sizeof(detection->seen) / sizeof(detection->seen[0]) ||
        !detect_within_deadline(deadline)) return;
    detection->seen[detection->count++] = handle;
    struct module_info info = {.size = sizeof(info)};
    errno = 0;
    int result = sceKernelGetModuleInfo(handle, &info);
    int error = errno;
    int terminated = memchr(info.name, 0, sizeof(info.name)) != NULL;
    detect_log("psmkit-detect child module-info route=%s handle=%#x result=%#x errno=%d size=%#zx name=%.*s terminated=%d\n",
               route, (unsigned)handle, (unsigned)result, error, info.size,
               (int)sizeof(info.name), info.name, terminated);
    int exact = !result && terminated &&
                (!strcmp(info.name, "libSceSystemService.prx") ||
                 !strcmp(info.name, "libSceSystemService.sprx"));
    if (exact) detection->exact_name = 1;
    if ((!by_id && !exact) || !detect_within_deadline(deadline)) return;
    void *address = NULL;
    errno = 0;
    result = sceKernelDlsym(handle, "sceSystemServiceGetParentSocketForPsmKit", &address);
    error = errno;
    detect_log("psmkit-detect child dlsym route=%s handle=%#x symbol=sceSystemServiceGetParentSocketForPsmKit result=%#x errno=%d address=%p called=0\n",
               route, (unsigned)handle, (unsigned)result, error, address);
    if (!result && address) detection->exact_symbol = 1;
    /* The pointer is logged only: no cast/call, dereference or fd assumption. */
}

static void inspect_list(struct detection *detection, int list2, int64_t deadline)
{
    if (!detect_within_deadline(deadline)) return;
    int32_t handles[DETECT_CAPACITY] = {0};
    size_t count = 0;
    const char *route = list2 ? "LIST2" : "LIST";
    errno = 0;
    int result = list2 ? sceKernelGetModuleList2(handles, DETECT_CAPACITY, &count) :
                        sceKernelGetModuleList(handles, DETECT_CAPACITY, &count);
    int error = errno;
    detect_log("psmkit-detect child module-list route=%s result=%#x errno=%d count=%zu capacity=%d\n",
               route, (unsigned)result, error, count, DETECT_CAPACITY);
    if (result || count > DETECT_CAPACITY)
    {
        detect_log("psmkit-detect child list-info-skip route=%s reason=ERROR_OR_TRUNCATED\n", route);
        return;
    }
    for (size_t i = 0; i < count && detect_within_deadline(deadline); i++)
    {
        detect_log("psmkit-detect child list-entry route=%s index=%zu handle=%#x\n",
                   route, i, (unsigned)handles[i]);
        inspect_handle(detection, handles[i], route, 0, deadline);
    }
}

int main(void)
{
    int64_t start = detect_now_ms();
    int64_t deadline = start + DETECT_OVERALL_MS;
    struct detection detection = {0};
    int load_ok = 0;
    detect_log("psmkit-detect child main pid=%d proc_type=%#x deadline_ms=%d handle_sweep=DISABLED\n",
               (int)getpid(), (unsigned)sceKernelGetProcessType(getpid()), DETECT_OVERALL_MS);
    if (start < 0 || !detect_within_deadline(deadline)) goto complete;
    errno = 0;
    int result = sceSysmoduleLoadModuleInternal(DETECT_MODULE_ID);
    int error = errno;
    detect_log("psmkit-detect child internal-load id=%#x result=%#x errno=%d calls=1\n",
               DETECT_MODULE_ID, (unsigned)result, error);
    load_ok = result == 0;
    if (!load_ok || !detect_within_deadline(deadline)) goto complete;
    int32_t handle = -1;
    errno = 0;
    result = sceSysmoduleGetModuleHandleInternal(DETECT_MODULE_ID, &handle);
    error = errno;
    detect_log("psmkit-detect child id-handle id=%#x result=%#x errno=%d handle=%#x calls=1\n",
               DETECT_MODULE_ID, (unsigned)result, error, (unsigned)handle);
    /* 0x1610 and 0x3bb0 return 0 iff this exact ID has a positive load
     * refcount, then write its 32-bit loader handle. This is authoritative
     * by-ID detection even when public module-info/dlsym cannot see it. */
    detection.id_handle = !result && handle >= 0;
    if (!detect_within_deadline(deadline)) goto complete;
    errno = 0;
    result = sceSysmoduleIsLoadedInternal(DETECT_MODULE_ID);
    error = errno;
    detect_log("psmkit-detect child id-loaded id=%#x result=%#x errno=%d loaded=%d calls=1\n",
               DETECT_MODULE_ID, (unsigned)result, error, result == 0);
    if (detection.id_handle)
        inspect_handle(&detection, handle, "ID_HANDLE", 1, deadline);
    inspect_list(&detection, 0, deadline);
    inspect_list(&detection, 1, deadline);
complete:
    {
        int in_time = start >= 0 && detect_within_deadline(deadline);
        int detected = load_ok && in_time &&
                       (detection.id_handle || detection.exact_name || detection.exact_symbol);
        detect_log("psmkit-detect child complete detection=%s id_handle=%d exact_name=%d exact_symbol=%d info_calls=%zu in_time=%d exit=%d module_cleanup=PROCESS_EXIT getter_calls=0\n",
                   detected ? "AUTHORITATIVE" : "INCONCLUSIVE", detection.id_handle,
                   detection.exact_name, detection.exact_symbol, detection.count,
                   in_time, detected ? 0 : 1);
        return detected ? 0 : 1;
    }
}
