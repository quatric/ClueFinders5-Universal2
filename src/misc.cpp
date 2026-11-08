// The rest of the Toolbox the game links against: cursors, menus, alerts,
// mouse polling, printing, save dialogs, the scrap, Apple events and
// Multiprocessing locks. Printing and saving report that the user cancelled.
#include "cf.h"
#include "heap.h"
#include "host.h"
#include "res.h"
#include "window.h"

#include <SDL.h>
#include <map>

static uint32_t fourcc(const char *s) {
	return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3];
}

const int32_t userCanceledErr = -128;

// ---------------------------------------------------------------------------
// Cursors

static int cursorHidden = 0;
static void applyCursor() {
	SDL_ShowCursor(cursorHidden > 0 ? SDL_DISABLE : SDL_ENABLE);
}
IMPORT(InitCursor) {
	cursorHidden = 0;
	applyCursor();
}
IMPORT(SetCursor) {
	// Black-and-white cursors only come from the system here: use the arrow
	SDL_SetCursor(SDL_GetDefaultCursor());
}

// Color cursors ('crsr'): the resource is parsed here and becomes an SDL cursor
namespace {

uint16_t be16(const uint8_t *p) {
	return (uint16_t)(p[0] << 8 | p[1]);
}
uint32_t be32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

struct ColorCursor {
	std::vector<uint8_t> data; // the 'crsr' resource
	SDL_Cursor *cursor = nullptr;
};
std::map<gaddr, ColorCursor> colorCursors;

SDL_Cursor *makeCursor(const std::vector<uint8_t> &d) {
	// CCrsr: type, map, data, ..., crsr1Data[32] at 20, crsrMask[32] at 52, hot spot (v, h) at 84
	if (d.size() < 96)
		return nullptr;
	uint32_t mapOff = be32(&d[2]), dataOff = be32(&d[6]);
	if (mapOff + 50 > d.size())
		return nullptr;
	const uint8_t *pm = &d[mapOff];
	int rowBytes = be16(pm + 4) & 0x3FFF;
	int top = (int16_t)be16(pm + 6), left = (int16_t)be16(pm + 8);
	int bottom = (int16_t)be16(pm + 10), right = (int16_t)be16(pm + 12);
	int depth = be16(pm + 32);
	uint32_t tableOff = be32(pm + 42);
	int w = std::min(right - left, 16), h = std::min(bottom - top, 16);
	std::vector<uint32_t> colors;
	if (tableOff + 8 <= d.size()) {
		int n = be16(&d[tableOff + 6]) + 1;
		colors.assign(256, 0xFF000000u);
		for (int i = 0; i < n && tableOff + 8 + 8 * i + 8 <= d.size(); i++) {
			const uint8_t *e = &d[tableOff + 8 + 8 * i];
			int value = be16(e) & 0xFF;
			colors[value] = 0xFF000000u | (uint32_t)e[2] << 16 | (uint32_t)e[4] << 8 | e[6];
		}
	}
	if (w <= 0 || h <= 0 || !(depth == 1 || depth == 2 || depth == 4 || depth == 8) || colors.empty())
		return nullptr;
	int scale = 1;
	std::vector<uint32_t> px((size_t)16 * 16 * scale * scale, 0);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			bool mask = d[52 + 2 * y + x / 8] & (0x80 >> (x % 8));
			bool bw = d[20 + 2 * y + x / 8] & (0x80 >> (x % 8));
			uint32_t c = 0;
			if (mask) {
				size_t bit = (size_t)y * rowBytes * 8 + (size_t)x * depth;
				if (dataOff + bit / 8 >= d.size())
					continue;
				int v = (d[dataOff + bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
				c = colors[v];
			} else if (bw) {
				c = 0xFF000000u; // inverted pixels: draw them black
			}
			for (int sy = 0; sy < scale; sy++)
				for (int sx = 0; sx < scale; sx++)
					px[(size_t)(y * scale + sy) * 16 * scale + x * scale + sx] = c;
		}
	}
	SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), 16 * scale, 16 * scale, 32, 16 * scale * 4,
		SDL_PIXELFORMAT_ARGB8888);
	if (!surf)
		return nullptr;
	int hotV = (int16_t)be16(&d[84]), hotH = (int16_t)be16(&d[86]);
	SDL_Cursor *cur = SDL_CreateColorCursor(surf, hotH * scale, hotV * scale);
	SDL_FreeSurface(surf);
	return cur;
}

} // namespace

IMPORT(GetCCursor) {
	gaddr r = res::get(fourcc("crsr"), c.sarg16(0));
	if (!r || !emu::r32(r)) {
		c.ret(0);
		return;
	}
	const uint8_t *p = emu::ptr(emu::r32(r));
	ColorCursor cc;
	cc.data.assign(p, p + heap::handleSize(r));
	gaddr h = heap::handleFromData(cc.data.data(), (uint32_t)cc.data.size());
	cc.cursor = makeCursor(cc.data);
	colorCursors[h] = std::move(cc);
	c.ret(h);
}
IMPORT(SetCCursor) {
	auto it = colorCursors.find(c.arg(0));
	SDL_SetCursor(it != colorCursors.end() && it->second.cursor ? it->second.cursor : SDL_GetDefaultCursor());
}
IMPORT(DisposeCCursor) {
	auto it = colorCursors.find(c.arg(0));
	if (it == colorCursors.end())
		return;
	if (SDL_GetCursor() == it->second.cursor)
		SDL_SetCursor(SDL_GetDefaultCursor());
	if (it->second.cursor)
		SDL_FreeCursor(it->second.cursor);
	heap::disposeHandle(it->first);
	colorCursors.erase(it);
}
IMPORT(HideCursor) {
	cursorHidden++;
	applyCursor();
}
IMPORT(ShowCursor) {
	if (cursorHidden > 0)
		cursorHidden--;
	applyCursor();
}
IMPORT(ShieldCursor) {}
IMPORT(GetQDGlobalsArrow) {
	static gaddr arrow = heap::alloc(68, true);
	if (c.arg(0))
		memcpy(emu::ptr(c.arg(0)), emu::ptr(arrow), 68);
	c.ret(c.arg(0) ? c.arg(0) : arrow);
}

// ---------------------------------------------------------------------------
// Mouse polling outside the event loop

IMPORT(Button) {
	ev::pumpOnly();
	c.ret(host::mouseDown);
}
IMPORT(StillDown) {
	ev::pumpOnly();
	c.ret(host::mouseDown);
}

// ---------------------------------------------------------------------------
// Menus: the game runs full screen with the menu bar hidden

IMPORT(HideMenuBar) {}
IMPORT(ShowMenuBar) {}
IMPORT(AppendMenuItemTextWithCFString) { c.ret(0); }
IMPORT(CopyMenuItemTextAsCFString) {
	emu::w32(c.arg(2), cf::makeString(std::string()));
	c.ret(0);
}
IMPORT(CountMenuItems) { c.ret(0); }
IMPORT(DeleteMenuItems) { c.ret(0); }
IMPORT(DisableMenuItem) {}
IMPORT(EnableMenuItem) {}
IMPORT(GetIndMenuItemWithCommandID) { c.ret((uint32_t)-5620); } // menuItemNotFoundErr
// Classic menus: the game has no MENU resources and only touches the Apple
// menu's About item, so a menu is just a handle that remembers item text
namespace {
std::map<int16_t, gaddr> menusByID;
std::map<gaddr, std::map<int, std::string>> menuItems;
}
IMPORT(GetMenu) {
	int16_t id = c.sarg16(0);
	gaddr &h = menusByID[id];
	if (!h) {
		h = heap::newHandle(32, true);
		emu::w16(emu::r32(h), (uint16_t)id);
	}
	c.ret(h);
}
IMPORT(AcquireRootMenu) {
	static gaddr root = heap::newHandle(32, true);
	c.ret(root);
}
IMPORT(GetMenuItemText) {
	// (menu, item, Str255 out)
	auto &items = menuItems[c.arg(0)];
	auto it = items.find(c.sarg16(1));
	emu::wpstr(c.arg(2), it != items.end() ? it->second : c.sarg16(1) == 1 ? "About " : "");
}
IMPORT(SetMenuItemText) {
	menuItems[c.arg(0)][c.sarg16(1)] = emu::pstr(c.arg(2));
}
IMPORT(MenuKey) { c.ret(0); }
IMPORT(MenuSelect) { c.ret(0); }
IMPORT(HiliteMenu) {}
IMPORT(DrawMenuBar) {}
IMPORT(DeleteMenu) {}
IMPORT(DisposeMenu) {
	for (auto it = menusByID.begin(); it != menusByID.end(); ++it) {
		if (it->second == c.arg(0)) {
			menusByID.erase(it);
			break;
		}
	}
	menuItems.erase(c.arg(0));
	heap::disposeHandle(c.arg(0));
}

// ---------------------------------------------------------------------------
// Alerts become native message boxes

namespace {
struct Alert {
	std::string title, text;
	gaddr notify = 0; // sheets report their button to this event target
	std::vector<std::string> buttons; // default first
	std::vector<int> items;
};
std::map<gaddr, Alert> alerts;

std::string alertText(gaddr s, const char *def) {
	if (s == 0xFFFFFFFF)
		return def;
	return s ? cf::utf8(s) : std::string();
}
}

IMPORT(CreateStandardAlert) {
	// (alertType, CFStringRef error, CFStringRef explanation, const AlertStdCFStringAlertParamRec *, DialogRef *)
	Alert a;
	a.title = cf::utf8(c.arg(1));
	a.text = c.arg(2) ? cf::utf8(c.arg(2)) : std::string();
	gaddr p = c.arg(3);
	if (p) {
		std::string def = alertText(emu::r32(p + 6), "OK"), cancel = alertText(emu::r32(p + 10), "Cancel"),
					other = alertText(emu::r32(p + 14), "Don't Save");
		if (!def.empty()) {
			a.buttons.push_back(def);
			a.items.push_back(1);
		}
		if (!other.empty() && emu::r32(p + 14)) {
			a.buttons.push_back(other);
			a.items.push_back(3);
		}
		if (!cancel.empty() && emu::r32(p + 10)) {
			a.buttons.push_back(cancel);
			a.items.push_back(2);
		}
	}
	if (a.buttons.empty()) {
		a.buttons.push_back("OK");
		a.items.push_back(1);
	}
	gaddr ref = heap::alloc(16, true);
	alerts[ref] = a;
	emu::w32(c.arg(4), ref);
	c.ret(0);
}
IMPORT(CreateStandardSheet) {
	// (alertType, error, explanation, param, EventTargetRef notifyTarget, DialogRef *):
	// CreateStandardAlert's arguments with the target in the out slot
	gaddr notify = c.arg(4), out = c.arg(5);
	emu::w32(c.esp + 4 + 4 * 4, out);
	imp_CreateStandardAlert(c);
	alerts[emu::r32(out)].notify = notify;
}
IMPORT(RunStandardAlert) {
	auto it = alerts.find(c.arg(0));
	int item = 1;
	if (it != alerts.end()) {
		int hit = host::messageBox(it->second.title, it->second.text, it->second.buttons);
		item = it->second.items[std::max(0, std::min(hit, (int)it->second.items.size() - 1))];
		alerts.erase(it);
	}
	if (c.arg(2))
		emu::w16(c.arg(2), (uint16_t)item);
	c.ret(0);
}
IMPORT(GetDialogWindow) { c.ret(c.arg(0)); }

namespace sheets {
// ShowSheetWindow on an alert sheet: ask now, then send the button's command
bool showSheet(gaddr dialog) {
	auto it = alerts.find(dialog);
	if (it == alerts.end())
		return false;
	Alert a = it->second;
	alerts.erase(it);
	int hit = host::messageBox(a.title, a.text, a.buttons);
	int item = a.items[std::max(0, std::min(hit, (int)a.items.size() - 1))];
	uint32_t ids[] = {0, fourcc("ok  "), fourcc("not!"), fourcc("othr")};
	uint8_t cmd[14] = {};
	memcpy(cmd + 4, &ids[item], 4);
	gaddr ref = ev::newEvent(fourcc("cmds"), 1);
	ev::event(ref)->set(fourcc("----"), fourcc("hcmd"), cmd, 14);
	ev::Target *t = ev::target(a.notify);
	ev::send(ref, t ? t : ev::appTarget());
	ev::releaseEvent(ref);
	return true;
}
}
// Classic alerts from ALRT/DITL resources, with ParamText's ^0-^3
namespace {
std::string paramText[4];
}
IMPORT(ParamText) {
	for (int i = 0; i < 4; i++)
		paramText[i] = c.arg(i) ? cf::macRomanToUtf8(emu::pstr(c.arg(i))) : std::string();
}
static int16_t classicAlert(int16_t id, const char *kind) {
	gaddr alrt = res::get(fourcc("ALRT"), id);
	gaddr ditl = alrt && emu::r32(alrt) ? res::get(fourcc("DITL"), (int16_t)be16(emu::ptr(emu::r32(alrt)) + 8)) : 0;
	std::string text;
	std::vector<std::string> buttons;
	std::vector<int> items;
	if (ditl && emu::r32(ditl)) {
		const uint8_t *d = emu::ptr(emu::r32(ditl));
		uint32_t size = heap::handleSize(ditl), o = 2;
		int n = be16(d) + 1;
		for (int i = 1; i <= n && o + 14 <= size; i++) {
			int type = d[o + 12] & 0x7F, len = d[o + 13];
			std::string s = cf::macRomanToUtf8(std::string((const char *)d + o + 14, std::min<uint32_t>(len, size - o - 14)));
			if (type == 4) { // button
				buttons.push_back(s);
				items.push_back(i);
			} else if (type == 8) { // static text
				for (int k = 0; k < 4; k++) {
					std::string key = "^" + std::to_string(k);
					for (size_t at; (at = s.find(key)) != std::string::npos;)
						s.replace(at, 2, paramText[k]);
				}
				text += (text.empty() ? "" : "\n\n") + s;
			}
			o += 14 + len + (len & 1);
		}
	}
	if (buttons.empty()) {
		buttons.push_back("OK");
		items.push_back(1);
	}
	int hit = host::messageBox(host::gameName, text.empty() ? kind : text, buttons);
	return (int16_t)items[std::max(0, std::min(hit, (int)items.size() - 1))];
}
IMPORT(NoteAlert) { c.ret((uint16_t)classicAlert(c.sarg16(0), "Note")); }

IMPORT(NewModalFilterUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeModalFilterUPP) {}
IMPORT(StdFilterProc) { c.ret(0); }

static gaddr constString(const char *s) {
	gaddr r = cf::makeString(std::string(s));
	cf::get(r)->refs = 1 << 30;
	return r;
}
IMPORT_DATA(kCFUserNotificationAlertHeaderKey) {
	gaddr p = emu::sysAlloc(4);
	emu::w32(p, constString("AlertHeader"));
	return p;
}
IMPORT_DATA(kCFUserNotificationAlertMessageKey) {
	gaddr p = emu::sysAlloc(4);
	emu::w32(p, constString("AlertMessage"));
	return p;
}
IMPORT_DATA(kCFUserNotificationDefaultButtonTitleKey) {
	gaddr p = emu::sysAlloc(4);
	emu::w32(p, constString("DefaultButtonTitle"));
	return p;
}
IMPORT_DATA(kCFUserNotificationOtherButtonTitleKey) {
	gaddr p = emu::sysAlloc(4);
	emu::w32(p, constString("OtherButtonTitle"));
	return p;
}
IMPORT_DATA(kCFUserNotificationIconURLKey) {
	gaddr p = emu::sysAlloc(4);
	emu::w32(p, constString("IconURL"));
	return p;
}

namespace {
std::map<gaddr, gaddr> notifications; // notification -> dictionary
}

IMPORT(CFUserNotificationCreate) {
	// (allocator, CFTimeInterval timeout, flags, SInt32 *error, CFDictionaryRef)
	gaddr ref = heap::alloc(16, true);
	notifications[ref] = c.arg(5);
	cf::retain(c.arg(5));
	if (c.arg(4))
		emu::w32(c.arg(4), 0);
	c.ret(ref);
}
IMPORT(CFUserNotificationReceiveResponse) {
	// (notification, CFTimeInterval timeout, CFOptionFlags *response)
	auto it = notifications.find(c.arg(0));
	uint32_t response = 0;
	if (it != notifications.end()) {
		cf::Obj *d = cf::get(it->second);
		std::string header, message, def = "OK", other;
		if (d) {
			for (auto &kv : d->pairs) {
				std::string k = cf::utf8(kv.first);
				if (k == "AlertHeader") header = cf::utf8(kv.second);
				else if (k == "AlertMessage") message = cf::utf8(kv.second);
				else if (k == "DefaultButtonTitle") def = cf::utf8(kv.second);
				else if (k == "OtherButtonTitle") other = cf::utf8(kv.second);
			}
		}
		std::vector<std::string> buttons = {def};
		if (!other.empty())
			buttons.push_back(other);
		int hit = host::messageBox(header, message, buttons);
		response = hit == 1 ? 2 : 0; // kCFUserNotificationOtherResponse
	}
	if (c.arg(3))
		emu::w32(c.arg(3), response);
	c.ret(0);
}

// ---------------------------------------------------------------------------
// Printing: report a cancelled print dialog

IMPORT(PMCreateSession) {
	emu::w32(c.arg(0), heap::alloc(16, true));
	c.ret(0);
}
IMPORT(PMCreatePageFormat) {
	emu::w32(c.arg(0), heap::alloc(16, true));
	c.ret(0);
}
IMPORT(PMCreatePrintSettings) {
	emu::w32(c.arg(0), heap::alloc(16, true));
	c.ret(0);
}
IMPORT(PMSessionDefaultPageFormat) { c.ret(0); }
IMPORT(PMSessionDefaultPrintSettings) { c.ret(0); }
IMPORT(PMSetJobNameCFString) { c.ret(0); }
IMPORT(PMSetOrientation) { c.ret(0); }
IMPORT(PMSessionPageSetupDialog) {
	emu::w8(c.arg(2), 0);
	c.ret(0);
}
IMPORT(PMSessionPrintDialog) {
	host::messageBox("Printing", "Printing is not available in this version.", {"OK"});
	emu::w8(c.arg(3), 0); // accepted = false
	c.ret(0);
}
IMPORT(PMSessionBeginDocument) { c.ret((uint32_t)userCanceledErr); }
IMPORT(PMSessionEndDocument) { c.ret(0); }
IMPORT(PMSessionBeginPage) { c.ret((uint32_t)userCanceledErr); }
IMPORT(PMSessionEndPage) { c.ret(0); }
IMPORT(PMSessionError) { c.ret(0); }
IMPORT(PMSessionGetGraphicsContext) { c.ret((uint32_t)userCanceledErr); }
IMPORT(PMRelease) { c.ret(0); }

// ---------------------------------------------------------------------------
// Navigation Services: saving is cancelled

IMPORT(NavGetDefaultDialogCreationOptions) {
	memset(emu::ptr(c.arg(0)), 0, 66);
	c.ret(0);
}
IMPORT(NavCreatePutFileDialog) {
	emu::w32(c.arg(5), heap::alloc(16, true));
	c.ret(0);
}
IMPORT(NavDialogRun) { c.ret(0); }
IMPORT(NavDialogGetReply) { c.ret((uint32_t)userCanceledErr); }
IMPORT(NavDialogSetSaveFileExtensionHidden) { c.ret(0); }

// ---------------------------------------------------------------------------
// Scrap, drag, Apple events, MP

IMPORT(UnloadScrap) { c.ret(0); }
IMPORT(GetCurrentScrap) {
	emu::w32(c.arg(0), heap::alloc(16, true));
	c.ret(0);
}
IMPORT(GetScrapFlavorSize) { c.ret((uint32_t)-102); } // noTypeErr
IMPORT(GetScrapFlavorData) { c.ret((uint32_t)-102); }
IMPORT(AddDragItemFlavor) { c.ret(0); }
IMPORT(AEGetNthPtr) { c.ret((uint32_t)-1701); } // errAEDescNotFound
IMPORT(AEInteractWithUser) { c.ret(0); }
IMPORT(MPCreateCriticalRegion) {
	emu::w32(c.arg(0), heap::alloc(16, true));
	c.ret(0);
}
IMPORT(MPEnterCriticalRegion) { c.ret(0); }
IMPORT(MPExitCriticalRegion) { c.ret(0); }

IMPORT(ExitToShell) {
	logf("ExitToShell");
	exit(0);
}
IMPORT(SetItemCmd) {}
// Code fragments are PowerPC libraries ('pwpc'); none can load here
IMPORT(GetSharedLibrary) {
	// (libName, archType, loadFlags, CFragConnectionID *, Ptr *mainAddr, Str255 errMessage)
	emu::w32(c.arg(3), 0);
	if (c.arg(4))
		emu::w32(c.arg(4), 0);
	if (c.arg(5))
		emu::w8(c.arg(5), 0);
	c.ret((uint32_t)(int32_t)-2803); // cfragNoLibraryErr
}
