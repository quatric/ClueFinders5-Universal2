// Text helpers shared with the Control Manager
#pragma once

#include "qd.h"

#include <string>

namespace txt {

void drawLabel(qd::Port *p, const std::string &utf8, const qd::Rect &r, int just, int size, bool bold, uint32_t color);
int labelWidth(const std::string &utf8, int size);

} // namespace txt
