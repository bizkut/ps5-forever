/* Asynchronous klog for B1 title processes (title-log.c).
 *
 * sceKernelDebugOutText costs the calling thread milliseconds of CPU per
 * line on the console (~8 ms for 300 bytes, ~26 ms for 600). B1 binds the
 * Unix modules' sceKernelDebugOutText import, and the native runtime's
 * io_log, to pw_title_klog_text: the line is copied into a bounded ring and
 * one native writer thread performs the klog write. A full ring drops the line
 * and counts it; the writer reports "wine: title log dropped=N total=M".
 *
 * Switch: WINEPS5_LOG_ASYNC=0|off at start, and live "log_async=on|off" in the
 * tuning file the writer polls (C:\wineps5-tune.txt). Default on. */
#ifndef PSMKIT_WINE_TITLE_LOG_H
#define PSMKIT_WINE_TITLE_LOG_H

/* Same contract as sceKernelDebugOutText; never blocks while asynchronous.
 * Async-signal-safe: lock-free, no allocation, preserves errno. */
int pw_title_klog_text(int channel, const char *text);

/* Starts the writer. Until then, and when it cannot start, lines are written
 * synchronously. tune_path may be NULL (no live switch). */
void pw_title_log_start(const char *tune_path);

/* Exit path: stops the writer, writes every queued line on the calling thread
 * and makes later lines synchronous. */
void pw_title_log_shutdown(void);

#endif
