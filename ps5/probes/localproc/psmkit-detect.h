#ifndef PS5_LOCALPROC_PSMKIT_DETECT_H
#define PS5_LOCALPROC_PSMKIT_DETECT_H

#include <stdint.h>

#define DETECT_MODULE_ID UINT32_C(0x80000010)
#define DETECT_CAPACITY 256
#define DETECT_OVERALL_MS 5000
#define DETECT_PARENT_MS 20000

void detect_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
int64_t detect_now_ms(void);
int detect_within_deadline(int64_t deadline);

#endif
