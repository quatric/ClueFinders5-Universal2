// The display as the game sees it: one screen the size of the virtual
// screen, which never changes mode. The host window scales it to fit.
#include "cf.h"
#include "emu.h"
#include "host.h"

namespace {

const uint32_t kMainDisplay = 0x04273000;

gaddr number(int64_t v) {
	cf::Obj *o = new cf::Obj{cf::kNumber};
	o->ival = v;
	o->dval = (double)v;
	return cf::make(o);
}

// A display mode dictionary like CGDisplayCurrentMode's
gaddr modeDict() {
	static gaddr mode = 0;
	if (!mode) {
		cf::Obj *d = new cf::Obj{cf::kDictionary};
		auto add = [&](const char *k, int64_t v) { d->pairs.push_back({cf::makeString(std::string(k)), number(v)}); };
		add("Width", host::screenW);
		add("Height", host::screenH);
		add("BitsPerPixel", 32);
		add("BitsPerSample", 8);
		add("SamplesPerPixel", 3);
		add("RefreshRate", 60);
		add("Mode", 1);
		add("IOFlags", 7);
		add("UsableForDesktopGUI", 1);
		d->refs = 1 << 30;
		mode = cf::make(d);
	}
	return mode;
}

} // namespace

IMPORT(CGMainDisplayID) { c.ret(kMainDisplay); }
IMPORT(CGDisplayCurrentMode) { c.ret(modeDict()); }
IMPORT(CGDisplayAvailableModes) {
	static gaddr modes = 0;
	if (!modes) {
		cf::Obj *a = new cf::Obj{cf::kArray};
		a->items.push_back(modeDict());
		a->refs = 1 << 30;
		modes = cf::make(a);
	}
	c.ret(modes);
}
IMPORT(CGDisplaySwitchToMode) {
	logf("CGDisplaySwitchToMode (ignored)");
	c.ret(0);
}

// Display Manager notifications: the display never changes
IMPORT(NewDMExtendedNotificationUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeDMExtendedNotificationUPP) {}
IMPORT(DMRegisterExtendedNotifyProc) { c.ret(0); }
IMPORT(DMRemoveExtendedNotifyProc) { c.ret(0); }

// Full-screen presentation is the host window's business
static uint32_t uiMode = 0, uiOptions = 0;
IMPORT(GetSystemUIMode) {
	if (c.arg(0))
		emu::w32(c.arg(0), uiMode);
	if (c.arg(1))
		emu::w32(c.arg(1), uiOptions);
}
IMPORT(SetSystemUIMode) {
	uiMode = c.arg(0);
	uiOptions = c.arg(1);
	c.ret(0);
}
IMPORT(GetMBarHeight) { c.ret(0); }
IMPORT(SetDepth) { c.ret(0); } // the emulated screen stays 32-bit
IMPORT(CGDisplayBitsPerPixel) { c.ret(32); }
IMPORT_SRET(CGDisplayBounds) {
	// CGRect of floats (origin, size), through the hidden result pointer
	gaddr r = c.arg(0);
	emu::wfloat(r, 0);
	emu::wfloat(r + 4, 0);
	emu::wfloat(r + 8, (float)host::screenW);
	emu::wfloat(r + 12, (float)host::screenH);
	c.ret(r);
}
