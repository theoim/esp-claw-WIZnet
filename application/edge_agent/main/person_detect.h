#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true on success. Must be called after camera is open (PSRAM available). */
bool person_detect_init(void);

/* Returns person confidence 0-100, or -1 on inference error.
 * Input: raw YUYV buffer at width×height×2 bytes (e.g. 640×480 from OV3660). */
int  person_detect_run(const uint8_t *yuyv, int width, int height);

void person_detect_deinit(void);

#ifdef __cplusplus
}
#endif
