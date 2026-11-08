#pragma once

#include "emu.h"

// printf-style formatting with the arguments read from guest varargs
std::string guestFormat(const std::string &fmt, gaddr args);
