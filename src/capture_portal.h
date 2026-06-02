#ifndef CAPTURE_PORTAL_H
#define CAPTURE_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

#include "capture.h"

bool capture_portal_available(void);

void* capture_portal_create(const OutputInfo* outputs, uint32_t output_count);

extern const CaptureImpl capture_portal_impl;

#endif
