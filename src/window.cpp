// Carbon Event Manager and Window Manager. Input from SDL becomes Carbon
// events in the main queue; the event loop sends them through the handler
// chains (control -> window -> application), fires timers, sends update
// events and composites the visible windows onto the screen.
#include "window.h"
#include "cf.h"
#include "heap.h"
#include "host.h"
#include "sound.h"

namespace movie {
void taskAll();
}

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>

static uint32_t fourcc(const char *s) {
	return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3];
}

static std::string fourccStr(uint32_t v) {
	char s[5] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, 0};
	return s;
}

const int32_t eventNotHandledErr = -9874;
const int32_t eventParameterNotFoundErr = -9870;
const int32_t eventLoopTimedOutErr = -9875;
const int32_t eventLoopQuitErr = -9876;

namespace ev {

bool quitRequested = false;

namespace {

const uint32_t kEventMagic = 0x45564E54; // 'EVNT'

std::unordered_map<gaddr, Event *> events;
std::unordered_map<gaddr, Target *> targets;
std::unordered_map<gaddr, std::pair<Target *, gaddr>> handlerRefs; // ref -> (target, ref)
std::deque<gaddr> queue;

struct Timer {
	gaddr ref, proc, userData;
	double next, interval;
	bool firing = false;
};
std::map<gaddr, Timer> timers;

// Frames of handler chains in progress, for CallNextEventHandler
struct Frame {
	Target *target;
	std::vector<Target::Handler> chain; // the target's handlers when the event arrived
	size_t next;
	gaddr event;
};
std::vector<Frame> frames;
const gaddr kCallRefBase = 0xE00F0000;

Target app;
gaddr kEventLoop, kMainQueue;

// Mouse tracking: the window that got the mouse down gets the mouse up
wm::Window *mouseWindow = nullptr;

} // namespace

Target *appTarget() {
	return &app;
}

gaddr newTarget(Target *t) {
	t->addr = heap::alloc(16, true);
	targets[t->addr] = t;
	return t->addr;
}

Target *target(gaddr ref) {
	auto it = targets.find(ref);
	return it == targets.end() ? nullptr : it->second;
}

void Event::set(uint32_t name, uint32_t type, const void *data, size_t size) {
	for (auto &p : params) {
		if (p.first == name) {
			p.second.type = type;
			p.second.data.assign((const uint8_t *)data, (const uint8_t *)data + size);
			return;
		}
	}
	Param prm;
	prm.type = type;
	prm.data.assign((const uint8_t *)data, (const uint8_t *)data + size);
	params.push_back({name, prm});
}

const Param *Event::get(uint32_t name) const {
	for (auto &p : params) {
		if (p.first == name)
			return &p.second;
	}
	return nullptr;
}

gaddr newEvent(uint32_t eventClass, uint32_t kind) {
	Event *e = new Event;
	e->eventClass = eventClass;
	e->kind = kind;
	e->time = host::now();
	gaddr a = heap::alloc(16, true);
	emu::w32(a, kEventMagic);
	events[a] = e;
	return a;
}

Event *event(gaddr ref) {
	auto it = events.find(ref);
	return it == events.end() ? nullptr : it->second;
}

void releaseEvent(gaddr ref) {
	Event *e = event(ref);
	if (!e)
		return;
	if (--e->refs > 0)
		return;
	events.erase(ref);
	heap::free(ref);
	delete e;
}

static bool matches(const Target::Handler &h, const Event *e) {
	for (auto &t : h.types) {
		if (t.first == e->eventClass && t.second == e->kind)
			return true;
	}
	return false;
}

static int32_t builtin(gaddr eventRef, Target *t);

// Runs the handlers of `t` from index `from`, then its parents
static bool installed(Target *t, gaddr ref) {
	for (auto &h : t->handlers) {
		if (h.ref == ref)
			return true;
	}
	return false;
}

// Runs `chain` (a snapshot of t's handlers) from index `from`, then t's
// built-in handler, then the parents. Handlers installed while the event is
// being handled do not see it; removed ones are skipped.
static int32_t dispatchFrom(gaddr eventRef, Target *t, std::vector<Target::Handler> chain, size_t from) {
	Event *e = event(eventRef);
	while (t) {
		for (size_t i = from; i < chain.size(); i++) {
			const Target::Handler &h = chain[i];
			if (!matches(h, e) || !installed(t, h.ref))
				continue;
			frames.push_back({t, chain, i + 1, eventRef});
			if (emu::traceImports)
				logf("handler 0x%08x for '%s' %u on 0x%08x (frame %zu)", h.proc, fourccStr(e->eventClass).c_str(), e->kind,
					t->addr, frames.size() - 1);
			gaddr callRef = kCallRefBase + (gaddr)(frames.size() - 1) * 4;
			int32_t r = (int32_t)emu::call(h.proc, {callRef, eventRef, h.userData});
			frames.pop_back();
			e = event(eventRef);
			if (!e)
				return r;
			if (r != eventNotHandledErr)
				return r;
		}
		int32_t r = builtin(eventRef, t);
		if (r != eventNotHandledErr)
			return r;
		t = t->parent;
		if (t)
			chain = t->handlers;
		from = 0;
	}
	return eventNotHandledErr;
}

int32_t send(gaddr eventRef, Target *t) {
	Event *e = event(eventRef);
	if (!e)
		return -50;
	if (emu::traceImports)
		logf("event '%s' %u -> target 0x%08x", fourccStr(e->eventClass).c_str(), e->kind, t ? t->addr : 0);
	e->refs++;
	int32_t r = dispatchFrom(eventRef, t, t ? t->handlers : std::vector<Target::Handler>(), 0);
	releaseEvent(eventRef);
	return r;
}

// ---------------------------------------------------------------------------
// Input to events

static void setPoint(Event *e, uint32_t name, int x, int y) {
	float pt[2] = {(float)x, (float)y};
	e->set(name, fourcc("hipt"), pt, 8);
}

static void setU32(Event *e, uint32_t name, uint32_t type, uint32_t v) {
	e->set(name, type, &v, 4);
}

static gaddr mouseEvent(const host::Input &in) {
	uint32_t kind = in.kind == host::Input::MouseDown ? 1 : in.kind == host::Input::MouseUp ? 2
		: in.kind == host::Input::Wheel ? 10 : host::mouseDown ? 6 : 5;
	gaddr ref = newEvent(fourcc("mous"), kind);
	Event *e = event(ref);
	e->time = in.time;
	setPoint(e, fourcc("mloc"), in.x, in.y);
	setU32(e, fourcc("kmod"), fourcc("magn"), in.mods);
	uint16_t button = 1;
	e->set(fourcc("mbtn"), fourcc("mbtn"), &button, 2);
	setU32(e, fourcc("ccnt"), fourcc("magn"), (uint32_t)in.clicks);
	if (kind == 10) {
		uint16_t axis = 1; // kEventMouseWheelAxisY
		e->set(fourcc("mwax"), fourcc("mwax"), &axis, 2);
		setU32(e, fourcc("mwdl"), fourcc("long"), (uint32_t)in.wheel);
	}
	wm::Window *w = wm::windowAt(in.x, in.y);
	if (kind == 2 && mouseWindow)
		w = mouseWindow;
	if (w) {
		e->set(fourcc("wind"), fourcc("wind"), &w->ref, 4);
		setPoint(e, fourcc("wmou"), in.x - w->bounds.left, in.y - w->bounds.top);
	}
	return ref;
}

static gaddr keyEvent(const host::Input &in) {
	uint32_t kind = in.kind == host::Input::KeyDown ? 1 : in.kind == host::Input::KeyRepeat ? 2 : 3;
	gaddr ref = newEvent(fourcc("keyb"), kind);
	Event *e = event(ref);
	e->time = in.time;
	e->set(fourcc("kchr"), fourcc("TEXT"), &in.ch, 1);
	setU32(e, fourcc("kcod"), fourcc("magn"), in.macKey);
	setU32(e, fourcc("kmod"), fourcc("magn"), in.mods);
	setU32(e, fourcc("kbdt"), fourcc("magn"), 0);
	uint16_t uni = in.ch < 0x80 ? in.ch : (uint16_t)cf::utf16(cf::macRomanToUtf8(std::string(1, (char)in.ch)))[0];
	e->set(fourcc("kuni"), fourcc("utxt"), &uni, 2);
	return ref;
}

static void pumpInput() {
	host::Input in;
	while (host::poll(in)) {
		switch (in.kind) {
		case host::Input::Quit: {
			// The window's close box or Cmd-Q: ask the game, as its Quit menu item would
			gaddr ref = newEvent(fourcc("cmds"), 1);
			uint8_t cmd[14] = {};
			uint32_t id = fourcc("quit");
			memcpy(cmd + 4, &id, 4);
			event(ref)->set(fourcc("----"), fourcc("hcmd"), cmd, 14);
			queue.push_back(ref);
			static double lastQuit = -10;
			if (in.time - lastQuit < 2.0)
				quitRequested = true; // asked twice: leave now
			lastQuit = in.time;
			break;
		}
		case host::Input::MouseDown:
		case host::Input::MouseUp:
		case host::Input::MouseMove:
		case host::Input::Wheel:
			queue.push_back(mouseEvent(in));
			break;
		default:
			queue.push_back(keyEvent(in));
			break;
		}
	}
}

// Where the dispatcher sends an event from the queue
static Target *dispatchTarget(Event *e) {
	if (e->eventClass == fourcc("mous")) {
		const Param *p = e->get(fourcc("wind"));
		if (p) {
			gaddr w;
			memcpy(&w, p->data.data(), 4);
			if (wm::Window *win = wm::window(w))
				return &win->target;
		}
		return &app;
	}
	if (e->eventClass == fourcc("keyb")) {
		if (wm::Window *w = wm::frontWindow())
			return &w->target;
	}
	return &app;
}

static void dispatchQueued() {
	// Events posted while dispatching wait for the next pass
	size_t n = queue.size();
	for (size_t i = 0; i < n && !queue.empty(); i++) {
		gaddr ref = queue.front();
		queue.pop_front();
		Event *e = event(ref);
		if (!e)
			continue;
		if (e->eventClass == fourcc("mous") && e->kind == 1) {
			const Param *p = e->get(fourcc("wind"));
			gaddr w = 0;
			if (p)
				memcpy(&w, p->data.data(), 4);
			mouseWindow = wm::window(w);
		}
		send(ref, dispatchTarget(e));
		if (e->eventClass == fourcc("mous") && e->kind == 2)
			mouseWindow = nullptr;
		releaseEvent(ref);
	}
}

static double nextTimer() {
	double t = 1e30;
	for (auto &it : timers) {
		if (!it.second.firing)
			t = std::min(t, it.second.next);
	}
	return t;
}

static void fireTimers() {
	double now = host::now();
	std::vector<gaddr> due;
	for (auto &it : timers) {
		if (!it.second.firing && it.second.next <= now)
			due.push_back(it.first);
	}
	for (gaddr ref : due) {
		auto it = timers.find(ref);
		if (it == timers.end() || it->second.firing)
			continue;
		Timer &t = it->second;
		t.next = t.interval > 0 ? std::max(t.next + t.interval, now) : 1e30;
		t.firing = true;
		gaddr proc = t.proc, ud = t.userData;
		emu::call(proc, {ref, ud});
		it = timers.find(ref);
		if (it != timers.end())
			it->second.firing = false;
	}
}

void pumpOnly() {
	pumpInput();
	wm::presentScreen();
}

void runOnce(double timeout) {
	pumpInput();
	fireTimers();
	snd::serviceCallbacks();
	movie::taskAll();
	dispatchQueued();
	wm::updateWindows();
	wm::presentScreen();
	if (timeout > 0 && queue.empty()) {
		double wait = std::min(timeout, std::min(nextTimer() - host::now(), 1.0 / 60));
		host::wait(wait);
	}
}

// ---------------------------------------------------------------------------
// Built-in standard handlers

static int32_t builtin(gaddr eventRef, Target *t) {
	Event *e = event(eventRef);
	wm::Window *w = nullptr;
	for (wm::Window *x : wm::windows()) {
		if (&x->target == t)
			w = x;
	}
	if (!w)
		return eventNotHandledErr;
	if (e->eventClass == fourcc("mous") && e->kind == 1) {
		const Param *p = e->get(fourcc("mloc"));
		float pt[2];
		memcpy(pt, p->data.data(), 8);
		uint32_t mods = 0;
		if (const Param *m = e->get(fourcc("kmod")))
			memcpy(&mods, m->data.data(), 4);
		int h = (int)pt[0] - w->bounds.left + w->port->portRect.left;
		int v = (int)pt[1] - w->bounds.top + w->port->portRect.top;
		if (wm::trackControls(w, h, v, mods))
			return 0;
	}
	if (e->eventClass == fourcc("keyb") && (e->kind == 1 || e->kind == 2)) {
		uint8_t ch = e->get(fourcc("kchr"))->data[0];
		uint32_t key, mods;
		memcpy(&key, e->get(fourcc("kcod"))->data.data(), 4);
		memcpy(&mods, e->get(fourcc("kmod"))->data.data(), 4);
		if (wm::keyToControls(w, ch, (uint16_t)key, mods))
			return 0;
	}
	if (e->eventClass == fourcc("wind") && e->kind == 2) { // kEventWindowDrawContent
		wm::drawControls(w);
		return 0;
	}
	return eventNotHandledErr;
}

} // namespace ev

// ===========================================================================
// Window Manager

namespace wm {

namespace {

std::vector<Window *> windowList; // back to front
std::unordered_map<gaddr, Window *> byRef;
std::vector<uint8_t> screen;
double lastPresent = 0;
bool layoutChanged = true;

const int kTitleBar = 22;

bool hasFrame(Window *w) {
	// A classic window's frame comes from its definition, whatever its class
	if (w->classic)
		return w->procID < 1 || w->procID > 3;
	switch (w->wclass) {
	case 1: case 2: case 3: case 4: case 6: case 8: case 15:
		return true;
	default:
		return false;
	}
}

qd::Rect structureBounds(Window *w) {
	qd::Rect r = w->bounds;
	if (hasFrame(w)) {
		r.top -= w->classic || w->wclass == 6 || w->wclass == 4 || w->wclass == 2 ? kTitleBar : 1;
		r.left -= 1;
		r.right += 1;
		r.bottom += 1;
	}
	return r;
}

} // namespace

std::vector<Window *> &windows() {
	return windowList;
}

Window *window(gaddr ref) {
	auto it = byRef.find(ref);
	return it == byRef.end() ? nullptr : it->second;
}

Window *windowForPort(gaddr port) {
	for (Window *w : windowList) {
		if (w->port->addr == port)
			return w;
	}
	return nullptr;
}

Window *frontWindow() {
	for (auto it = windowList.rbegin(); it != windowList.rend(); ++it) {
		if ((*it)->visible)
			return *it;
	}
	return nullptr;
}

Window *windowAt(int x, int y) {
	for (auto it = windowList.rbegin(); it != windowList.rend(); ++it) {
		if ((*it)->visible && structureBounds(*it).contains(y, x))
			return *it;
	}
	return nullptr;
}

void invalidate(Window *w, const qd::Rects &r) {
	w->invalid = qd::rgnOp(w->invalid, r, 0);
}

bool handles(const ev::Target &t, uint32_t eventClass, uint32_t kind) {
	for (auto &h : t.handlers)
		for (auto &type : h.types)
			if (type.first == eventClass && type.second == kind)
				return true;
	return false;
}

void updateWindows() {
	for (size_t i = 0; i < windowList.size(); i++) {
		Window *w = windowList[i];
		if (!w->visible || w->invalid.empty())
			continue;
		// Classic windows get update events from the event loop, unless a
		// kEventWindowUpdate handler takes them first
		if (w->classic && !handles(w->target, fourcc("wind"), 1))
			continue;
		w->invalid.clear();
		bool standard = (w->attrs & (1u << 25)) != 0;
		gaddr ref = ev::newEvent(fourcc("wind"), standard ? 2 : 1);
		ev::event(ref)->set(fourcc("----"), fourcc("wind"), &w->ref, 4);
		qd::Port *saved = qd::current();
		qd::setCurrent(w->port->addr);
		int32_t r = ev::send(ref, &w->target);
		if (r == eventNotHandledErr && !standard)
			drawControls(w);
		ev::releaseEvent(ref);
		if (saved)
			qd::setCurrent(saved->addr);
		if (std::find(windowList.begin(), windowList.end(), w) == windowList.end())
			break;
	}
}

static void blitWindow(Window *w, uint8_t *dst, size_t pitch) {
	int W = host::screenW, H = host::screenH;
	if (hasFrame(w)) {
		qd::Rect s = structureBounds(w);
		for (int y = std::max<int>(s.top, 0); y < std::min<int>(s.bottom, H); y++) {
			for (int x = std::max<int>(s.left, 0); x < std::min<int>(s.right, W); x++) {
				bool title = y < w->bounds.top - 1;
				uint8_t v = title ? (uint8_t)(0xE8 - (y - s.top) * 2) : 0x80;
				uint8_t *p = dst + (size_t)y * pitch + x * 4;
				p[0] = 0xFF;
				p[1] = p[2] = p[3] = v;
			}
		}
	}
	qd::Bits b = qd::portBits(w->port);
	for (int y = std::max<int>(w->bounds.top, 0); y < std::min<int>(w->bounds.bottom, H); y++) {
		int sy = y - w->bounds.top;
		if (sy >= b.bounds.height())
			break;
		int x0 = std::max<int>(w->bounds.left, 0), x1 = std::min<int>(w->bounds.right, W);
		x1 = std::min(x1, w->bounds.left + b.bounds.width());
		if (x1 <= x0)
			continue;
		if (b.depth == 32) {
			memcpy(dst + (size_t)y * pitch + x0 * 4, b.base + (size_t)sy * b.rowBytes + (x0 - w->bounds.left) * 4,
				(size_t)(x1 - x0) * 4);
		} else {
			for (int x = x0; x < x1; x++) {
				uint32_t c = qd::getPixel(b, x - w->bounds.left, sy);
				uint8_t *p = dst + (size_t)y * pitch + x * 4;
				p[0] = 0xFF;
				p[1] = (uint8_t)(c >> 16);
				p[2] = (uint8_t)(c >> 8);
				p[3] = (uint8_t)c;
			}
		}
	}
	w->port->dirty = false;
}

void presentScreen(bool force) {
	bool dirty = layoutChanged;
	for (Window *w : windowList)
		dirty = dirty || (w->visible && w->port->dirty);
	if (!dirty && !force)
		return;
	double now = host::now();
	if (!force && now - lastPresent < 1.0 / 60)
		return;
	lastPresent = now;
	layoutChanged = false;
	screen.assign((size_t)host::screenW * host::screenH * 4, 0);
	for (size_t i = 0; i < screen.size(); i += 4)
		screen[i] = 0xFF;
	for (Window *w : windowList) {
		if (w->visible)
			blitWindow(w, screen.data(), (size_t)host::screenW * 4);
	}
	host::present(screen.data());
}

void composeScreen(const qd::Bits &b) {
	if (b.depth != 32)
		return;
	for (int y = 0; y < host::screenH && y < b.bounds.height(); y++)
		memset(b.base + (size_t)y * b.rowBytes, 0, (size_t)host::screenW * 4);
	for (Window *w : windowList) {
		if (w->visible)
			blitWindow(w, b.base, b.rowBytes);
	}
}

} // namespace wm

using namespace ev;
using namespace wm;

// ---------------------------------------------------------------------------
// Event Manager imports

IMPORT(GetApplicationEventTarget) { c.ret(app.addr); }
IMPORT(GetWindowEventTarget) {
	Window *w = window(c.arg(0));
	c.ret(w ? w->target.addr : 0);
}
IMPORT(NewEventHandlerUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeEventHandlerUPP) {}
IMPORT(NewEventLoopTimerUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeEventLoopTimerUPP) {}
IMPORT(NewEventComparatorUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeEventComparatorUPP) {}

IMPORT(InstallEventHandler) {
	// (target, handler, numTypes, list, userData, outRef)
	Target *t = target(c.arg(0));
	if (!t) {
		logf("InstallEventHandler: unknown target 0x%08x", c.arg(0));
		c.ret((uint32_t)-50);
		return;
	}
	Target::Handler h;
	h.ref = heap::alloc(16, true);
	h.proc = c.arg(1);
	h.userData = c.arg(4);
	std::string desc;
	for (uint32_t i = 0; i < c.arg(2); i++) {
		uint32_t cls = emu::r32(c.arg(3) + 8 * i), kind = emu::r32(c.arg(3) + 8 * i + 4);
		h.types.push_back({cls, kind});
		desc += " " + fourccStr(cls) + "/" + std::to_string(kind);
	}
	logf("InstallEventHandler target 0x%08x proc 0x%08x:%s", t->addr, h.proc, desc.c_str());
	t->handlers.insert(t->handlers.begin(), h);
	handlerRefs[h.ref] = {t, h.ref};
	if (c.arg(5))
		emu::w32(c.arg(5), h.ref);
	c.ret(0);
}

IMPORT(AddEventTypesToHandler) {
	auto it = handlerRefs.find(c.arg(0));
	if (it == handlerRefs.end()) {
		c.ret((uint32_t)-50);
		return;
	}
	for (auto &h : it->second.first->handlers) {
		if (h.ref != c.arg(0))
			continue;
		for (uint32_t i = 0; i < c.arg(1); i++)
			h.types.push_back({emu::r32(c.arg(2) + 8 * i), emu::r32(c.arg(2) + 8 * i + 4)});
	}
	c.ret(0);
}

IMPORT(RemoveEventHandler) {
	auto it = handlerRefs.find(c.arg(0));
	if (it != handlerRefs.end()) {
		auto &hs = it->second.first->handlers;
		hs.erase(std::remove_if(hs.begin(), hs.end(), [&](const Target::Handler &h) { return h.ref == c.arg(0); }), hs.end());
		handlerRefs.erase(it);
	}
	c.ret(0);
}

IMPORT(CallNextEventHandler) {
	uint32_t idx = (c.arg(0) - kCallRefBase) / 4;
	if (idx >= frames.size()) {
		c.ret((uint32_t)eventNotHandledErr);
		return;
	}
	Frame f = frames[idx];
	c.ret((uint32_t)dispatchFrom(c.arg(1), f.target, f.chain, f.next));
}

IMPORT(SendEventToEventTarget) {
	Target *t = target(c.arg(1));
	if (!t) {
		logf("SendEventToEventTarget: unknown target 0x%08x", c.arg(1));
		c.ret((uint32_t)-50);
		return;
	}
	c.ret((uint32_t)send(c.arg(0), t));
}

IMPORT(CreateEvent) {
	// (allocator, class, kind, EventTime when, attributes, outEvent)
	gaddr ref = newEvent(c.arg(1), c.arg(2));
	double when = c.argd(3);
	if (when > 0)
		event(ref)->time = when;
	emu::w32(c.arg(6), ref);
	c.ret(0);
}
IMPORT(ReleaseEvent) { releaseEvent(c.arg(0)); }
IMPORT(GetEventClass) {
	Event *e = event(c.arg(0));
	c.ret(e ? e->eventClass : 0);
}
IMPORT(GetEventKind) {
	Event *e = event(c.arg(0));
	c.ret(e ? e->kind : 0);
}

IMPORT(SetEventParameter) {
	// (event, name, type, size, data)
	Event *e = event(c.arg(0));
	if (!e) {
		c.ret((uint32_t)-50);
		return;
	}
	e->set(c.arg(1), c.arg(2), emu::ptr(c.arg(4)), c.arg(3));
	c.ret(0);
}

IMPORT(GetEventParameter) {
	// (event, name, desiredType, outActualType, bufferSize, outActualSize, outData)
	Event *e = event(c.arg(0));
	const Param *p = e ? e->get(c.arg(1)) : nullptr;
	if (!p) {
		c.ret((uint32_t)eventParameterNotFoundErr);
		return;
	}
	uint32_t want = c.arg(2);
	std::vector<uint8_t> data = p->data;
	uint32_t type = p->type;
	if (want == fourcc("QDpt") && type == fourcc("hipt")) {
		float f[2];
		memcpy(f, data.data(), 8);
		int16_t pt[2] = {(int16_t)floorf(f[1]), (int16_t)floorf(f[0])};
		data.assign((uint8_t *)pt, (uint8_t *)pt + 4);
		type = want;
	} else if (want == fourcc("hipt") && type == fourcc("QDpt")) {
		int16_t pt[2];
		memcpy(pt, data.data(), 4);
		float f[2] = {(float)pt[1], (float)pt[0]};
		data.assign((uint8_t *)f, (uint8_t *)f + 8);
		type = want;
	} else if (want != fourcc("****") && want != type && type != fourcc("magn") && want != fourcc("magn")) {
		logf("GetEventParameter '%s': want '%s', have '%s'", fourccStr(c.arg(1)).c_str(), fourccStr(want).c_str(),
			fourccStr(type).c_str());
	}
	if (c.arg(3))
		emu::w32(c.arg(3), type);
	uint32_t n = std::min<uint32_t>(c.arg(4), (uint32_t)data.size());
	if (c.arg(6) && n)
		memcpy(emu::ptr(c.arg(6)), data.data(), n);
	if (c.arg(5))
		emu::w32(c.arg(5), (uint32_t)data.size());
	c.ret(0);
}

IMPORT(GetCurrentEventLoop) { c.ret(kEventLoop); }
IMPORT(GetMainEventQueue) { c.ret(kMainQueue); }
IMPORT(GetCurrentEventQueue) { c.ret(kMainQueue); }
IMPORT_F(GetCurrentEventTime) { c.retd(host::now()); }
IMPORT(GetCurrentKeyModifiers) { c.ret(host::modifiers); }

IMPORT(PostEventToQueue) {
	Event *e = event(c.arg(1));
	if (!e) {
		c.ret((uint32_t)-50);
		return;
	}
	e->refs++;
	if ((int16_t)c.arg(2) == 2) // kEventPriorityHigh
		queue.push_front(c.arg(1));
	else
		queue.push_back(c.arg(1));
	c.ret(0);
}

static bool typeMatch(Event *e, uint32_t numTypes, gaddr list) {
	if (!list || !numTypes)
		return true;
	for (uint32_t i = 0; i < numTypes; i++) {
		if (emu::r32(list + 8 * i) == e->eventClass && emu::r32(list + 8 * i + 4) == e->kind)
			return true;
	}
	return false;
}

IMPORT(ReceiveNextEvent) {
	// (numTypes, list, EventTimeout timeout, Boolean pull, EventRef *out)
	uint32_t numTypes = c.arg(0);
	gaddr list = c.arg(1);
	double timeout = c.argd(2);
	bool pull = (c.arg(4) & 0xFF) != 0;
	gaddr out = c.arg(5);
	double deadline = timeout < 0 ? 1e30 : host::now() + timeout;
	for (;;) {
		pumpInput();
		fireTimers();
		snd::serviceCallbacks();
		movie::taskAll();
		for (auto it = queue.begin(); it != queue.end(); ++it) {
			Event *e = event(*it);
			if (!e || !typeMatch(e, numTypes, list))
				continue;
			gaddr ref = *it;
			if (pull) {
				queue.erase(it); // the caller now owns our reference
			}
			if (out)
				emu::w32(out, ref);
			c.ret(0);
			return;
		}
		if (quitRequested) {
			c.ret((uint32_t)eventLoopQuitErr);
			return;
		}
		wm::updateWindows();
		wm::presentScreen();
		double now = host::now();
		if (now >= deadline) {
			c.ret((uint32_t)eventLoopTimedOutErr);
			return;
		}
		host::wait(std::min(deadline - now, std::min(nextTimer() - now, 1.0 / 60)));
	}
}

IMPORT(FlushEvents) {
	// Classic event masks: mouseDown 2, mouseUp 4, keyDown 8, keyUp 16, autoKey 32
	uint16_t mask = (uint16_t)c.arg(0);
	queue.erase(std::remove_if(queue.begin(), queue.end(), [&](gaddr ref) {
		Event *e = event(ref);
		if (!e)
			return true;
		bool hit = false;
		if (e->eventClass == fourcc("mous"))
			hit = (e->kind == 1 && (mask & 2)) || (e->kind == 2 && (mask & 4));
		else if (e->eventClass == fourcc("keyb"))
			hit = (e->kind == 1 && (mask & 8)) || (e->kind == 3 && (mask & 16)) || (e->kind == 2 && (mask & 32));
		if (hit)
			releaseEvent(ref);
		return hit;
	}), queue.end());
}

IMPORT(FlushSpecificEventsFromQueue) {
	// (queue, comparator, data)
	std::vector<gaddr> keep;
	std::deque<gaddr> q;
	q.swap(queue);
	for (gaddr ref : q) {
		bool match = (emu::call(c.arg(1), {ref, c.arg(2)}) & 0xFF) != 0;
		if (match)
			releaseEvent(ref);
		else
			queue.push_back(ref);
	}
	c.ret(0);
}

IMPORT(ConvertEventRefToEventRecord) {
	// EventRecord: what, message, when, where (v, h), modifiers; packed to 2
	Event *e = event(c.arg(0));
	gaddr rec = c.arg(1);
	memset(emu::ptr(rec), 0, 16);
	if (!e) {
		c.ret(0);
		return;
	}
	uint16_t what = 0;
	uint32_t message = 0;
	if (e->eventClass == fourcc("mous") && (e->kind == 1 || e->kind == 2))
		what = e->kind == 1 ? 1 : 2;
	else if (e->eventClass == fourcc("keyb")) {
		what = e->kind == 1 ? 3 : e->kind == 2 ? 5 : 4;
		uint32_t key = 0;
		if (const Param *p = e->get(fourcc("kcod")))
			memcpy(&key, p->data.data(), 4);
		message = (key << 8) | e->get(fourcc("kchr"))->data[0];
	}
	emu::w16(rec, what);
	emu::w32(rec + 2, message);
	emu::w32(rec + 6, (uint32_t)(e->time * 60));
	emu::w16(rec + 10, (uint16_t)host::mouseY);
	emu::w16(rec + 12, (uint16_t)host::mouseX);
	uint32_t mods = host::modifiers;
	if (const Param *p = e->get(fourcc("kmod")))
		memcpy(&mods, p->data.data(), 4);
	emu::w16(rec + 14, (uint16_t)(mods | (host::mouseDown ? 0 : 0x80)));
	c.ret(what != 0);
}

IMPORT(InstallEventLoopTimer) {
	// (loop, EventTimerInterval delay, EventTimerInterval interval, proc, userData, outTimer)
	Timer t;
	t.ref = heap::alloc(16, true);
	double delay = c.argd(1), interval = c.argd(3);
	t.proc = c.arg(5);
	t.userData = c.arg(6);
	t.interval = interval;
	t.next = delay < 0 ? 1e30 : host::now() + delay;
	timers[t.ref] = t;
	logf("InstallEventLoopTimer proc 0x%08x delay %g interval %g", t.proc, delay, interval);
	if (c.arg(7))
		emu::w32(c.arg(7), t.ref);
	c.ret(0);
}
IMPORT(RemoveEventLoopTimer) {
	timers.erase(c.arg(0));
	c.ret(0);
}
IMPORT(SetEventLoopTimerNextFireTime) {
	auto it = timers.find(c.arg(0));
	if (it != timers.end()) {
		double d = c.argd(1);
		it->second.next = d < 0 ? 1e30 : host::now() + d;
	}
	c.ret(0);
}

IMPORT(RunApplicationEventLoop) {
	// A launched application first gets the Open Application Apple event,
	// then becomes active
	static bool launched = false;
	if (!launched) {
		launched = true;
		gaddr oapp = newEvent(fourcc("eppc"), 1); // kEventAppleEvent
		uint32_t cls = fourcc("aevt"), id = fourcc("oapp");
		event(oapp)->set(fourcc("evcl"), fourcc("type"), &cls, 4);
		event(oapp)->set(fourcc("evti"), fourcc("type"), &id, 4);
		queue.push_back(oapp);
		queue.push_back(newEvent(fourcc("appl"), 1)); // kEventAppActivated
	}
	while (!quitRequested)
		runOnce(1.0);
}

IMPORT(RunAppModalLoopForWindow) {
	Window *w = window(c.arg(0));
	if (!w) {
		c.ret((uint32_t)-50);
		return;
	}
	logf("RunAppModalLoopForWindow 0x%08x", w->ref);
	w->modalDepth++;
	bool saved = w->modalQuit;
	w->modalQuit = false;
	while (!w->modalQuit && !quitRequested) {
		runOnce(1.0);
		if (!window(c.arg(0)))
			break;
	}
	if (window(c.arg(0))) {
		w->modalQuit = saved;
		w->modalDepth--;
	}
	if (quitRequested)
		_exit(0);
	c.ret(0);
}
IMPORT(QuitAppModalLoopForWindow) {
	if (Window *w = window(c.arg(0)))
		w->modalQuit = true;
	c.ret(0);
}

IMPORT(ProcessHICommand) {
	gaddr ref = newEvent(fourcc("cmds"), 1);
	event(ref)->set(fourcc("----"), fourcc("hcmd"), emu::ptr(c.arg(0)), 14);
	Window *w = frontWindow();
	c.ret((uint32_t)send(ref, w ? &w->target : &app));
	releaseEvent(ref);
}

// ---------------------------------------------------------------------------
// Windows

static Window *argWindow(Ctx &c, int n, const char *who) {
	Window *w = window(c.arg(n));
	if (!w)
		emu::fatal("%s: unknown window 0x%08x, called from 0x%08x", who, c.arg(n), emu::r32(c.esp));
	return w;
}

static Window *makeWindow(uint32_t wclass, uint32_t attrs, const qd::Rect &bounds) {
	Window *w = new Window;
	w->ref = heap::alloc(64, true);
	w->wclass = wclass;
	w->attrs = attrs;
	w->bounds = bounds;
	w->port = qd::newPort(std::max(bounds.width(), 1), std::max(bounds.height(), 1), 32, 0);
	w->port->window = w;
	w->target.parent = &app;
	newTarget(&w->target);
	byRef[w->ref] = w;
	// Floating and overlay classes stay above document windows
	windowList.push_back(w);
	layoutChanged = true;
	host::openWindow();
	return w;
}

namespace wm {
Window *createWindow(uint32_t wclass, uint32_t attrs, const qd::Rect &bounds) {
	return makeWindow(wclass, attrs, bounds);
}
}

IMPORT(CreateNewWindow) {
	// (class, attributes, const Rect *contentBounds, WindowRef *out)
	qd::Rect r = qd::readRect(c.arg(2));
	Window *w = makeWindow(c.arg(0), c.arg(1), r);
	logf("CreateNewWindow class %u attrs 0x%x (%d,%d)-(%d,%d) -> 0x%08x", c.arg(0), c.arg(1), r.left, r.top,
		r.right, r.bottom, w->ref);
	emu::w32(c.arg(3), w->ref);
	c.ret(0);
}

IMPORT(DisposeWindow) {
	Window *w = window(c.arg(0));
	if (!w)
		return;
	windowList.erase(std::remove(windowList.begin(), windowList.end(), w), windowList.end());
	byRef.erase(w->ref);
	w->port->window = nullptr;
	qd::disposePort(w->port);
	layoutChanged = true;
	// The ref stays allocated: the guest may still compare against it
}

IMPORT(GetWindowPort) {
	// No window: the screen, which some transitions read back
	c.ret(c.arg(0) ? argWindow(c, 0, "GetWindowPort")->port->addr : qd::screenPort()->addr);
}

IMPORT(ShowWindow) {
	Window *w = argWindow(c, 0, "ShowWindow");
	if (w->visible)
		return;
	w->visible = true;
	// A window shown after its siblings ends up in front of them
	windowList.erase(std::remove(windowList.begin(), windowList.end(), w), windowList.end());
	windowList.push_back(w);
	invalidate(w, {w->port->portRect});
	layoutChanged = true;
	gaddr ref = newEvent(fourcc("wind"), 5); // kEventWindowActivated
	event(ref)->set(fourcc("----"), fourcc("wind"), &w->ref, 4);
	send(ref, &w->target);
	releaseEvent(ref);
}
IMPORT(HideWindow) {
	Window *w = argWindow(c, 0, "HideWindow");
	w->visible = false;
	layoutChanged = true;
}
IMPORT(ShowHide) {
	Window *w = argWindow(c, 0, "ShowHide");
	if ((uint8_t)c.arg(1))
		imp_ShowWindow(c);
	else
		w->visible = false, layoutChanged = true;
}
// Windows are composited, so nothing behind a window needs repainting
IMPORT(CalcVisBehind) {}
IMPORT(PaintBehind) {}
IMPORT(GetWindowContentColor) {
	qd::writeRGB(c.arg(1), {0xFFFF, 0xFFFF, 0xFFFF});
	c.ret(0);
}
namespace sheets {
bool showSheet(gaddr dialog);
}

IMPORT(ShowSheetWindow) {
	if (!window(c.arg(0)) && sheets::showSheet(c.arg(0))) {
		c.ret(0);
		return;
	}
	Window *w = argWindow(c, 0, "ShowSheetWindow");
	Window *parent = window(c.arg(1));
	w->sheetParent = parent;
	if (parent) {
		// Sheets hang from the top of the parent, centered
		int width = w->bounds.width(), height = w->bounds.height();
		w->bounds.left = (int16_t)(parent->bounds.left + (parent->bounds.width() - width) / 2);
		w->bounds.top = parent->bounds.top;
		w->bounds.right = (int16_t)(w->bounds.left + width);
		w->bounds.bottom = (int16_t)(w->bounds.top + height);
	}
	windowList.erase(std::remove(windowList.begin(), windowList.end(), w), windowList.end());
	windowList.push_back(w);
	w->visible = true;
	invalidate(w, {w->port->portRect});
	layoutChanged = true;
	c.ret(0);
}
IMPORT(HideSheetWindow) {
	Window *w = window(c.arg(0));
	if (!w) {
		c.ret(0);
		return;
	}
	w->visible = false;
	layoutChanged = true;
	c.ret(0);
}
IMPORT(BringToFront) {
	Window *w = argWindow(c, 0, "BringToFront");
	windowList.erase(std::remove(windowList.begin(), windowList.end(), w), windowList.end());
	windowList.push_back(w);
	layoutChanged = true;
}
IMPORT(SendBehind) {
	Window *w = argWindow(c, 0, "SendBehind");
	logf("SendBehind 0x%08x behind 0x%08x", c.arg(0), c.arg(1));
	Window *behind = window(c.arg(1));
	windowList.erase(std::remove(windowList.begin(), windowList.end(), w), windowList.end());
	auto it = behind ? std::find(windowList.begin(), windowList.end(), behind) : windowList.begin();
	windowList.insert(it, w);
	layoutChanged = true;
}
IMPORT(SetUserFocusWindow) { c.ret(0); }
IMPORT(IsWindowActive) {
	Window *w = window(c.arg(0));
	c.ret(w && w == frontWindow());
}
IMPORT(IsWindowUpdatePending) {
	Window *w = window(c.arg(0));
	c.ret(w && !w->invalid.empty());
}

IMPORT(GetWindowBounds) {
	// (window, regionCode, Rect *out)
	Window *w = argWindow(c, 0, "GetWindowBounds");
	uint16_t code = (uint16_t)c.arg(1);
	qd::writeRect(c.arg(2), code == 32 ? structureBounds(w) : w->bounds);
	c.ret(0);
}
IMPORT(GetWindowRegion) {
	Window *w = argWindow(c, 0, "GetWindowRegion");
	uint16_t code = (uint16_t)c.arg(1);
	qd::setRgn(c.arg(2), {code == 32 ? structureBounds(w) : w->bounds});
	c.ret(0);
}
IMPORT(MoveWindow) {
	Window *w = argWindow(c, 0, "MoveWindow");
	logf("MoveWindow 0x%08x to %d,%d", c.arg(0), c.sarg16(1), c.sarg16(2));
	w->bounds = qd::offset(w->bounds, c.sarg16(1) - w->bounds.left, c.sarg16(2) - w->bounds.top);
	layoutChanged = true;
}
IMPORT(SizeWindow) {
	Window *w = argWindow(c, 0, "SizeWindow");
	int width = c.sarg16(1), height = c.sarg16(2);
	if (width == w->bounds.width() && height == w->bounds.height())
		return;
	w->bounds.right = (int16_t)(w->bounds.left + width);
	w->bounds.bottom = (int16_t)(w->bounds.top + height);
	qd::resizePort(w->port, width, height);
	invalidate(w, {w->port->portRect});
	layoutChanged = true;
}
IMPORT(GetWindowClass) {
	emu::w32(c.arg(1), argWindow(c, 0, "GetWindowClass")->wclass);
	c.ret(0);
}
IMPORT(ChangeWindowAttributes) {
	Window *w = argWindow(c, 0, "ChangeWindowAttributes");
	w->attrs = (w->attrs | c.arg(1)) & ~c.arg(2);
	c.ret(0);
}
IMPORT(SetWindowTitleWithCFString) {
	argWindow(c, 0, "SetWindowTitleWithCFString")->title = cf::utf8(c.arg(1));
	c.ret(0);
}
IMPORT(SetWRefCon) { argWindow(c, 0, "SetWRefCon")->refCon = c.arg(1); }
IMPORT(SetWindowContentColor) {
	argWindow(c, 0, "SetWindowContentColor")->contentColor = qd::argb(qd::readRGB(c.arg(1)));
	c.ret(0);
}
IMPORT(InvalWindowRect) {
	invalidate(argWindow(c, 0, "InvalWindowRect"), {qd::readRect(c.arg(1))});
	c.ret(0);
}
IMPORT(InvalWindowRgn) {
	invalidate(argWindow(c, 0, "InvalWindowRgn"), qd::rgnRects(c.arg(1)));
	c.ret(0);
}
IMPORT(ValidWindowRect) {
	Window *w = argWindow(c, 0, "ValidWindowRect");
	w->invalid = qd::rgnOp(w->invalid, {qd::readRect(c.arg(1))}, 2);
	c.ret(0);
}
IMPORT(ValidWindowRgn) {
	Window *w = argWindow(c, 0, "ValidWindowRgn");
	w->invalid = qd::rgnOp(w->invalid, qd::rgnRects(c.arg(1)), 2);
	c.ret(0);
}

IMPORT(CreateWindowGroup) {
	gaddr g = heap::alloc(16, true);
	logf("CreateWindowGroup attrs 0x%x -> 0x%08x", c.arg(0), g);
	emu::w32(c.arg(1), g);
	c.ret(0);
}
IMPORT(SetWindowGroup) {
	logf("SetWindowGroup 0x%08x -> 0x%08x", c.arg(0), c.arg(1));
	argWindow(c, 0, "SetWindowGroup")->group = c.arg(1);
	c.ret(0);
}
IMPORT(ReleaseWindowGroup) { c.ret(0); }

// ---------------------------------------------------------------------------
// Event Manager initialization

namespace ev {
void init() {
	newTarget(&app);
	kEventLoop = heap::alloc(16, true);
	kMainQueue = heap::alloc(16, true);
}
}

// ===========================================================================
// Classic Event Manager and Window Manager, for games written around
// WaitNextEvent. Input comes from the same queue as Carbon events.

namespace {

enum { nullEvent = 0, mouseDown = 1, mouseUp = 2, keyDown = 3, keyUp = 4, autoKey = 5, updateEvt = 6,
	activateEvt = 8 };

// Which classic event an queued Carbon event becomes (0 if none)
uint16_t classicKind(Event *e) {
	if (e->eventClass == fourcc("mous"))
		return e->kind == 1 ? mouseDown : e->kind == 2 ? mouseUp : 0;
	if (e->eventClass == fourcc("keyb"))
		return e->kind == 1 ? keyDown : e->kind == 2 ? autoKey : e->kind == 3 ? keyUp : 0;
	return 0;
}

void writeRecord(gaddr rec, uint16_t what, uint32_t message, double time, int x, int y, uint32_t mods) {
	memset(emu::ptr(rec), 0, 16);
	emu::w16(rec, what);
	emu::w32(rec + 2, message);
	emu::w32(rec + 6, (uint32_t)(time * 60));
	emu::w16(rec + 10, (uint16_t)y);
	emu::w16(rec + 12, (uint16_t)x);
	emu::w16(rec + 14, (uint16_t)(mods | (host::mouseDown ? 0 : 0x80))); // btnState: set when up
}

// Fills an EventRecord from a queued event
void toRecord(Event *e, uint16_t what, gaddr rec) {
	int x = host::mouseX, y = host::mouseY;
	if (const Param *p = e->get(fourcc("mloc"))) {
		float pt[2];
		memcpy(pt, p->data.data(), 8);
		x = (int)pt[0];
		y = (int)pt[1];
	}
	uint32_t mods = host::modifiers, message = 0;
	if (const Param *p = e->get(fourcc("kmod")))
		memcpy(&mods, p->data.data(), 4);
	if (e->eventClass == fourcc("keyb")) {
		uint32_t key = 0;
		if (const Param *p = e->get(fourcc("kcod")))
			memcpy(&key, p->data.data(), 4);
		message = (key << 8) | e->get(fourcc("kchr"))->data[0];
	} else if (what == mouseDown) {
		uint32_t clicks = 1;
		if (const Param *p = e->get(fourcc("ccnt")))
			memcpy(&clicks, p->data.data(), 4);
		message = clicks;
	}
	writeRecord(rec, what, message, e->time, x, y, mods);
}

// The window needing an update, front to back
Window *pendingUpdate() {
	for (auto it = windowList.rbegin(); it != windowList.rend(); ++it) {
		if ((*it)->classic && (*it)->visible && !(*it)->invalid.empty() && !wm::handles((*it)->target, fourcc("wind"), 1))
			return *it;
	}
	return nullptr;
}

bool nextClassicEvent(uint16_t mask, gaddr rec, bool remove) {
	for (auto it = queue.begin(); it != queue.end();) {
		Event *e = event(*it);
		uint16_t what = e ? classicKind(e) : 0;
		if (e && e->eventClass == fourcc("cmds")) {
			// Cmd-Q or the window's close box: to the game's command handler if
			// it has one, otherwise Cmd-Q for its menu handling
			gaddr ref = *it;
			it = queue.erase(it);
			bool handled = wm::handles(app, fourcc("cmds"), e->kind) && send(ref, &app) != eventNotHandledErr;
			releaseEvent(ref);
			if (handled)
				continue;
			if (mask & (1 << keyDown)) {
				writeRecord(rec, keyDown, (0x0C << 8) | 'q', host::now(), host::mouseX, host::mouseY, host::kCmd);
				return true;
			}
			continue;
		}
		if (!what) {
			// Mouse moves and the like have no classic form
			gaddr ref = *it;
			it = queue.erase(it);
			if (e)
				releaseEvent(ref);
			continue;
		}
		if (!(mask & (1 << what))) {
			++it;
			continue;
		}
		toRecord(e, what, rec);
		if (remove) {
			gaddr ref = *it;
			queue.erase(it);
			releaseEvent(ref);
		}
		return true;
	}
	if (mask & (1 << updateEvt)) {
		if (Window *w = pendingUpdate()) {
			writeRecord(rec, updateEvt, w->ref, host::now(), host::mouseX, host::mouseY, host::modifiers);
			return true;
		}
	}
	return false;
}

} // namespace

IMPORT(WaitNextEvent) {
	// (eventMask, EventRecord *, sleep ticks, mouseRgn)
	uint16_t mask = (uint16_t)c.arg(0);
	gaddr rec = c.arg(1);
	double deadline = host::now() + c.arg(2) / 60.0;
	for (;;) {
		pumpInput();
		fireTimers();
		snd::serviceCallbacks();
		movie::taskAll();
		wm::updateWindows();
		if (nextClassicEvent(mask, rec, true)) {
			c.ret(1);
			return;
		}
		wm::presentScreen();
		double now = host::now();
		if (now >= deadline || quitRequested) {
			writeRecord(rec, nullEvent, 0, now, host::mouseX, host::mouseY, host::modifiers);
			c.ret(0);
			return;
		}
		host::wait(std::min(deadline - now, std::min(nextTimer() - now, 1.0 / 60)));
	}
}
IMPORT(GetNextEvent) {
	pumpInput();
	gaddr rec = c.arg(1);
	if (nextClassicEvent((uint16_t)c.arg(0), rec, true)) {
		c.ret(1);
		return;
	}
	writeRecord(rec, nullEvent, 0, host::now(), host::mouseX, host::mouseY, host::modifiers);
	c.ret(0);
}
IMPORT(EventAvail) {
	pumpInput();
	gaddr rec = c.arg(1);
	if (nextClassicEvent((uint16_t)c.arg(0), rec, false)) {
		c.ret(1);
		return;
	}
	writeRecord(rec, nullEvent, 0, host::now(), host::mouseX, host::mouseY, host::modifiers);
	c.ret(0);
}
IMPORT(SetEventMask) {}

IMPORT(NewEventLoopIdleTimerUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeEventLoopIdleTimerUPP) {}
IMPORT(InstallEventLoopIdleTimer) {
	// Same arguments as InstallEventLoopTimer; the game is idle often enough
	// that a plain timer serves
	Timer t;
	t.ref = heap::alloc(16, true);
	double delay = c.argd(1), interval = c.argd(3);
	t.proc = c.arg(5);
	t.userData = c.arg(6);
	t.interval = interval;
	t.next = delay < 0 ? 1e30 : host::now() + delay;
	timers[t.ref] = t;
	logf("InstallEventLoopIdleTimer proc 0x%08x delay %g interval %g", t.proc, delay, interval);
	if (c.arg(7))
		emu::w32(c.arg(7), t.ref);
	c.ret(0);
}

// --- Windows

IMPORT(NewCWindow) {
	// (storage, const Rect *bounds, title, visible, procID, behind, goAwayFlag, refCon)
	qd::Rect r = qd::readRect(c.arg(1));
	int16_t procID = c.sarg16(4);
	// Plain boxes (procID 1-3) have no frame; anything else gets the document look
	bool plain = procID >= 1 && procID <= 3;
	Window *w = makeWindow(plain ? 13 : 6, 0, r);
	w->classic = true;
	w->procID = procID;
	w->title = cf::macRomanToUtf8(emu::pstr(c.arg(2)));
	w->refCon = c.arg(7);
	logf("NewCWindow procID %d (%d,%d)-(%d,%d) visible %d -> 0x%08x", procID, r.left, r.top, r.right, r.bottom,
		c.arg(3) & 0xFF, w->ref);
	if (c.arg(3) & 0xFF) {
		w->visible = true;
		invalidate(w, {w->port->portRect});
	}
	qd::setCurrent(w->port->addr);
	c.ret(w->ref);
}
IMPORT(SetWindowClass) {
	argWindow(c, 0, "SetWindowClass")->wclass = c.arg(1);
	layoutChanged = true;
	c.ret(0);
}
IMPORT(GetWRefCon) { c.ret(argWindow(c, 0, "GetWRefCon")->refCon); }
IMPORT(GetWVariant) { c.ret((uint32_t)(argWindow(c, 0, "GetWVariant")->procID & 15)); }
IMPORT(IsWindowVisible) { c.ret(argWindow(c, 0, "IsWindowVisible")->visible); }
IMPORT(FrontWindow) {
	Window *w = frontWindow();
	c.ret(w ? w->ref : 0);
}
IMPORT(FindWindow) {
	// (Point by value, WindowRef *): inDesk 0, inMenuBar 1, inContent 3
	int16_t v = (int16_t)(c.arg(0) & 0xFFFF), h = (int16_t)(c.arg(0) >> 16);
	Window *w = windowAt(h, v);
	if (c.arg(1))
		emu::w32(c.arg(1), w ? w->ref : 0);
	c.ret(w ? 3 : 0);
}
IMPORT(SetPortWindowPort) { qd::setCurrent(argWindow(c, 0, "SetPortWindowPort")->port->addr); }
IMPORT(BeginUpdate) {
	Window *w = argWindow(c, 0, "BeginUpdate");
	w->invalid.clear();
}
IMPORT(EndUpdate) {}
IMPORT(HiliteWindow) {}
IMPORT(DrawGrowIcon) {}
IMPORT(GrowWindow) { c.ret(0); }
IMPORT(ZoomWindow) {}
IMPORT(TrackBox) { c.ret(0); }
IMPORT(TrackGoAway) { c.ret(0); }
IMPORT(DragGrayRgn) { c.ret(0x80008000); }
IMPORT(CalcVis) {}
IMPORT(PaintOne) {
	if (Window *w = window(c.arg(0)))
		invalidate(w, {w->port->portRect});
}
IMPORT(SetWindowAlternateTitle) { c.ret(0); }
IMPORT(ChangeWindowGroupAttributes) { c.ret(0); }
