// Small Toolbox managers: Process Manager, Gestalt, time, Pascal strings
#include "emu.h"
#include "host.h"

#include <chrono>
#include <ctime>
#include <thread>

namespace {

std::string fourccStr(uint32_t v) {
	char s[5] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, 0};
	return s;
}

auto startTime = std::chrono::steady_clock::now();

} // namespace

uint32_t tickCount() {
	auto d = std::chrono::steady_clock::now() - startTime;
	return (uint32_t)(std::chrono::duration_cast<std::chrono::microseconds>(d).count() * 60 / 1000000) + 1;
}

// ---------------------------------------------------------------------------
// Process Manager: the game is the only process

const uint32_t kOurPSN = 2;

IMPORT(GetCurrentProcess) {
	emu::w32(c.arg(0), 0);
	emu::w32(c.arg(0) + 4, kOurPSN);
	c.ret(0);
}
IMPORT(GetNextProcess) {
	gaddr psn = c.arg(0);
	if (emu::r32(psn) == 0 && emu::r32(psn + 4) == 0) {
		emu::w32(psn + 4, kOurPSN);
		c.ret(0);
	} else {
		emu::w32(psn, 0);
		emu::w32(psn + 4, 0);
		c.ret((uint32_t)-600); // procNotFound
	}
}
IMPORT(SameProcess) {
	bool same = emu::r32(c.arg(0)) == emu::r32(c.arg(1)) && emu::r32(c.arg(0) + 4) == emu::r32(c.arg(1) + 4);
	emu::w8(c.arg(2), same);
	c.ret(0);
}
IMPORT(SetFrontProcess) { c.ret(0); }
IMPORT(GetProcessInformation) {
	// ProcessInfoRec: length, name, PSN, type, signature, mode, location,
	// size, free mem, launcher, launch date, active time, appSpec (FSSpec*)
	gaddr info = c.arg(1);
	gaddr name = emu::r32(info + 4);
	if (name)
		emu::wpstr(name, host::gameName);
	emu::w32(info + 8, 0);
	emu::w32(info + 12, kOurPSN);
	emu::w32(info + 16, 0x4150504C); // 'APPL'
	emu::w32(info + 20, 0x5343314B); // 'SC1K'
	c.ret(0);
}

// ---------------------------------------------------------------------------
// Gestalt

IMPORT(Gestalt) {
	uint32_t sel = c.arg(0);
	uint32_t v = 0;
	bool known = true;
	switch (sel) {
	case 0x73797376: v = 0x1049; break; // 'sysv': 10.4.9
	case 0x73797331: v = 10; break;     // 'sys1'
	case 0x73797332: v = 4; break;      // 'sys2'
	case 0x73797333: v = 9; break;      // 'sys3'
	case 0x7174696D: v = 0x07000000; break; // 'qtim': QuickTime 7
	case 0x71642020: v = 0x0220; break; // 'qd  ': Color QuickDraw 32-bit
	case 0x72616D20: v = 512u << 20; break; // 'ram '
	case 0x70686D76: v = 512u << 20; break; // 'lram'?
	default: known = false; break;
	}
	if (!known) {
		logf("Gestalt('%s'): unknown", fourccStr(sel).c_str());
		c.ret((uint32_t)-5551); // gestaltUndefSelectorErr
		return;
	}
	if (c.arg(1))
		emu::w32(c.arg(1), v);
	c.ret(0);
}

// ---------------------------------------------------------------------------
// Time

IMPORT(GetDateTime) {
	// Seconds since 1904-01-01, local time
	time_t now = time(nullptr);
	struct tm lt;
	localtime_r(&now, &lt);
	uint32_t secs = (uint32_t)(now + lt.tm_gmtoff + 2082844800u);
	emu::w32(c.arg(0), secs);
}
IMPORT(TickCount) {
	// The game idles by spinning on TickCount; once it has asked many times
	// within one tick, sleep until the next one instead of burning a core
	static uint32_t lastTick, calls;
	uint32_t t = tickCount();
	if (t != lastTick) {
		lastTick = t;
		calls = 0;
	} else if (++calls > 100) {
		auto d = std::chrono::steady_clock::now() - startTime;
		long long us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
		long long next = ((long long)t * 1000000 + 59) / 60;
		if (next > us)
			std::this_thread::sleep_for(std::chrono::microseconds(next - us));
		t = tickCount();
		lastTick = t;
		calls = 0;
	}
	c.ret(t);
}

// ---------------------------------------------------------------------------
// Pascal strings

IMPORT(PLstrlen) { c.ret(emu::r8(c.arg(0))); }
IMPORT(PLstrcpy) {
	uint8_t len = emu::r8(c.arg(1));
	memmove(emu::ptr(c.arg(0)), emu::ptr(c.arg(1)), len + 1);
	c.ret(c.arg(0));
}
IMPORT(PLstrncpy) {
	uint8_t len = emu::r8(c.arg(1));
	uint32_t max = c.arg(2);
	if (len > max)
		len = (uint8_t)max;
	memmove(emu::ptr(c.arg(0) + 1), emu::ptr(c.arg(1) + 1), len);
	emu::w8(c.arg(0), len);
	c.ret(c.arg(0));
}
IMPORT(CopyCStringToPascal) {
	std::string s = emu::cstr(c.arg(0));
	emu::wpstr(c.arg(1), s);
}

// ---------------------------------------------------------------------------
// Misc

IMPORT(SysBeep) {}
IMPORT(EnterMovies) { c.ret(0); }

// ---------------------------------------------------------------------------
// Utilities

IMPORT(NumToString) { emu::wpstr(c.arg(1), std::to_string(c.sarg(0))); }
IMPORT(EqualString) {
	// (str1, str2, caseSensitive, diacSensitive)
	std::string a = emu::pstr(c.arg(0)), b = emu::pstr(c.arg(1));
	if (!(c.arg(2) & 0xFF)) {
		for (char &ch : a) ch = (char)tolower((unsigned char)ch);
		for (char &ch : b) ch = (char)tolower((unsigned char)ch);
	}
	c.ret(a == b);
}
IMPORT(Microseconds) {
	auto d = std::chrono::steady_clock::now() - startTime;
	uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(d).count();
	// UnsignedWide is {lo, hi} on little-endian Macs
	emu::w32(c.arg(0), (uint32_t)us);
	emu::w32(c.arg(0) + 4, (uint32_t)(us >> 32));
}
IMPORT(GetUTCDateTime) {
	// (UTCDateTime *, options): 16-bit high seconds, 32-bit low seconds, 16-bit fraction since 1904
	uint64_t secs = (uint64_t)::time(nullptr) + 2082844800ull;
	emu::w16(c.arg(0), (uint16_t)(secs >> 32));
	emu::w32(c.arg(0) + 2, (uint32_t)secs);
	emu::w16(c.arg(0) + 6, 0);
	c.ret(0);
}
IMPORT(GetDblTime) { c.ret(30); }
IMPORT(SetA5) { c.ret(0); }
IMPORT(SetCurrentA5) { c.ret(0); }
IMPORT(SysBreakStr) { logf("SysBreakStr: %s", emu::pstr(c.arg(0)).c_str()); }
IMPORT(_dyld_lookup_and_bind_with_hint) {
	logf("_dyld_lookup_and_bind_with_hint(%s)", emu::cstr(c.arg(0)).c_str());
	if (c.arg(2))
		emu::w32(c.arg(2), imports::thunk(emu::cstr(c.arg(0)).substr(1)));
	if (c.arg(3))
		emu::w32(c.arg(3), 0);
}
