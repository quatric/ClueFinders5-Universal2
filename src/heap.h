#pragma once

#include "emu.h"

namespace heap {

gaddr alloc(uint32_t size, bool clear = false);
void free(gaddr p);
uint32_t size(gaddr p);
gaddr realloc(gaddr p, uint32_t size);

// Memory Manager handles: a handle is the address of a master pointer
gaddr newHandle(uint32_t size, bool clear = false);
gaddr handleFromData(const void *data, uint32_t size);
void disposeHandle(gaddr h);
uint32_t handleSize(gaddr h);
bool setHandleSize(gaddr h, uint32_t size);
inline gaddr deref(gaddr h) { return emu::r32(h); }
bool isHandle(gaddr h);

extern int16_t memErr;

} // namespace heap
