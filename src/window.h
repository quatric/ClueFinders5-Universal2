// Window Manager, Control Manager and the Carbon Event Manager
#pragma once

#include "emu.h"
#include "qd.h"

#include <string>
#include <vector>

namespace ev {

// An event target: the application, a window or a control
struct Target {
	struct Handler {
		gaddr ref;   // EventHandlerRef
		gaddr proc;
		gaddr userData;
		std::vector<std::pair<uint32_t, uint32_t>> types;
	};
	gaddr addr = 0;
	std::vector<Handler> handlers; // most recently installed first
	Target *parent = nullptr;
};

Target *appTarget();
Target *target(gaddr ref);
gaddr newTarget(Target *t);

// Events as the guest sees them (EventRef)
struct Param {
	uint32_t type;
	std::vector<uint8_t> data;
};
struct Event {
	uint32_t eventClass, kind;
	double time;
	int refs = 1;
	std::vector<std::pair<uint32_t, Param>> params;
	void set(uint32_t name, uint32_t type, const void *data, size_t size);
	const Param *get(uint32_t name) const;
};
gaddr newEvent(uint32_t eventClass, uint32_t kind);
Event *event(gaddr ref);
void releaseEvent(gaddr ref);
// Sends an event through a target's handler chain; returns the OSStatus
int32_t send(gaddr eventRef, Target *t);

// Runs one pass of the event loop: input, timers, redraws and the screen.
// Waits up to `timeout` seconds for something to happen.
void runOnce(double timeout);
// Reads input into the queue and refreshes the screen, without dispatching
void pumpOnly();
extern bool quitRequested;
void init();

} // namespace ev

namespace wm {

struct Control;

struct Window {
	gaddr ref = 0;
	qd::Port *port = nullptr;
	qd::Rect bounds; // content area in global coordinates
	uint32_t wclass = 0, attrs = 0;
	bool visible = false;
	std::string title;
	uint32_t refCon = 0;
	ev::Target target;
	Control *root = nullptr;
	qd::Rects invalid;
	bool modalQuit = false;
	int modalDepth = 0;
	gaddr group = 0;
	Window *sheetParent = nullptr;
	uint32_t contentColor = 0xFFFFFFFF;
	gaddr focus = 0; // ControlRef with keyboard focus
	std::vector<std::pair<uint32_t, uint32_t>> props;
	// Made with NewCWindow: redraws come as update events from
	// WaitNextEvent rather than as Carbon window events
	bool classic = false;
	int16_t procID = 0;
};

Window *window(gaddr ref);
Window *createWindow(uint32_t wclass, uint32_t attrs, const qd::Rect &bounds);
Window *windowForPort(gaddr port);
Window *frontWindow();
Window *windowAt(int x, int y);
std::vector<Window *> &windows(); // back to front
// Composites visible windows onto the screen and presents it
void presentScreen(bool force = false);
// Draws the visible windows into a screen-sized 32-bit pixmap
void composeScreen(const qd::Bits &screen);
void invalidate(Window *w, const qd::Rects &r);
// Sends update events for windows that need drawing
void updateWindows();
// Whether a target has a handler for this event type
bool handles(const ev::Target &t, uint32_t eventClass, uint32_t kind);
void drawControls(Window *w);

// Mouse handling for controls: returns true if a control took the click
bool trackControls(Window *w, int localH, int localV, uint32_t mods);
bool keyToControls(Window *w, uint8_t ch, uint16_t key, uint32_t mods);

} // namespace wm
