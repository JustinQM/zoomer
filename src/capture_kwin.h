#ifndef CAPTURE_KWIN_H
#define CAPTURE_KWIN_H

#include <stdbool.h>
#include <stdint.h>

#include "capture.h"

bool capture_kwin_available(void);

void* capture_kwin_create(const OutputInfo* outputs, uint32_t output_count);

extern const CaptureImpl capture_kwin_impl;

#endif
