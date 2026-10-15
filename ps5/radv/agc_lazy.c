/* The AGC functions RADV calls, loaded when it first calls one. Importing
 * libSceAgc and libSceAgcDriver gives a process a GPU address space and a turn
 * at the graphics pipe as soon as it starts (ps5/probes/agcload: a payload that
 * imports them and calls nothing is granted "GFX(pipe0) Game ... pid"), and a
 * process with a turn passes it on only at a suspend point. Every Wine process
 * that loaded libvulkan.so to read the GPU's name (explorer) then held the pipe
 * while the process that draws slept behind it (2026-10-03). Loaded at run time
 * (dlopen, which a payload's loader resolves as it resolves imports), the
 * modules reach only the processes that send the GPU work. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <ps5platform/agc.h>

/* What AGC returns when its modules could not be loaded. */
#define AGC_NOT_LOADED ((int32_t)0x80000001)

static struct
{
    int32_t (*init)(uint32_t);
    int32_t (*submit_dcb)(struct ps5_agc_submit_description *);
    int32_t (*suspend_point)(void);
    int32_t (*set_tf_ring)(uintptr_t, uint32_t);
    int32_t (*set_hs_offchip_param)(uint32_t, uint32_t);
} agc;
static pthread_once_t agc_once = PTHREAD_ONCE_INIT;

static void agc_load(void)
{
    void *base = dlopen("libSceAgc.sprx", RTLD_NOW), *driver = dlopen("libSceAgcDriver.sprx", RTLD_NOW);

    if (!base || !driver)
    {
        fprintf(stderr, "radv/ps5: cannot load AGC: %s\n", dlerror());
        return;
    }
    agc.init = (int32_t (*)(uint32_t))dlsym(base, "sceAgcInit");
    agc.suspend_point = (int32_t (*)(void))dlsym(base, "sceAgcSuspendPoint");
    agc.submit_dcb = (int32_t (*)(struct ps5_agc_submit_description *))dlsym(driver, "sceAgcDriverSubmitDcb");
    agc.set_tf_ring = (int32_t (*)(uintptr_t, uint32_t))dlsym(driver, "sceAgcDriverSetTFRing");
    agc.set_hs_offchip_param = (int32_t (*)(uint32_t, uint32_t))dlsym(driver, "sceAgcDriverSetHsOffchipParam");
    if (!agc.init || !agc.suspend_point || !agc.submit_dcb || !agc.set_tf_ring || !agc.set_hs_offchip_param)
        fprintf(stderr, "radv/ps5: AGC lacks a function RADV calls\n");
}

#define AGC_CALL(member, ...)                                                    \
    do {                                                                         \
        pthread_once(&agc_once, agc_load);                                       \
        return agc.member ? agc.member(__VA_ARGS__) : AGC_NOT_LOADED;            \
    } while (0)

int32_t sceAgcInit(uint32_t version) { AGC_CALL(init, version); }
int32_t sceAgcSuspendPoint(void) { AGC_CALL(suspend_point); }
int32_t sceAgcDriverSubmitDcb(struct ps5_agc_submit_description *description) { AGC_CALL(submit_dcb, description); }
int32_t sceAgcDriverSetTFRing(uintptr_t address, uint32_t size) { AGC_CALL(set_tf_ring, address, size); }
int32_t sceAgcDriverSetHsOffchipParam(uint32_t granularity, uint32_t buffering)
{
    AGC_CALL(set_hs_offchip_param, granularity, buffering);
}
