/* Local-only transcription. Audio lives in memory until this hold ends;
 * diagnostics never include audio, token IDs, model output or exception text. */
#include "voice.h"
#include "voice-model.h"
#include "whisper.h"
#include "ggml-backend.h"
#include "ggml-vulkan.h"
#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <vector>

extern "C" int sceKernelDebugOutText(int, const char *);
extern "C" int scePthreadSetprio(pthread_t, int);
extern "C" int sceAudioInInit(void);
extern "C" int sceAudioInOpen(int, unsigned, unsigned, unsigned, unsigned, unsigned);
extern "C" int sceAudioInInput(int, void *);
extern "C" int sceAudioInClose(int);

namespace {
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
pthread_t recorder, decoder;
bool threads, stopping, requested, active, finishing, capture_closed = true, nonzero;
bool capture_error, output_ready, reported_load_error;
std::atomic<bool> selected_vulkan{false};
bool capture_pending, audio_initialized;
int pending_handle = -1, audio_init_status;
unsigned pending_generation;
unsigned generation;
std::vector<float> audio;
pw_voice_result output;

uint64_t now_us()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return uint64_t(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
}
void note(const char *event, unsigned id, size_t samples, int status, size_t units, uint64_t us)
{
    char line[256];
    std::snprintf(line, sizeof(line), "voice event=%s session=%u samples=%zu status=%d text_units=%zu elapsed_ms=%llu\n",
        event, id, samples, status, units, static_cast<unsigned long long>(us / 1000));
    sceKernelDebugOutText(0, line);
}
/* Whisper and GGML messages may contain decoded text. Do not forward them,
 * including library errors or exception what() strings, into the title log. */
void quiet_log(ggml_log_level, const char *text, void *)
{
    if (std::strstr(text, "whisper_backend_init_gpu: using Vulkan"))
        selected_vulkan.store(true, std::memory_order_relaxed);
}

void *record(void *)
{
    scePthreadSetprio(pthread_self(), 767);
    pthread_mutex_lock(&lock);
    while (!stopping)
    {
        while (!capture_pending && !stopping) pthread_cond_wait(&changed, &lock);
        if (stopping) break;
        const unsigned id = pending_generation;
        const int handle = pending_handle;
        capture_pending = false;
        pending_handle = -1;
        pthread_mutex_unlock(&lock);
        bool failed = handle < 0;
        while (!failed)
        {
            pthread_mutex_lock(&lock);
            const bool keep = !stopping && generation == id && requested;
            pthread_mutex_unlock(&lock);
            if (!keep) break;
            alignas(64) int16_t block[256];
            const int rc = sceAudioInInput(handle, block);
            pthread_mutex_lock(&lock);
            if (generation == id)
            {
                if (rc < 0) { capture_error = true; failed = true; }
                else
                {
                    nonzero |= voice_pcm_nonzero(block, 256) != 0;
                    try
                    {
                        const size_t at = audio.size();
                        audio.resize(at + 256);
                        for (unsigned i = 0; i < 256; i++) audio[at + i] = block[i] / 32768.0f;
                    }
                    catch (...) { capture_error = true; failed = true; }
                }
            }
            pthread_mutex_unlock(&lock);
        }
        if (handle >= 0) sceAudioInClose(handle);
        pthread_mutex_lock(&lock);
        if (generation == id)
        {
            capture_closed = true;
            capture_error |= failed;
            requested = false;
            pthread_cond_broadcast(&changed);
        }
    }
    const int unused_handle = capture_pending ? pending_handle : -1;
    capture_pending = false;
    pending_handle = -1;
    pthread_mutex_unlock(&lock);
    if (unused_handle >= 0) sceAudioInClose(unused_handle);
    return nullptr;
}

whisper_context *load_model(unsigned id)
{
    struct stat st{};
    if (stat(FOREVER_VOICE_MODEL_PATH, &st) || static_cast<uint64_t>(st.st_size) != FOREVER_VOICE_MODEL_BYTES)
    {
        if (!reported_load_error) { note("model_missing", id, 0, -1, 0, 0); reported_load_error = true; }
        return nullptr;
    }
    const uint64_t start = now_us();
    whisper_context *ctx = nullptr;
    try
    {
        ggml_backend_reg_t vk = ggml_backend_vk_reg();
        if (vk && ggml_backend_reg_dev_count(vk))
        {
            selected_vulkan.store(false, std::memory_order_relaxed);
            auto params = whisper_context_default_params();
            params.use_gpu = true;
            params.gpu_device = 0;
            ctx = whisper_init_from_file_with_params(FOREVER_VOICE_MODEL_PATH, params);
            if (ctx && !selected_vulkan.load(std::memory_order_relaxed)) { whisper_free(ctx); ctx = nullptr; }
        }
    }
    catch (...) { ctx = nullptr; }
    if (ctx) note("model_loaded", id, 0, 0, 0, now_us() - start);
    else if (!reported_load_error) { note("model_failed", id, 0, -1, 0, now_us() - start); reported_load_error = true; }
    return ctx;
}

bool transcribe(whisper_context *ctx, const std::vector<float> &pcm, bool final, pw_voice_result &result)
{
    auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = 2;
    params.language = "auto";
    params.detect_language = false;
    params.translate = false;
    params.audio_ctx = voice_audio_ctx(pcm.size());
    params.max_tokens = voice_max_tokens(pcm.size());
    params.no_context = true;
    params.single_segment = true;
    params.no_timestamps = true;
    params.print_special = params.print_progress = params.print_realtime = params.print_timestamps = false;
    params.temperature = params.temperature_inc = 0;
    params.greedy.best_of = 1;
    if (pcm.size() > INT32_MAX || whisper_full(ctx, params, pcm.data(), static_cast<int>(pcm.size()))) return false;
    size_t used = 0;
    for (int i = 0; i < whisper_full_n_segments(ctx); i++)
    {
        const char *text = whisper_full_get_segment_text(ctx, i);
        while (*text && used + 1 < sizeof(result.text))
        {
            result.text[used++] = *text == '\n' || *text == '\r' ? ' ' : *text;
            text++;
        }
    }
    result.text[used] = 0;
    if (final) voice_trim_repetition(result.text);
    /* Bound the text by the inline IME's capacity and finish on a complete
     * UTF-8 codepoint. This same normalized text is retained for corrections. */
    uint16_t text[VOICE_TEXT_UNITS];
    unsigned units = voice_utf16(result.text, text, VOICE_TEXT_UNITS - 8);
    voice_utf8(text, units, result.text, sizeof(result.text));
    return true;
}
void *decode(void *)
{
    scePthreadSetprio(pthread_self(), 767);
    whisper_log_set(quiet_log, nullptr);
    ggml_log_set(quiet_log, nullptr);
    whisper_context *ctx = nullptr;
    std::vector<float> snapshot;
    unsigned loaded_attempt = 0;
    uint64_t next = 0;
    pthread_mutex_lock(&lock);
    while (!stopping)
    {
        const uint64_t now = now_us();
        bool final = finishing && capture_closed;
        if (!active || (finishing && !capture_closed) || (!final && now < next))
        {
            timespec until{};
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 50000000;
            if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&changed, &lock, &until);
            continue;
        }
        const unsigned id = generation;
        /* Loading and inference run unlocked: the independent recorder keeps
         * the pre-roll and every block arriving during a cold model load. */
        if (!ctx && loaded_attempt != id && !capture_error)
        {
            loaded_attempt = id;
            pthread_mutex_unlock(&lock);
            ctx = load_model(id);
            pthread_mutex_lock(&lock);
            if (stopping || generation != id || !active) continue;
            final = finishing && capture_closed;
        }
        if (finishing && !capture_closed) continue;
        pw_voice_result result{};
        result.generation = id;
        result.final = final;
        result.failed = capture_error || !ctx;
        result.muted = !nonzero;
        if (!final && !result.failed && result.muted) { next = now + 700000; continue; }
        size_t samples = audio.size();
        if (!result.failed && !result.muted)
        {
            try { snapshot.assign(audio.begin(), audio.end()); }
            catch (...) { result.failed = true; }
        }
        pthread_mutex_unlock(&lock);
        uint64_t start = now_us();
        if (!result.failed && !result.muted)
        {
            try { result.failed = !transcribe(ctx, snapshot, final, result); }
            catch (...) { result.failed = true; }
        }
        uint16_t units[VOICE_TEXT_UNITS];
        unsigned length = voice_utf16(result.text, units, VOICE_TEXT_UNITS);
        note(final ? "final" : "partial", id, samples, result.failed ? -1 : result.muted ? 1 : 0, length, now_us() - start);
        pthread_mutex_lock(&lock);
        if (generation == id && active)
        {
            output = result;
            output_ready = true;
            if (final || result.failed)
            {
                output.final = true;
                active = finishing = requested = false;
                /* Do not free the vector while a recorder appends. Capacity
                 * is reused; samples are overwritten/cleared at next press. */
                if (capture_closed) audio.clear();
                pthread_cond_broadcast(&changed);
            }
        }
        next = start + 700000;
    }
    audio.clear();
    pthread_mutex_unlock(&lock);
    if (ctx) whisper_free(ctx);
    return nullptr;
}
}

/* GGML's optional debugger and libc++ path discovery must fail closed in a
 * title, never issue desktop syscalls or spawn another executable. */
extern "C" pid_t forever_voice_fork(void) { errno = ENOSYS; return -1; }
extern "C" long forever_voice_pathconf(const char *, int) { errno = ENOSYS; return -1; }

extern "C" void pw_voice_capture(int user)
{
    pthread_mutex_lock(&lock);
    if (stopping) { pthread_mutex_unlock(&lock); return; }
    ++generation;
    if (!generation) ++generation;
    audio.clear();
    nonzero = capture_error = output_ready = active = finishing = false;
    try { if (audio.capacity() < 480000) audio.reserve(480000); }
    catch (...) { capture_error = true; }
    requested = true;
    capture_closed = false;
    if (!threads)
    {
        int rc = pthread_create(&recorder, nullptr, record, nullptr);
        if (!rc)
        {
            rc = pthread_create(&decoder, nullptr, decode, nullptr);
            if (!rc) threads = true;
            else
            {
                stopping = true;
                requested = false;
                pthread_cond_broadcast(&changed);
                pthread_mutex_unlock(&lock);
                pthread_join(recorder, nullptr);
                pthread_mutex_lock(&lock);
                stopping = false;
            }
        }
        if (rc) { requested = false; capture_closed = true; capture_error = true; }
    }
    if (threads)
    {
        /* Open on the physical press edge, not after the 400-ms classifier
         * or cold model load. The recorder exclusively owns it after wake. */
        if (capture_pending && pending_handle >= 0) sceAudioInClose(pending_handle);
        if (!audio_initialized) { audio_init_status = sceAudioInInit(); audio_initialized = true; }
        pending_handle = audio_init_status < 0 ? audio_init_status : sceAudioInOpen(user, 1, 0, 256, 16000, 0);
        pending_generation = generation;
        capture_pending = true;
    }
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}
extern "C" void pw_voice_start(void)
{
    pthread_mutex_lock(&lock);
    active = true;
    const unsigned id = generation;
    if (!threads)
    {
        output = {};
        output.generation = generation;
        output.final = output.failed = 1;
        output_ready = true;
        active = false;
    }
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    note("start", id, 0, 0, 0, 0);
}
extern "C" void pw_voice_finish(void)
{
    pthread_mutex_lock(&lock);
    requested = false;
    finishing = active;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}
extern "C" void pw_voice_discard(void)
{
    pthread_mutex_lock(&lock);
    ++generation;
    requested = active = finishing = output_ready = false;
    audio.clear();
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}
extern "C" int pw_voice_poll(pw_voice_result *result)
{
    int ready = 0;
    if (pthread_mutex_trylock(&lock)) return 0;
    if (output_ready) { *result = output; output_ready = false; ready = 1; }
    pthread_mutex_unlock(&lock);
    return ready;
}
extern "C" void pw_voice_shutdown(void)
{
    pthread_mutex_lock(&lock);
    stopping = true;
    requested = false;
    pthread_cond_broadcast(&changed);
    bool join = threads;
    pthread_mutex_unlock(&lock);
    if (join) { pthread_join(recorder, nullptr); pthread_join(decoder, nullptr); }
    pthread_mutex_lock(&lock);
    threads = false;
    std::vector<float>().swap(audio);
    pthread_mutex_unlock(&lock);
}
