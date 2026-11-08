// Control Manager, Interface Builder nibs and the Data Browser list. Nib
// windows and their controls are built from objects.xib; controls draw in a
// plain Aqua-like style and turn clicks into kEventControlHit and
// kEventCommandProcess events, as the standard window handler does.
#include "cf.h"
#include "heap.h"
#include "host.h"
#include "res.h"
#include "text.h"
#include "window.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>

static uint32_t fourcc(const char *s) {
	return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3];
}

const int32_t errUnknownControl = -30584;
const int32_t errDataNotSupported = -30583;

namespace wm {

struct Control {
	gaddr ref = 0;
	Window *window = nullptr;
	Control *parent = nullptr;
	std::vector<Control *> kids;
	std::string kind; // nib class without the IBCarbon prefix
	qd::Rect bounds;  // window coordinates
	uint32_t sig = 0;
	int32_t id = 0;
	int32_t value = 0, min = 0, max = 1;
	bool visible = true, enabled = true, small = false;
	std::string title;
	std::u16string text; // edit and static text
	uint32_t command = 0;
	int buttonType = 0;
	int just = -2;
	int16_t pictID = 0;
	gaddr pict = 0;
	gaddr action = 0;
	bool hilite = false;
	ev::Target target;
	std::map<uint32_t, std::vector<uint8_t>> data; // other control data tags
	std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> props;
	std::vector<std::string> items; // radio titles, popup items
	// Data Browser
	std::vector<uint32_t> dbItems;
	std::vector<uint32_t> dbSelected;
	uint32_t dbProperty = 0;
	gaddr dbCallbacks = 0;
	int dbScroll = 0;
};

} // namespace wm

using namespace wm;

namespace {

std::unordered_map<gaddr, Control *> controls;
Control *pressed = nullptr;

Control *control(gaddr ref) {
	auto it = controls.find(ref);
	return it == controls.end() ? nullptr : it->second;
}

Control *newControl(Window *w, Control *parent, const std::string &kind, const qd::Rect &bounds) {
	Control *c = new Control;
	c->ref = heap::alloc(32, true);
	c->window = w;
	c->parent = parent;
	c->kind = kind;
	c->bounds = bounds;
	c->target.parent = parent ? &parent->target : &w->target;
	ev::newTarget(&c->target);
	if (parent)
		parent->kids.push_back(c);
	controls[c->ref] = c;
	return c;
}

bool shown(Control *c) {
	for (Control *x = c; x; x = x->parent) {
		if (!x->visible)
			return false;
	}
	return true;
}

bool active(Control *c) {
	for (Control *x = c; x; x = x->parent) {
		if (!x->enabled)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Drawing

const uint32_t kText = 0xFF000000u, kDisabledText = 0xFF8C8C8C, kFace = 0xFFF4F4F4, kEdge = 0xFF8E8E8E;

void bevel(qd::Port *p, const qd::Rect &r, bool down, bool isDefault) {
	uint32_t face = down ? 0xFFB8C8E8 : isDefault ? 0xFF8DB5EF : kFace;
	qd::fillRect(p, r, face);
	qd::frameRect(p, r, isDefault ? 0xFF4A78C8 : kEdge);
}

void drawOne(Control *c, qd::Port *p);

std::string dbText(Control *c, uint32_t item);

void drawKids(Control *c, qd::Port *p) {
	for (Control *k : c->kids) {
		if (k->visible)
			drawOne(k, p);
	}
}

void drawOne(Control *c, qd::Port *p) {
	bool on = active(c);
	uint32_t tc = on ? kText : kDisabledText;
	int fs = c->small ? 11 : 13;
	const qd::Rect &r = c->bounds;
	const std::string &k = c->kind;
	auto dataProc = [&](const char *tag) -> gaddr {
		auto it = c->data.find(fourcc(tag));
		if (it == c->data.end() || it->second.size() < 4)
			return 0;
		gaddr v;
		memcpy(&v, it->second.data(), 4);
		return v;
	};
	if (k == "UserPane" || k == "RootControl") {
		if (gaddr draw = dataProc("draw")) {
			qd::setCurrent(p->addr);
			emu::call(draw, {c->ref, 0});
		}
	} else if (k == "Button") {
		bevel(p, r, c->hilite, c->buttonType == 1);
		txt::drawLabel(p, c->title, r, 1, fs, false, c->buttonType == 1 && on ? 0xFFFFFFFFu : tc);
	} else if (k == "CheckBox" || k == "Radio") {
		qd::Rect box = r;
		box.top = (int16_t)(r.top + (r.height() - 12) / 2);
		box.bottom = (int16_t)(box.top + 12);
		box.right = (int16_t)(box.left + 12);
		qd::fillRect(p, box, c->hilite ? 0xFFB8C8E8 : 0xFFFFFFFFu);
		qd::frameRect(p, box, kEdge);
		if (c->value) {
			qd::Rect dot = box;
			dot.top += 3;
			dot.left += 3;
			dot.bottom -= 3;
			dot.right -= 3;
			qd::fillRect(p, dot, on ? 0xFF2A5DB0 : kDisabledText);
		}
		qd::Rect lr = r;
		lr.left = (int16_t)(box.right + 5);
		txt::drawLabel(p, c->title, lr, -2, fs, false, tc);
	} else if (k == "RadioGroup") {
		drawKids(c, p);
		return;
	} else if (k == "StaticText") {
		txt::drawLabel(p, c->text.empty() ? c->title : cf::utf8(c->text), r, c->just == -2 ? -2 : c->just, fs, false, tc);
	} else if (k == "EditText") {
		qd::fillRect(p, r, 0xFFFFFFFFu);
		qd::Rect o = r;
		o.top -= 1;
		o.left -= 1;
		o.bottom += 1;
		o.right += 1;
		bool focus = c->window->focus == c->ref;
		qd::frameRect(p, o, focus ? 0xFF6D9CE8 : kEdge, focus ? 2 : 1);
		std::string s = cf::utf8(c->text);
		qd::Rect tr = r;
		tr.left += 3;
		txt::drawLabel(p, s, tr, -2, fs, false, kText);
		if (focus) {
			int x = r.left + 3 + txt::labelWidth(s, fs) + 1;
			qd::Rect caret;
			caret.left = (int16_t)std::min<int>(x, r.right - 2);
			caret.right = (int16_t)(caret.left + 1);
			caret.top = (int16_t)(r.top + 2);
			caret.bottom = (int16_t)(r.bottom - 2);
			qd::fillRect(p, caret, kText);
		}
	} else if (k == "Picture") {
		gaddr pic = c->pict;
		if (!pic && c->pictID)
			pic = res::get(0x50494354, c->pictID); // 'PICT'
		if (pic)
			qd::drawPicture(p, pic, r);
	} else if (k == "GroupBox") {
		qd::Rect b = r;
		b.top += 8;
		qd::frameRect(p, b, 0xFFC8C8C8);
		if (!c->title.empty()) {
			qd::Rect t = r;
			t.left += 10;
			t.bottom = (int16_t)(r.top + 16);
			t.right = (int16_t)(t.left + txt::labelWidth(c->title, 11) + 6);
			qd::fillRect(p, t, argb(p->bg));
			txt::drawLabel(p, c->title, t, -2, 11, false, tc);
		}
	} else if (k == "Separator") {
		qd::Rect l = r;
		if (r.width() > r.height())
			l.bottom = (int16_t)(l.top + 1);
		else
			l.right = (int16_t)(l.left + 1);
		qd::fillRect(p, l, 0xFFC0C0C0);
	} else if (k == "Slider") {
		qd::Rect track = r;
		track.top = (int16_t)(r.top + r.height() / 2 - 2);
		track.bottom = (int16_t)(track.top + 4);
		qd::fillRect(p, track, 0xFFC8C8C8);
		int span = std::max(1, c->max - c->min);
		int x = r.left + 6 + (int)((int64_t)(c->value - c->min) * (r.width() - 12) / span);
		qd::Rect thumb;
		thumb.left = (int16_t)(x - 6);
		thumb.right = (int16_t)(x + 6);
		thumb.top = (int16_t)(r.top + r.height() / 2 - 8);
		thumb.bottom = (int16_t)(thumb.top + 16);
		bevel(p, thumb, c->hilite, false);
	} else if (k == "PopupButton") {
		bevel(p, r, c->hilite, false);
		std::string t = c->value >= 1 && c->value <= (int)c->items.size() ? c->items[c->value - 1] : c->title;
		qd::Rect tr = r;
		tr.left += 8;
		txt::drawLabel(p, t + "  ▾", tr, -2, fs, false, tc);
	} else if (k == "Tab") {
		qd::Rect body = r;
		body.top += 10;
		qd::fillRect(p, body, 0xFFE6E6E6);
		qd::frameRect(p, body, 0xFFB0B0B0);
		int n = std::max<int>(1, (int)c->items.size());
		int w = 90, x0 = r.left + (r.width() - n * w) / 2;
		for (int i = 0; i < (int)c->items.size(); i++) {
			qd::Rect t;
			t.left = (int16_t)(x0 + i * w);
			t.right = (int16_t)(t.left + w);
			t.top = r.top;
			t.bottom = (int16_t)(r.top + 21);
			bevel(p, t, c->value == i + 1, false);
			txt::drawLabel(p, c->items[i], t, 1, 11, c->value == i + 1, tc);
		}
	} else if (k == "DBListView") {
		qd::fillRect(p, r, 0xFFFFFFFFu);
		qd::frameRect(p, r, kEdge);
		int rowH = 18;
		for (size_t i = 0; i < c->dbItems.size(); i++) {
			qd::Rect row = r;
			row.top = (int16_t)(r.top + 1 + (int)i * rowH - c->dbScroll);
			row.bottom = (int16_t)(row.top + rowH);
			row.left += 1;
			row.right -= 1;
			if (row.bottom > r.bottom)
				break;
			bool sel = std::find(c->dbSelected.begin(), c->dbSelected.end(), c->dbItems[i]) != c->dbSelected.end();
			if (sel)
				qd::fillRect(p, row, 0xFF3875D7);
			qd::Rect tr = row;
			tr.left += 6;
			txt::drawLabel(p, dbText(c, c->dbItems[i]), tr, -2, 12, false, sel ? 0xFFFFFFFFu : kText);
		}
	} else if (k != "RootControl") {
		static std::map<std::string, bool> warned;
		if (!warned[k]) {
			warned[k] = true;
			logf("controls: no drawing for %s", k.c_str());
		}
	}
	drawKids(c, p);
}

// ---------------------------------------------------------------------------
// Data Browser item text through the item-data callback

std::unordered_map<gaddr, std::u16string> *itemDataText = nullptr;

std::string dbText(Control *c, uint32_t item) {
	if (!c->dbCallbacks)
		return std::string();
	gaddr itemData = emu::r32(c->dbCallbacks + 4);
	if (!itemData)
		return std::string();
	static std::unordered_map<gaddr, std::u16string> texts;
	itemDataText = &texts;
	gaddr data = heap::alloc(16, true);
	emu::w32(data, 0x44425444); // 'DBTD'
	texts[data].clear();
	emu::call(itemData, {c->ref, item, c->dbProperty ? c->dbProperty : fourcc("user"), data, 0});
	std::string s = cf::utf8(texts[data]);
	texts.erase(data);
	heap::free(data);
	return s;
}

// ---------------------------------------------------------------------------
// Events from controls

void sendHit(Control *c, int16_t part, uint32_t mods) {
	gaddr ref = ev::newEvent(fourcc("cntl"), 1); // kEventControlHit
	ev::Event *e = ev::event(ref);
	e->set(fourcc("----"), fourcc("ctrl"), &c->ref, 4);
	e->set(fourcc("cprt"), fourcc("cprt"), &part, 2);
	e->set(fourcc("kmod"), fourcc("magn"), &mods, 4);
	int32_t r = ev::send(ref, &c->target);
	ev::releaseEvent(ref);
	if (c->action)
		emu::call(c->action, {c->ref, (uint32_t)part});
	(void)r;
	if (c->command) {
		uint8_t cmd[14] = {};
		uint32_t attrs = 4; // kHICommandFromControl
		memcpy(cmd, &attrs, 4);
		memcpy(cmd + 4, &c->command, 4);
		memcpy(cmd + 8, &c->ref, 4);
		gaddr cref = ev::newEvent(fourcc("cmds"), 1); // kEventCommandProcess
		ev::event(cref)->set(fourcc("----"), fourcc("hcmd"), cmd, 14);
		ev::event(cref)->set(fourcc("kmod"), fourcc("magn"), &mods, 4);
		ev::send(cref, &c->target);
		ev::releaseEvent(cref);
	}
}

Control *hitTest(Control *c, int h, int v) {
	if (!c->visible || !active(c))
		return nullptr;
	for (auto it = c->kids.rbegin(); it != c->kids.rend(); ++it) {
		if (Control *hit = hitTest(*it, h, v))
			return hit;
	}
	if (c->kind == "RootControl" || c->kind == "StaticText" || c->kind == "Picture" || c->kind == "GroupBox" ||
		c->kind == "Separator" || c->kind == "RadioGroup")
		return nullptr;
	if (c->kind == "UserPane" && !c->data.count(fourcc("hitt")))
		return nullptr;
	return c->bounds.contains(v, h) ? c : nullptr;
}

void setSliderFromMouse(Control *c, int h) {
	int span = c->max - c->min;
	int w = std::max(1, c->bounds.width() - 12);
	int v = c->min + (int)((int64_t)(h - c->bounds.left - 6) * span / w);
	c->value = std::max(c->min, std::min(c->max, v));
}

void focusNext(Window *w) {
	std::vector<Control *> edits;
	std::function<void(Control *)> walk = [&](Control *c) {
		if (c->kind == "EditText" && shown(c) && active(c))
			edits.push_back(c);
		for (Control *k : c->kids)
			walk(k);
	};
	if (w->root)
		walk(w->root);
	if (edits.empty())
		return;
	size_t i = 0;
	for (; i < edits.size(); i++) {
		if (edits[i]->ref == w->focus)
			break;
	}
	w->focus = edits[(i + 1) % edits.size()]->ref;
	invalidate(w, {w->port->portRect});
}

Control *findButton(Control *c, bool cancel) {
	if (!c || !shown(c))
		return nullptr;
	if (c->kind == "Button" && active(c)) {
		if (!cancel && (c->buttonType == 1 || c->command == fourcc("ok  ")))
			return c;
		if (cancel && (c->buttonType == 2 || c->command == fourcc("not!")))
			return c;
	}
	for (Control *k : c->kids) {
		if (Control *b = findButton(k, cancel))
			return b;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// A tiny XML reader for objects.xib

struct XNode {
	std::string tag;
	std::map<std::string, std::string> attrs;
	std::string text;
	std::vector<std::unique_ptr<XNode>> kids;
	const XNode *child(const std::string &name) const {
		for (auto &k : kids) {
			auto it = k->attrs.find("name");
			if (it != k->attrs.end() && it->second == name)
				return k.get();
		}
		return nullptr;
	}
	std::string str(const std::string &name, const std::string &def = "") const {
		const XNode *n = child(name);
		return n ? n->text : def;
	}
	int num(const std::string &name, int def = 0) const {
		const XNode *n = child(name);
		return n ? atoi(n->text.c_str()) : def;
	}
	bool flag(const std::string &name) const { return str(name) == "TRUE"; }
};

std::string unescape(const std::string &s) {
	std::string o;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] != '&') {
			o += s[i];
			continue;
		}
		size_t semi = s.find(';', i);
		std::string ent = s.substr(i + 1, semi - i - 1);
		if (ent == "amp") o += '&';
		else if (ent == "lt") o += '<';
		else if (ent == "gt") o += '>';
		else if (ent == "quot") o += '"';
		else if (ent == "apos") o += '\'';
		else if (!ent.empty() && ent[0] == '#') {
			uint32_t cp = ent[1] == 'x' ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
			std::u16string u(1, (char16_t)cp);
			o += cf::utf8(u);
		}
		i = semi;
	}
	return o;
}

std::unique_ptr<XNode> parseXML(const std::string &s) {
	auto root = std::make_unique<XNode>();
	std::vector<XNode *> stack = {root.get()};
	size_t i = 0;
	while (i < s.size()) {
		size_t lt = s.find('<', i);
		if (lt == std::string::npos)
			break;
		if (lt > i && stack.size() > 1)
			stack.back()->text += unescape(s.substr(i, lt - i));
		size_t gt = s.find('>', lt);
		std::string tag = s.substr(lt + 1, gt - lt - 1);
		i = gt + 1;
		if (tag[0] == '?' || tag[0] == '!')
			continue;
		if (tag[0] == '/') {
			if (stack.size() > 1)
				stack.pop_back();
			continue;
		}
		bool selfClose = tag.back() == '/';
		if (selfClose)
			tag.pop_back();
		auto node = std::make_unique<XNode>();
		size_t sp = tag.find_first_of(" \t\n");
		node->tag = tag.substr(0, sp);
		while (sp != std::string::npos) {
			size_t eq = tag.find('=', sp);
			if (eq == std::string::npos)
				break;
			size_t ks = tag.find_first_not_of(" \t\n", sp);
			std::string key = tag.substr(ks, eq - ks);
			size_t q1 = tag.find('"', eq), q2 = tag.find('"', q1 + 1);
			node->attrs[key] = unescape(tag.substr(q1 + 1, q2 - q1 - 1));
			sp = q2 + 1 < tag.size() ? q2 + 1 : std::string::npos;
		}
		XNode *raw = node.get();
		stack.back()->kids.push_back(std::move(node));
		if (!selfClose)
			stack.push_back(raw);
	}
	return root;
}

struct Nib {
	std::unique_ptr<XNode> doc;
	std::map<std::string, const XNode *> byID;
	std::map<std::string, const XNode *> names;
};
std::unordered_map<gaddr, Nib *> nibs;

void indexNode(Nib *nib, const XNode *n) {
	auto it = n->attrs.find("id");
	if (it != n->attrs.end() && n->tag == "object")
		nib->byID[it->second] = n;
	for (auto &k : n->kids)
		indexNode(nib, k.get());
}

const XNode *resolve(Nib *nib, const XNode *n) {
	if (n && n->tag == "reference") {
		auto it = nib->byID.find(n->attrs.at("idRef"));
		return it == nib->byID.end() ? nullptr : it->second;
	}
	return n;
}

qd::Rect parseBounds(const std::string &s) {
	qd::Rect r;
	int t = 0, l = 0, b = 0, rt = 0;
	sscanf(s.c_str(), "%d %d %d %d", &t, &l, &b, &rt);
	r.top = (int16_t)t;
	r.left = (int16_t)l;
	r.bottom = (int16_t)b;
	r.right = (int16_t)rt;
	return r;
}

uint32_t ostype(const std::string &s) {
	if (s.size() < 4)
		return 0;
	return fourcc(s.c_str());
}

void buildControls(Nib *nib, Window *w, Control *parent, const XNode *node) {
	const XNode *subs = node->child("subviews");
	if (!subs)
		return;
	for (auto &k : subs->kids) {
		const XNode *n = resolve(nib, k.get());
		if (!n)
			continue;
		std::string cls = n->attrs.count("class") ? n->attrs.at("class") : "";
		if (cls.rfind("IBCarbon", 0) == 0)
			cls = cls.substr(8);
		Control *c = newControl(w, parent, cls, parseBounds(n->str("bounds")));
		c->id = n->num("controlID");
		c->sig = ostype(n->str("controlSignature"));
		c->command = ostype(n->str("command"));
		c->title = n->str("title");
		c->enabled = !n->flag("disabled");
		c->small = n->flag("small") || n->num("controlSize") == 1;
		c->buttonType = n->num("buttonType");
		c->just = n->num("justification", -2);
		c->pictID = (int16_t)n->num("contentResID");
		c->max = n->num("maximumValue", cls == "Slider" ? 100 : 1);
		c->min = n->num("minimumValue", 0);
		c->value = n->num("initialValue", n->num("value", 0));
		if (cls == "RadioGroup") {
			if (const XNode *titles = n->child("radioTitles")) {
				int i = 0;
				for (auto &t : titles->kids) {
					std::string title = t->kids.empty() ? "" : t->kids[0]->text;
					qd::Rect rb = c->bounds;
					int cellH = 18;
					rb.top = (int16_t)(c->bounds.top + i * (cellH + n->num("intercellHeight", 2)));
					rb.bottom = (int16_t)(rb.top + cellH);
					Control *radio = newControl(w, c, "Radio", rb);
					radio->title = title;
					radio->small = c->small;
					i++;
				}
				c->max = i;
			}
		}
		if (cls == "PopupButton") {
			if (const XNode *menu = n->child("menu")) {
				if (const XNode *items = menu->child("items")) {
					for (auto &it : items->kids) {
						const XNode *item = resolve(nib, it.get());
						if (item)
							c->items.push_back(item->str("title"));
					}
				}
			}
			c->value = 1;
			c->max = (int)c->items.size();
		}
		if (cls == "DBListView") {
			if (const XNode *cols = n->child("listViewColumns")) {
				if (!cols->kids.empty())
					c->dbProperty = ostype(cols->kids[0]->str("propertyID"));
			}
		}
		if (cls == "Tab") {
			c->value = 1;
			if (const XNode *tabs = n->child("tabs")) {
				for (auto &t : tabs->kids)
					c->items.push_back(t->str("title", t->text));
			}
		}
		if (cls == "EditText" && !w->focus)
			w->focus = c->ref;
		buildControls(nib, w, c, n);
	}
}

Window *windowFromNib(Nib *nib, const XNode *n) {
	qd::Rect r = parseBounds(n->str("windowRect"));
	uint32_t wclass = (uint32_t)n->num("carbonWindowClass", 6);
	uint32_t attrs = 1u << 25; // kWindowStandardHandlerAttribute
	if (n->num("windowPosition") >= 1 && wclass != 11) {
		int width = r.width(), height = r.height();
		r.left = (int16_t)((host::screenW - width) / 2);
		r.top = (int16_t)((host::screenH - height) / 3);
		r.right = (int16_t)(r.left + width);
		r.bottom = (int16_t)(r.top + height);
	}
	Window *w = createWindow(wclass, attrs, r);
	w->title = n->str("title");
	w->contentColor = 0xFFECECEC;
	qd::fillRect(w->port, w->port->portRect, w->contentColor);
	w->port->bg = {0xECEC, 0xECEC, 0xECEC};
	const XNode *root = resolve(nib, n->child("rootControl"));
	w->root = newControl(w, nullptr, "RootControl", w->port->portRect);
	if (root)
		buildControls(nib, w, w->root, root);
	return w;
}

Control *findByID(Control *c, uint32_t sig, int32_t id) {
	if (!c)
		return nullptr;
	if (c->id == id && (!sig || !c->sig || c->sig == sig))
		return c;
	for (Control *k : c->kids) {
		if (Control *f = findByID(k, sig, id))
			return f;
	}
	return nullptr;
}

} // namespace

namespace wm {

void drawControls(Window *w) {
	if (!w->root)
		return;
	qd::Port *saved = qd::current();
	qd::setCurrent(w->port->addr);
	drawOne(w->root, w->port);
	if (saved)
		qd::setCurrent(saved->addr);
}

bool trackControls(Window *w, int h, int v, uint32_t mods) {
	if (!w->root)
		return false;
	Control *c = hitTest(w->root, h, v);
	if (!c)
		return false;
	if (c->kind == "EditText") {
		w->focus = c->ref;
		invalidate(w, {w->port->portRect});
		return true;
	}
	if (c->kind == "UserPane" || c->kind == "DBListView" || c->kind == "Tab" || c->kind == "Slider") {
		// These act on the press itself
		if (c->kind == "DBListView") {
			int row = (v - c->bounds.top - 1 + c->dbScroll) / 18;
			if (row >= 0 && row < (int)c->dbItems.size()) {
				c->dbSelected = {c->dbItems[row]};
				if (gaddr notify = c->dbCallbacks ? emu::r32(c->dbCallbacks + 12) : 0)
					emu::call(notify, {c->ref, c->dbItems[row], 5}); // kDataBrowserItemSelected
			}
		} else if (c->kind == "Tab") {
			int n = std::max<int>(1, (int)c->items.size());
			int width = 90, x0 = c->bounds.left + (c->bounds.width() - n * width) / 2;
			int idx = (h - x0) / width + 1;
			if (idx >= 1 && idx <= n && v < c->bounds.top + 22) {
				c->value = idx;
				sendHit(c, 1, mods);
			}
		} else if (c->kind == "Slider") {
			setSliderFromMouse(c, h);
			pressed = c;
			c->hilite = true;
			sendHit(c, 129, mods);
		} else {
			sendHit(c, 1, mods);
		}
		invalidate(w, {w->port->portRect});
		return true;
	}
	pressed = c;
	c->hilite = true;
	invalidate(w, {c->bounds});
	// Track until the button comes up, the way HandleControlClick does
	while (host::mouseDown && !ev::quitRequested) {
		host::Input in;
		while (host::poll(in)) {
			if (in.kind == host::Input::MouseUp)
				break;
		}
		int lh = host::mouseX - w->bounds.left + w->port->portRect.left;
		int lv = host::mouseY - w->bounds.top + w->port->portRect.top;
		bool inside = c->bounds.contains(lv, lh);
		if (inside != c->hilite) {
			c->hilite = inside;
			invalidate(w, {c->bounds});
		}
		updateWindows();
		presentScreen();
		host::wait(1.0 / 60);
	}
	bool inside = c->hilite;
	c->hilite = false;
	pressed = nullptr;
	invalidate(w, {w->port->portRect});
	if (!inside)
		return true;
	int16_t part = 10; // kControlButtonPart
	if (c->kind == "CheckBox") {
		c->value = !c->value;
		part = 11;
	} else if (c->kind == "Radio") {
		Control *group = c->parent;
		int i = 1;
		for (Control *k : group->kids) {
			k->value = k == c;
			if (k == c)
				group->value = i;
			i++;
		}
		c = group;
		part = 12;
	} else if (c->kind == "PopupButton") {
		c->value = c->value % std::max(1, c->max) + 1;
		part = 129;
	}
	sendHit(c, part, mods);
	return true;
}

bool keyToControls(Window *w, uint8_t ch, uint16_t key, uint32_t mods) {
	if (!w->root)
		return false;
	if (ch == 13 || ch == 3) {
		if (Control *b = findButton(w->root, false)) {
			sendHit(b, 10, mods);
			return true;
		}
	}
	if (ch == 27 || (ch == '.' && (mods & host::kCmd))) {
		if (Control *b = findButton(w->root, true)) {
			sendHit(b, 10, mods);
			return true;
		}
	}
	if (ch == 9) {
		focusNext(w);
		return true;
	}
	Control *f = control(w->focus);
	if (!f || f->kind != "EditText" || !shown(f))
		return false;
	if (mods & host::kCmd)
		return false;
	if (ch == 8 || ch == 127) {
		if (!f->text.empty())
			f->text.pop_back();
	} else if (ch >= 32) {
		std::u16string u = cf::utf16(cf::macRomanToUtf8(std::string(1, (char)ch)));
		f->text += u;
	} else {
		return false;
	}
	invalidate(w, {w->port->portRect});
	(void)key;
	return true;
}

} // namespace wm

// ---------------------------------------------------------------------------
// Nib imports

IMPORT(CreateNibReference) {
	std::string name = cf::utf8(c.arg(0));
	std::string path = host::gameApp + "/Contents/Resources/" + name + ".nib/objects.xib";
	std::ifstream f(path);
	if (!f) {
		logf("CreateNibReference: no %s", path.c_str());
		c.ret((uint32_t)-43);
		return;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	Nib *nib = new Nib;
	nib->doc = parseXML(ss.str());
	indexNode(nib, nib->doc.get());
	// The name table pairs strings with references
	std::function<void(const XNode *)> findTable = [&](const XNode *n) {
		auto it = n->attrs.find("name");
		if (n->tag == "dictionary" && it != n->attrs.end() && it->second == "nameTable") {
			for (size_t i = 0; i + 1 < n->kids.size(); i += 2)
				nib->names[n->kids[i]->text] = resolve(nib, n->kids[i + 1].get());
			return;
		}
		for (auto &k : n->kids)
			findTable(k.get());
	};
	findTable(nib->doc.get());
	gaddr ref = heap::alloc(16, true);
	nibs[ref] = nib;
	emu::w32(c.arg(1), ref);
	c.ret(0);
}
IMPORT(CreateNibReferenceWithCFBundle) {
	// (bundle, name, out): same nib lookup in the main bundle
	Ctx c2 = c;
	c2.esp = c.esp + 4;
	imp_CreateNibReference(c2);
}
IMPORT(DisposeNibReference) {}

IMPORT(CreateWindowFromNib) {
	auto it = nibs.find(c.arg(0));
	std::string name = cf::utf8(c.arg(1));
	if (it == nibs.end() || !it->second->names.count(name) || !it->second->names[name]) {
		logf("CreateWindowFromNib: no window %s", name.c_str());
		c.ret((uint32_t)-43);
		return;
	}
	Window *w = windowFromNib(it->second, it->second->names[name]);
	logf("CreateWindowFromNib %s -> 0x%08x", name.c_str(), w->ref);
	emu::w32(c.arg(2), w->ref);
	c.ret(0);
}

IMPORT(SetMenuBarFromNib) { c.ret(0); }
IMPORT(FindControl) {
	// (Point, window, ControlHandle *): the game draws its own buttons
	if (c.arg(2))
		emu::w32(c.arg(2), 0);
	c.ret(0);
}

// ---------------------------------------------------------------------------
// Control imports

static Control *argControl(Ctx &c, int n, const char *who) {
	Control *ctl = control(c.arg(n));
	if (!ctl)
		emu::fatal("%s: unknown control 0x%08x, called from 0x%08x", who, c.arg(n), emu::r32(c.esp));
	return ctl;
}

static void changed(Control *c) {
	invalidate(c->window, {c->window->port->portRect});
}

// The game links these classic calls but creates no controls of its own, so
// they only ever see controls from nibs (none here) or null
IMPORT(DisposeControl) {}
IMPORT(ShowControl) {
	if (Control *ctl = control(c.arg(0))) {
		ctl->visible = true;
		changed(ctl);
	}
}
IMPORT(HideControl) {
	if (Control *ctl = control(c.arg(0))) {
		ctl->visible = false;
		changed(ctl);
	}
}
IMPORT(IsControlVisible) {
	Control *ctl = control(c.arg(0));
	c.ret(ctl && ctl->visible);
}
IMPORT(MoveControl) {}
IMPORT(TrackControl) { c.ret(0); }
IMPORT(GetControlTitle) {
	if (c.arg(1))
		emu::w8(c.arg(1), 0);
}
IMPORT(SetControlTitle) {}

IMPORT(GetControlByID) {
	Window *w = window(c.arg(0));
	uint32_t sig = emu::r32(c.arg(1));
	int32_t id = (int32_t)emu::r32(c.arg(1) + 4);
	Control *ctl = w ? findByID(w->root, sig, id) : nullptr;
	if (!ctl) {
		logf("GetControlByID('%c%c%c%c', %d): not found", sig >> 24, sig >> 16, sig >> 8, sig, id);
		emu::w32(c.arg(2), 0);
		c.ret((uint32_t)errUnknownControl);
		return;
	}
	emu::w32(c.arg(2), ctl->ref);
	c.ret(0);
}
IMPORT(GetControlID) {
	Control *ctl = argControl(c, 0, "GetControlID");
	emu::w32(c.arg(1), ctl->sig);
	emu::w32(c.arg(1) + 4, (uint32_t)ctl->id);
	c.ret(0);
}
IMPORT(GetControlEventTarget) { c.ret(argControl(c, 0, "GetControlEventTarget")->target.addr); }
IMPORT(GetSuperControl) {
	Control *ctl = argControl(c, 0, "GetSuperControl");
	emu::w32(c.arg(1), ctl->parent ? ctl->parent->ref : 0);
	c.ret(ctl->parent ? 0 : (uint32_t)errUnknownControl);
}
IMPORT(GetControlValue) { c.ret((uint16_t)argControl(c, 0, "GetControlValue")->value); }
IMPORT(GetControl32BitValue) { c.ret((uint32_t)argControl(c, 0, "GetControl32BitValue")->value); }
static void setValue(Control *ctl, int32_t v) {
	v = std::max(ctl->min, std::min(ctl->max, v));
	ctl->value = v;
	if (ctl->kind == "RadioGroup") {
		int i = 1;
		for (Control *k : ctl->kids)
			k->value = i++ == v;
	}
	changed(ctl);
}
IMPORT(SetControlValue) { setValue(argControl(c, 0, "SetControlValue"), c.sarg16(1)); }
IMPORT(SetControl32BitValue) { setValue(argControl(c, 0, "SetControl32BitValue"), c.sarg(1)); }
IMPORT(SetControl32BitMinimum) { argControl(c, 0, "SetControl32BitMinimum")->min = c.sarg(1); }
IMPORT(SetControl32BitMaximum) { argControl(c, 0, "SetControl32BitMaximum")->max = c.sarg(1); }
IMPORT(EnableControl) {
	Control *ctl = argControl(c, 0, "EnableControl");
	ctl->enabled = true;
	changed(ctl);
	c.ret(0);
}
IMPORT(DisableControl) {
	Control *ctl = argControl(c, 0, "DisableControl");
	ctl->enabled = false;
	changed(ctl);
	c.ret(0);
}
IMPORT(IsControlEnabled) { c.ret(active(argControl(c, 0, "IsControlEnabled"))); }
IMPORT(IsControlActive) { c.ret(active(argControl(c, 0, "IsControlActive"))); }
IMPORT(SetControlVisibility) {
	Control *ctl = argControl(c, 0, "SetControlVisibility");
	ctl->visible = (c.arg(1) & 0xFF) != 0;
	changed(ctl);
	c.ret(0);
}
IMPORT(GetControlBounds) {
	qd::writeRect(c.arg(1), argControl(c, 0, "GetControlBounds")->bounds);
	c.ret(c.arg(1));
}
IMPORT(SizeControl) {
	Control *ctl = argControl(c, 0, "SizeControl");
	ctl->bounds.right = (int16_t)(ctl->bounds.left + c.sarg16(1));
	ctl->bounds.bottom = (int16_t)(ctl->bounds.top + c.sarg16(2));
	changed(ctl);
}
IMPORT(Draw1Control) {
	Control *ctl = argControl(c, 0, "Draw1Control");
	if (shown(ctl))
		drawOne(ctl, ctl->window->port);
}
IMPORT(NewControlActionUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeControlActionUPP) {}
IMPORT(NewControlKeyFilterUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeControlKeyFilterUPP) {}
IMPORT(NewControlEditTextValidationUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeControlEditTextValidationUPP) {}
IMPORT(SetControlAction) { argControl(c, 0, "SetControlAction")->action = c.arg(1); }
IMPORT(SetKeyboardFocus) {
	// (window, control, part)
	Window *w = window(c.arg(0));
	if (w) {
		w->focus = (int16_t)c.arg(2) ? c.arg(1) : 0;
		invalidate(w, {w->port->portRect});
	}
	c.ret(0);
}

IMPORT(SetControlData) {
	// (control, part, tag, size, data)
	Control *ctl = argControl(c, 0, "SetControlData");
	uint32_t tag = c.arg(2), size = c.arg(3);
	gaddr data = c.arg(4);
	if (tag == fourcc("cfst")) {
		ctl->text = cf::utf16(cf::utf8(emu::r32(data)));
	} else if (tag == fourcc("text")) {
		ctl->text = cf::utf16(cf::macRomanToUtf8(std::string((const char *)emu::ptr(data), size)));
	} else if (tag == fourcc("pich")) {
		ctl->pict = emu::r32(data);
	} else {
		ctl->data[tag].assign(emu::ptr(data), emu::ptr(data) + size);
	}
	changed(ctl);
	c.ret(0);
}
IMPORT(GetControlDataSize) {
	Control *ctl = argControl(c, 0, "GetControlDataSize");
	uint32_t tag = c.arg(2);
	uint32_t size = 0;
	if (tag == fourcc("cfst") || tag == fourcc("pich"))
		size = 4;
	else if (tag == fourcc("text"))
		size = (uint32_t)cf::utf8ToMacRoman(cf::utf8(ctl->text)).size();
	else if (ctl->data.count(tag))
		size = (uint32_t)ctl->data[tag].size();
	else {
		c.ret((uint32_t)errDataNotSupported);
		return;
	}
	emu::w32(c.arg(3), size);
	c.ret(0);
}
IMPORT(GetControlData) {
	// (control, part, tag, bufferSize, buffer, actualSize)
	Control *ctl = argControl(c, 0, "GetControlData");
	uint32_t tag = c.arg(2), size = c.arg(3);
	gaddr buf = c.arg(4);
	std::vector<uint8_t> out;
	if (tag == fourcc("cfst")) {
		gaddr s = cf::makeString(ctl->text);
		out.assign((uint8_t *)&s, (uint8_t *)&s + 4);
	} else if (tag == fourcc("text")) {
		std::string mr = cf::utf8ToMacRoman(cf::utf8(ctl->text));
		out.assign(mr.begin(), mr.end());
	} else if (tag == fourcc("pich")) {
		out.assign((uint8_t *)&ctl->pict, (uint8_t *)&ctl->pict + 4);
	} else if (ctl->data.count(tag)) {
		out = ctl->data[tag];
	} else {
		c.ret((uint32_t)errDataNotSupported);
		return;
	}
	if (buf)
		memcpy(emu::ptr(buf), out.data(), std::min<size_t>(size, out.size()));
	if (c.arg(5))
		emu::w32(c.arg(5), (uint32_t)out.size());
	c.ret(0);
}

IMPORT(SetControlProperty) {
	// (control, creator, tag, size, data)
	Control *ctl = argControl(c, 0, "SetControlProperty");
	ctl->props[{c.arg(1), c.arg(2)}].assign(emu::ptr(c.arg(4)), emu::ptr(c.arg(4)) + c.arg(3));
	c.ret(0);
}
IMPORT(GetControlProperty) {
	// (control, creator, tag, bufferSize, actualSize, buffer)
	Control *ctl = argControl(c, 0, "GetControlProperty");
	auto it = ctl->props.find({c.arg(1), c.arg(2)});
	if (it == ctl->props.end()) {
		c.ret((uint32_t)-5603); // errControlPropertyNotFound
		return;
	}
	if (c.arg(4))
		emu::w32(c.arg(4), (uint32_t)it->second.size());
	if (c.arg(5))
		memcpy(emu::ptr(c.arg(5)), it->second.data(), std::min<size_t>(c.arg(3), it->second.size()));
	c.ret(0);
}

// ---------------------------------------------------------------------------
// Data Browser

#define DB_UPP(name) \
	IMPORT(New##name##UPP) { c.ret(c.arg(0)); } \
	IMPORT(Dispose##name##UPP) {}
DB_UPP(DataBrowserItemData)
DB_UPP(DataBrowserItemCompare)
DB_UPP(DataBrowserItemNotification)
DB_UPP(DataBrowserAddDragItem)
DB_UPP(DataBrowserAcceptDrag)
DB_UPP(DataBrowserReceiveDrag)
DB_UPP(DataBrowserPostProcessDrag)
DB_UPP(DataBrowserGetContextualMenu)
DB_UPP(DataBrowserSelectContextualMenu)
DB_UPP(DataBrowserItemHelpContent)

IMPORT(InitDataBrowserCallbacks) {
	gaddr cb = c.arg(0);
	uint32_t version = emu::r32(cb);
	memset(emu::ptr(cb + 4), 0, 4 * 10);
	emu::w32(cb, version);
	c.ret(0);
}
IMPORT(GetDataBrowserCallbacks) {
	Control *ctl = argControl(c, 0, "GetDataBrowserCallbacks");
	if (ctl->dbCallbacks)
		memcpy(emu::ptr(c.arg(1) + 4), emu::ptr(ctl->dbCallbacks + 4), 40);
	c.ret(0);
}
IMPORT(SetDataBrowserCallbacks) {
	Control *ctl = argControl(c, 0, "SetDataBrowserCallbacks");
	if (!ctl->dbCallbacks)
		ctl->dbCallbacks = heap::alloc(48, true);
	memcpy(emu::ptr(ctl->dbCallbacks), emu::ptr(c.arg(1)), 44);
	c.ret(0);
}
IMPORT(AddDataBrowserItems) {
	// (browser, container, numItems, items, preSortProperty)
	Control *ctl = argControl(c, 0, "AddDataBrowserItems");
	for (uint32_t i = 0; i < c.arg(2); i++) {
		uint32_t item = c.arg(3) ? emu::r32(c.arg(3) + 4 * i) : i + 1;
		ctl->dbItems.push_back(item);
		if (gaddr notify = ctl->dbCallbacks ? emu::r32(ctl->dbCallbacks + 12) : 0)
			emu::call(notify, {ctl->ref, item, 1}); // kDataBrowserItemAdded
	}
	changed(ctl);
	c.ret(0);
}
IMPORT(RemoveDataBrowserItems) {
	// (browser, container, numItems, items, preSortProperty)
	Control *ctl = argControl(c, 0, "RemoveDataBrowserItems");
	if (c.arg(2) == 0) {
		ctl->dbItems.clear();
		ctl->dbSelected.clear();
	} else {
		for (uint32_t i = 0; i < c.arg(2); i++) {
			uint32_t item = emu::r32(c.arg(3) + 4 * i);
			ctl->dbItems.erase(std::remove(ctl->dbItems.begin(), ctl->dbItems.end(), item), ctl->dbItems.end());
			ctl->dbSelected.erase(std::remove(ctl->dbSelected.begin(), ctl->dbSelected.end(), item), ctl->dbSelected.end());
		}
	}
	changed(ctl);
	c.ret(0);
}
IMPORT(UpdateDataBrowserItems) {
	changed(argControl(c, 0, "UpdateDataBrowserItems"));
	c.ret(0);
}
IMPORT(SetDataBrowserSelectedItems) {
	// (browser, numItems, items, operation: 0 set, 1 add, 2 remove, 3 toggle)
	Control *ctl = argControl(c, 0, "SetDataBrowserSelectedItems");
	uint32_t op = c.arg(3);
	if (op == 0)
		ctl->dbSelected.clear();
	for (uint32_t i = 0; i < c.arg(1); i++) {
		uint32_t item = emu::r32(c.arg(2) + 4 * i);
		auto it = std::find(ctl->dbSelected.begin(), ctl->dbSelected.end(), item);
		if (op == 2 || (op == 3 && it != ctl->dbSelected.end()))
			ctl->dbSelected.erase(std::remove(ctl->dbSelected.begin(), ctl->dbSelected.end(), item), ctl->dbSelected.end());
		else if (it == ctl->dbSelected.end())
			ctl->dbSelected.push_back(item);
	}
	changed(ctl);
	c.ret(0);
}
IMPORT(SetDataBrowserSelectionFlags) { c.ret(0); }
IMPORT(GetDataBrowserPropertyFlags) {
	emu::w32(c.arg(2), 0);
	c.ret(0);
}
IMPORT(SetDataBrowserPropertyFlags) { c.ret(0); }
IMPORT(RemoveDataBrowserTableViewColumn) { c.ret(0); }
IMPORT(SetDataBrowserItemDataText) {
	if (itemDataText) {
		auto it = itemDataText->find(c.arg(0));
		if (it != itemDataText->end())
			it->second = cf::utf16(cf::utf8(c.arg(1)));
	}
	c.ret(0);
}
