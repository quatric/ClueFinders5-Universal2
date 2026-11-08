// Resource Manager
#pragma once

#include "emu.h"

namespace res {

void init();
// A resource handle from the open resource files, or 0
gaddr get(uint32_t type, int16_t id);

} // namespace res
