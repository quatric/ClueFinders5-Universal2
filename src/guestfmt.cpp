#include "guestfmt.h"
#include "cf.h"

#include <cstdio>

std::string guestFormat(const std::string &fmt, gaddr args) {
	std::string out;
	for (size_t i = 0; i < fmt.size(); i++) {
		if (fmt[i] != '%') {
			out += fmt[i];
			continue;
		}
		// Collect flags, width, precision and length modifiers
		std::string spec = "%";
		size_t j = i + 1;
		while (j < fmt.size() && strchr("-+ #0123456789.*", fmt[j])) {
			if (fmt[j] == '*') {
				spec += std::to_string((int32_t)emu::r32(args));
				args += 4;
			} else {
				spec += fmt[j];
			}
			j++;
		}
		bool longlong = false;
		while (j < fmt.size() && strchr("hlLqjzt", fmt[j])) {
			if (fmt[j] == 'q' || (fmt[j] == 'l' && j + 1 < fmt.size() && fmt[j + 1] == 'l'))
				longlong = true;
			j++;
		}
		if (j >= fmt.size())
			break;
		char conv = fmt[j];
		char buf[512];
		switch (conv) {
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'c':
			if (longlong) {
				uint64_t v = emu::r32(args) | (uint64_t)emu::r32(args + 4) << 32;
				args += 8;
				snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)v);
			} else {
				uint32_t v = emu::r32(args);
				args += 4;
				snprintf(buf, sizeof(buf), (spec + conv).c_str(), v);
			}
			out += buf;
			break;
		case 'f':
		case 'g':
		case 'e':
		case 'G':
		case 'E': {
			double v = emu::rdouble(args);
			args += 8;
			snprintf(buf, sizeof(buf), (spec + conv).c_str(), v);
			out += buf;
			break;
		}
		case 's': {
			std::string s = emu::cstr(emu::r32(args));
			args += 4;
			snprintf(buf, sizeof(buf), (spec + 's').c_str(), s.c_str());
			out += buf;
			break;
		}
		case 'p':
			snprintf(buf, sizeof(buf), "0x%x", emu::r32(args));
			args += 4;
			out += buf;
			break;
		case '@': {
			// A CF object: strings as themselves, anything else described
			gaddr obj = emu::r32(args);
			args += 4;
			cf::Obj *o = cf::get(obj);
			out += o && o->type == cf::kString ? cf::utf8(obj) : cf::describe(obj);
			break;
		}
		case '%':
			out += '%';
			break;
		default:
			out += spec + conv;
			break;
		}
		i = j;
	}
	return out;
}
