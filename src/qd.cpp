// QuickDraw ports, GWorlds, regions, colors and CopyBits
#include "qd.h"
#include "heap.h"
#include "host.h"
#include "window.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace qd {

namespace {

const uint32_t kPortMagic = 0x504F5254; // 'PORT'
const uint32_t kPortSize = 256;
const uint32_t kPixMapSize = 50;

std::unordered_map<gaddr, Port *> ports;
std::unordered_map<gaddr, Port *> portsByPixMap; // PixMap pointer -> port
std::unordered_map<gaddr, Rects> regions;
Port *thePort = nullptr;
gaddr theDevice = 0;
gaddr screenPixMapH = 0;
Port *theScreen = nullptr;

} // namespace

Rect readRect(gaddr a) {
	Rect r;
	r.top = (int16_t)emu::r16(a);
	r.left = (int16_t)emu::r16(a + 2);
	r.bottom = (int16_t)emu::r16(a + 4);
	r.right = (int16_t)emu::r16(a + 6);
	return r;
}

void writeRect(gaddr a, const Rect &r) {
	emu::w16(a, (uint16_t)r.top);
	emu::w16(a + 2, (uint16_t)r.left);
	emu::w16(a + 4, (uint16_t)r.bottom);
	emu::w16(a + 6, (uint16_t)r.right);
}

Rect sect(const Rect &a, const Rect &b) {
	Rect r;
	r.top = std::max(a.top, b.top);
	r.left = std::max(a.left, b.left);
	r.bottom = std::min(a.bottom, b.bottom);
	r.right = std::min(a.right, b.right);
	if (r.empty())
		r = Rect();
	return r;
}

Rect offset(const Rect &r, int dh, int dv) {
	Rect o = r;
	o.left += dh;
	o.right += dh;
	o.top += dv;
	o.bottom += dv;
	return o;
}

// ---------------------------------------------------------------------------
// Regions

Rect bounds(const Rects &rs) {
	Rect b;
	bool first = true;
	for (const Rect &r : rs) {
		if (r.empty())
			continue;
		if (first) {
			b = r;
			first = false;
		} else {
			b.top = std::min(b.top, r.top);
			b.left = std::min(b.left, r.left);
			b.bottom = std::max(b.bottom, r.bottom);
			b.right = std::max(b.right, r.right);
		}
	}
	return b;
}

static bool covers(const Rects &rs, int v, int h) {
	for (const Rect &r : rs) {
		if (r.contains(v, h))
			return true;
	}
	return false;
}

Rects rgnOp(const Rects &a, const Rects &b, int op) {
	// Split the plane at every edge, decide each cell, then merge cells
	// into rows of spans
	std::vector<int16_t> xs, ys;
	for (const Rects *s : {&a, &b}) {
		for (const Rect &r : *s) {
			if (r.empty())
				continue;
			xs.push_back(r.left);
			xs.push_back(r.right);
			ys.push_back(r.top);
			ys.push_back(r.bottom);
		}
	}
	std::sort(xs.begin(), xs.end());
	xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
	std::sort(ys.begin(), ys.end());
	ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
	Rects out;
	std::vector<std::pair<int16_t, int16_t>> prevSpans;
	std::vector<size_t> prevIdx;
	for (size_t j = 0; j + 1 < ys.size(); j++) {
		std::vector<std::pair<int16_t, int16_t>> spans;
		for (size_t i = 0; i + 1 < xs.size(); i++) {
			bool ia = covers(a, ys[j], xs[i]), ib = covers(b, ys[j], xs[i]);
			bool in = op == 0 ? (ia || ib) : op == 1 ? (ia && ib) : op == 2 ? (ia && !ib) : (ia != ib);
			if (!in)
				continue;
			if (!spans.empty() && spans.back().second == xs[i])
				spans.back().second = xs[i + 1];
			else
				spans.push_back({xs[i], xs[i + 1]});
		}
		// Extend the rectangles of the row above when the spans match
		std::vector<size_t> idx;
		if (spans == prevSpans) {
			for (size_t k : prevIdx)
				out[k].bottom = ys[j + 1];
			idx = prevIdx;
		} else {
			for (auto &s : spans) {
				Rect r;
				r.top = ys[j];
				r.bottom = ys[j + 1];
				r.left = s.first;
				r.right = s.second;
				idx.push_back(out.size());
				out.push_back(r);
			}
		}
		prevSpans = spans;
		prevIdx = idx;
	}
	return out;
}

gaddr newRgn() {
	gaddr h = heap::newHandle(10, true);
	emu::w16(heap::deref(h), 10);
	regions[h] = Rects();
	return h;
}

void disposeRgn(gaddr rgn) {
	if (regions.erase(rgn))
		heap::disposeHandle(rgn);
}

const Rects &rgnRects(gaddr rgn) {
	static Rects none;
	auto it = regions.find(rgn);
	return it == regions.end() ? none : it->second;
}

void setRgn(gaddr rgn, const Rects &rects) {
	Rects clean;
	for (const Rect &r : rects) {
		if (!r.empty())
			clean.push_back(r);
	}
	regions[rgn] = clean;
	Rect b = bounds(clean);
	gaddr p = heap::deref(rgn);
	emu::w16(p, clean.size() > 1 ? 28 : 10);
	writeRect(p + 2, b);
}

// ---------------------------------------------------------------------------
// Pixels

bool readBits(gaddr bm, Bits &b) {
	if (!bm || !emu::valid(bm, 14))
		return false;
	b.addr = bm;
	b.base = emu::valid(emu::r32(bm)) ? emu::ptr(emu::r32(bm)) : nullptr;
	uint16_t rb = emu::r16(bm + 4);
	b.rowBytes = rb & 0x3FFF;
	b.bounds = readRect(bm + 6);
	if (rb & 0x8000) {
		b.depth = emu::r16(bm + 32);
		b.ctab = emu::r32(bm + 42);
	} else {
		b.depth = 1;
		b.ctab = 0;
	}
	return b.base != nullptr;
}

static std::vector<uint32_t> systemPalette(int depth) {
	std::vector<uint32_t> p;
	if (depth == 8) {
		static const uint8_t lv[6] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00};
		for (int r = 0; r < 6; r++)
			for (int g = 0; g < 6; g++)
				for (int b = 0; b < 6; b++)
					if (r != 5 || g != 5 || b != 5)
						p.push_back(0xFF000000u | lv[r] << 16 | lv[g] << 8 | lv[b]);
		static const uint8_t ramp[10] = {0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
		for (int c = 0; c < 4; c++) {
			for (uint8_t v : ramp) {
				uint32_t col = c == 0 ? v << 16 : c == 1 ? v << 8 : c == 2 ? v : (v << 16 | v << 8 | v);
				p.push_back(0xFF000000u | col);
			}
		}
		p.push_back(0xFF000000u);
	} else {
		int n = 1 << depth;
		for (int i = 0; i < n; i++) {
			uint32_t v = 255 - i * 255 / (n - 1);
			p.push_back(0xFF000000u | v << 16 | v << 8 | v);
		}
	}
	return p;
}

const std::vector<uint32_t> &palette(gaddr ctab, int depth) {
	struct Cached {
		uint32_t seed;
		std::vector<uint32_t> colors;
	};
	static std::map<int, std::vector<uint32_t>> system;
	static std::unordered_map<gaddr, Cached> cache;
	if (ctab && heap::isHandle(ctab) && heap::deref(ctab)) {
		gaddr t = heap::deref(ctab);
		uint32_t seed = emu::r32(t);
		int size = (int16_t)emu::r16(t + 6) + 1;
		auto it = cache.find(ctab);
		if (it != cache.end() && it->second.seed == seed && (int)it->second.colors.size() >= size)
			return it->second.colors;
		Cached &c = cache[ctab];
		c.seed = seed;
		c.colors.assign(std::max(size, 1 << std::min(depth, 8)), 0xFF000000u);
		bool indexed = (emu::r16(t + 4) & 0x8000) == 0; // device tables ignore .value
		for (int i = 0; i < size; i++) {
			gaddr e = t + 8 + 8 * i;
			int idx = indexed ? emu::r16(e) & 0xFF : i;
			RGB rgb = readRGB(e + 2);
			if (idx < (int)c.colors.size())
				c.colors[idx] = argb(rgb);
		}
		return c.colors;
	}
	auto &p = system[depth];
	if (p.empty())
		p = systemPalette(depth);
	return p;
}

static std::unordered_map<uint64_t, uint8_t> nearestCache;

void colorsChanged() { nearestCache.clear(); }

uint8_t nearestIndex(gaddr ctab, int depth, uint32_t c) {
	auto &cache = nearestCache;
	uint64_t key = (uint64_t)ctab << 32 | (c & 0xFFFFFF);
	auto it = cache.find(key);
	if (it != cache.end())
		return it->second;
	const auto &pal = palette(ctab, depth);
	int best = 0, bestD = 1 << 30;
	int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
	for (size_t i = 0; i < pal.size(); i++) {
		int dr = (int)(pal[i] >> 16 & 0xFF) - r, dg = (int)(pal[i] >> 8 & 0xFF) - g, db = (int)(pal[i] & 0xFF) - b;
		int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
		if (d < bestD) {
			bestD = d;
			best = (int)i;
		}
	}
	if (cache.size() > 100000)
		cache.clear();
	cache[key] = (uint8_t)best;
	return (uint8_t)best;
}

static inline uint32_t load32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void store32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

uint32_t getPixel(const Bits &b, int x, int y) {
	const uint8_t *row = b.base + (size_t)y * b.rowBytes;
	switch (b.depth) {
	case 32:
		return load32(row + x * 4) | 0xFF000000u;
	case 16: {
		uint16_t v = (uint16_t)(row[x * 2] << 8 | row[x * 2 + 1]);
		uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, bl = v & 31;
		return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (bl << 3 | bl >> 2);
	}
	case 8:
		return palette(b.ctab, 8)[row[x]];
	case 4:
		return palette(b.ctab, 4)[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
	case 2:
		return palette(b.ctab, 2)[(row[x >> 2] >> (6 - 2 * (x & 3))) & 3];
	default:
		return (row[x >> 3] >> (7 - (x & 7)) & 1) ? 0xFF000000u : 0xFFFFFFFFu;
	}
}

void putPixel(const Bits &b, int x, int y, uint32_t c) {
	uint8_t *row = b.base + (size_t)y * b.rowBytes;
	switch (b.depth) {
	case 32:
		store32(row + x * 4, c | 0xFF000000u);
		break;
	case 16: {
		uint16_t v = (uint16_t)(((c >> 19) & 31) << 10 | ((c >> 11) & 31) << 5 | ((c >> 3) & 31));
		row[x * 2] = (uint8_t)(v >> 8);
		row[x * 2 + 1] = (uint8_t)v;
		break;
	}
	case 8:
		row[x] = nearestIndex(b.ctab, 8, c);
		break;
	case 1: {
		bool black = ((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF)) < 384;
		uint8_t m = (uint8_t)(0x80 >> (x & 7));
		row[x >> 3] = black ? (row[x >> 3] | m) : (row[x >> 3] & ~m);
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Ports

Port *port(gaddr a) {
	auto it = ports.find(a);
	return it == ports.end() ? nullptr : it->second;
}

Port *current() {
	return thePort;
}

void setCurrent(gaddr a) {
	if (Port *p = port(a))
		thePort = p;
	else if (a)
		logf("SetPort: unknown port 0x%08x", a);
}

static void writePixMap(Port *p, int depth, gaddr ctab, int width, int height) {
	uint32_t rowBytes = depth >= 8 ? (uint32_t)(width * depth / 8) : (uint32_t)((width * depth + 15) / 16 * 2);
	rowBytes = (rowBytes + 15) & ~15u;
	if (p->pixels)
		heap::free(p->pixels);
	p->pixels = heap::alloc(rowBytes * (uint32_t)std::max(height, 1), true);
	if (depth == 32)
		memset(emu::ptr(p->pixels), 0xFF, rowBytes * (uint32_t)std::max(height, 1));
	gaddr pm = heap::deref(p->pixmap);
	if (Port *old = portsByPixMap[pm]; old && old != p)
		logf("pixmap shared by two ports");
	portsByPixMap[pm] = p;
	memset(emu::ptr(pm), 0, kPixMapSize);
	emu::w32(pm, p->pixels);
	emu::w16(pm + 4, (uint16_t)(rowBytes | 0x8000));
	writeRect(pm + 6, p->portRect);
	emu::w32(pm + 22, 72 << 16);
	emu::w32(pm + 26, 72 << 16);
	emu::w16(pm + 30, depth > 8 ? 16 : 0); // RGBDirect or chunky
	emu::w16(pm + 32, (uint16_t)depth);
	emu::w16(pm + 34, depth > 8 ? 3 : 1);
	emu::w16(pm + 36, depth == 32 ? 8 : depth == 16 ? 5 : (uint16_t)depth);
	emu::w32(pm + 38, depth == 32 ? 0x20 : depth); // k32ARGBPixelFormat, k8IndexedPixelFormat...
	emu::w32(pm + 42, ctab);
}

Port *newPort(int width, int height, int depth, gaddr ctab, int16_t left, int16_t top) {
	Port *p = new Port;
	p->addr = heap::alloc(kPortSize, true);
	emu::w32(p->addr + 4, kPortMagic);
	p->pixmap = heap::newHandle(kPixMapSize, true);
	p->portRect.left = left;
	p->portRect.top = top;
	p->portRect.right = (int16_t)(left + width);
	p->portRect.bottom = (int16_t)(top + height);
	if (depth <= 8 && !ctab) {
		// Indexed ports always carry a color table
		const auto &pal = palette(0, depth);
		ctab = heap::newHandle(8 + 8 * (uint32_t)pal.size(), true);
		gaddr t = heap::deref(ctab);
		emu::w32(t, 0x10000 + depth);
		emu::w16(t + 4, 0x8000);
		emu::w16(t + 6, (uint16_t)(pal.size() - 1));
		for (size_t i = 0; i < pal.size(); i++) {
			gaddr e = t + 8 + 8 * (uint32_t)i;
			emu::w16(e, (uint16_t)i);
			uint32_t c = pal[i];
			writeRGB(e + 2, {(uint16_t)((c >> 16 & 0xFF) * 0x101), (uint16_t)((c >> 8 & 0xFF) * 0x101), (uint16_t)((c & 0xFF) * 0x101)});
		}
	}
	if (!ctab) {
		// Direct pixmaps get an empty table too: games read its seed
		ctab = heap::newHandle(8, true);
		gaddr t = heap::deref(ctab);
		emu::w32(t, (uint32_t)depth);
		emu::w16(t + 6, 0xFFFF);
	}
	writePixMap(p, depth, ctab, width, height);
	p->clipRgn = newRgn();
	Rect huge;
	huge.top = huge.left = -32767;
	huge.bottom = huge.right = 32767;
	setRgn(p->clipRgn, {huge});
	p->visRgn = newRgn();
	setRgn(p->visRgn, {p->portRect});
	p->device = theDevice;
	ports[p->addr] = p;
	return p;
}

void resizePort(Port *p, int width, int height) {
	gaddr pm = heap::deref(p->pixmap);
	int depth = emu::r16(pm + 32);
	gaddr ctab = emu::r32(pm + 42);
	p->portRect.right = (int16_t)(p->portRect.left + width);
	p->portRect.bottom = (int16_t)(p->portRect.top + height);
	writePixMap(p, depth, ctab, width, height);
	setRgn(p->visRgn, {p->portRect});
}

void disposePort(Port *p) {
	if (!p)
		return;
	if (thePort == p)
		thePort = nullptr;
	ports.erase(p->addr);
	portsByPixMap.erase(heap::deref(p->pixmap));
	disposeRgn(p->clipRgn);
	disposeRgn(p->visRgn);
	if (p->ownsPixels)
		heap::free(p->pixels);
	heap::disposeHandle(p->pixmap);
	heap::free(p->addr);
	delete p;
}

Bits portBits(Port *p) {
	Bits b;
	readBits(heap::deref(p->pixmap), b);
	return b;
}

Rects clipRects(Port *p) {
	return rgnOp(rgnRects(p->clipRgn), rgnRects(p->visRgn), 1);
}

Port *portForBits(gaddr bitmap) {
	auto it = portsByPixMap.find(bitmap);
	return it == portsByPixMap.end() ? nullptr : it->second;
}

void markDirty(Port *p) {
	if (p)
		p->dirty = true;
}

gaddr mainDevice() {
	return theDevice;
}

Port *screenPort() {
	return theScreen;
}

void init() {
	// The main screen: a 32-bit GDevice the size of the emulated screen
	theDevice = heap::newHandle(62, true);
	gaddr gd = heap::deref(theDevice);
	emu::w16(gd + 4, 2); // directType
	Port *screen = newPort(host::screenW, host::screenH, 32, 0);
	screenPixMapH = screen->pixmap;
	theScreen = screen;
	emu::w32(gd + 22, screenPixMapH);
	writeRect(gd + 34, screen->portRect);
	emu::w16(gd + 20, 0x8000 | 0x4000 | 0x0800 | 0x0400); // allInit, gdDevType, mainScreen, screenDevice
	screen->device = theDevice;
	thePort = screen;
}

// ---------------------------------------------------------------------------
// Drawing

void fillRect(Port *p, const Rect &r, uint32_t c) {
	Bits b = portBits(p);
	Rects clip = clipRects(p);
	for (const Rect &cr : clip) {
		Rect d = sect(sect(r, cr), b.bounds);
		if (d.empty())
			continue;
		for (int y = d.top; y < d.bottom; y++) {
			int py = y - b.bounds.top;
			if (b.depth == 32) {
				uint8_t *row = b.base + (size_t)py * b.rowBytes;
				for (int x = d.left; x < d.right; x++)
					store32(row + (x - b.bounds.left) * 4, c | 0xFF000000u);
			} else if (b.depth == 8) {
				uint8_t idx = nearestIndex(b.ctab, 8, c);
				memset(b.base + (size_t)py * b.rowBytes + (d.left - b.bounds.left), idx, d.width());
			} else {
				for (int x = d.left; x < d.right; x++)
					putPixel(b, x - b.bounds.left, py, c);
			}
		}
	}
	markDirty(p);
}

void frameRect(Port *p, const Rect &r, uint32_t c, int pen) {
	if (r.empty())
		return;
	Rect e = r;
	e.bottom = (int16_t)(r.top + pen);
	fillRect(p, e, c);
	e = r;
	e.top = (int16_t)(r.bottom - pen);
	fillRect(p, e, c);
	e = r;
	e.right = (int16_t)(r.left + pen);
	fillRect(p, e, c);
	e = r;
	e.left = (int16_t)(r.right - pen);
	fillRect(p, e, c);
}

void copyBits(const Bits &src, const Bits &dst, const Rect &sr, const Rect &dr, int mode, const Rects *clip,
	uint32_t bgColor) {
	if (sr.empty() || dr.empty())
		return;
	mode &= ~64; // ditherCopy
	int sw = sr.width(), sh = sr.height(), dw = dr.width(), dh = dr.height();
	Rects area = clip ? *clip : Rects{dr};
	bool sameIndexed = src.depth == 8 && dst.depth == 8 &&
		(src.ctab == dst.ctab || palette(src.ctab, 8) == palette(dst.ctab, 8));
	std::vector<uint8_t> map8;
	if (src.depth == 8 && dst.depth == 8 && !sameIndexed) {
		const auto &pal = palette(src.ctab, 8);
		map8.resize(256);
		for (int i = 0; i < 256; i++)
			map8[i] = nearestIndex(dst.ctab, 8, i < (int)pal.size() ? pal[i] : 0xFF000000u);
	}
	const std::vector<uint32_t> *spal = src.depth <= 8 && src.depth > 1 ? &palette(src.ctab, src.depth) : nullptr;
	for (const Rect &cr : area) {
		Rect d = sect(sect(dr, cr), dst.bounds);
		if (d.empty())
			continue;
		for (int y = d.top; y < d.bottom; y++) {
			int sy = sr.top + (int)((int64_t)(y - dr.top) * sh / dh);
			if (sy < src.bounds.top || sy >= src.bounds.bottom)
				continue;
			int py = sy - src.bounds.top;
			int dy = y - dst.bounds.top;
			const uint8_t *srow = src.base + (size_t)py * src.rowBytes;
			uint8_t *drow = dst.base + (size_t)dy * dst.rowBytes;
			for (int x = d.left; x < d.right; x++) {
				int sx = sr.left + (int)((int64_t)(x - dr.left) * sw / dw);
				if (sx < src.bounds.left || sx >= src.bounds.right)
					continue;
				int px = sx - src.bounds.left;
				int dx = x - dst.bounds.left;
				if (mode == 0 && sameIndexed) {
					drow[dx] = srow[px];
					continue;
				}
				if (mode == 0 && !map8.empty()) {
					drow[dx] = map8[srow[px]];
					continue;
				}
				uint32_t c;
				if (src.depth == 32)
					c = load32(srow + px * 4) | 0xFF000000u;
				else if (src.depth == 8)
					c = (*spal)[srow[px]];
				else
					c = getPixel(src, px, py);
				switch (mode) {
				case 0: // srcCopy
					break;
				case 1: // srcOr: black source pixels only
				case 8:
					if (src.depth == 1 && c != 0xFF000000u)
						continue;
					break;
				case 3: // srcBic: clear where source is black
					if (c != 0xFF000000u)
						continue;
					c = bgColor;
					break;
				case 2: // srcXor
				case 10: {
					uint32_t o = dst.depth == 32 ? load32(drow + dx * 4) : getPixel(dst, dx, dy);
					if (src.depth == 1 && c != 0xFF000000u)
						continue;
					c = (o ^ (c == 0xFF000000u ? 0xFFFFFF : c)) | 0xFF000000u;
					break;
				}
				case 4: // notSrcCopy
					c = (~c & 0xFFFFFF) | 0xFF000000u;
					break;
				case 36: // transparent
					if ((c & 0xFFFFFF) == (bgColor & 0xFFFFFF))
						continue;
					break;
				case 32: { // blend 50%
					uint32_t o = dst.depth == 32 ? load32(drow + dx * 4) : getPixel(dst, dx, dy);
					c = 0xFF000000u | (((o >> 16 & 0xFF) + (c >> 16 & 0xFF)) / 2) << 16 |
						(((o >> 8 & 0xFF) + (c >> 8 & 0xFF)) / 2) << 8 | (((o & 0xFF) + (c & 0xFF)) / 2);
					break;
				}
				default:
					break;
				}
				if (dst.depth == 32)
					store32(drow + dx * 4, c);
				else
					putPixel(dst, dx, dy, c);
			}
		}
	}
}

void drawImage(Port *p, const uint32_t *img, int w, int h, const Rect &dr) {
	if (dr.empty() || w <= 0 || h <= 0)
		return;
	Bits b = portBits(p);
	Rects clip = clipRects(p);
	int dw = dr.width(), dh = dr.height();
	for (const Rect &cr : clip) {
		Rect d = sect(sect(dr, cr), b.bounds);
		for (int y = d.top; y < d.bottom; y++) {
			int sy = (int)((int64_t)(y - dr.top) * h / dh);
			for (int x = d.left; x < d.right; x++) {
				int sx = (int)((int64_t)(x - dr.left) * w / dw);
				uint32_t c = img[(size_t)sy * w + sx];
				uint32_t a = c >> 24;
				if (a == 0)
					continue;
				int dx = x - b.bounds.left, dy = y - b.bounds.top;
				if (a < 255) {
					uint32_t o = getPixel(b, dx, dy);
					auto mix = [&](int s) {
						return (((c >> s & 0xFF) * a + (o >> s & 0xFF) * (255 - a)) / 255) << s;
					};
					c = 0xFF000000u | mix(16) | mix(8) | mix(0);
				}
				putPixel(b, dx, dy, c);
			}
		}
	}
	markDirty(p);
}

} // namespace qd

using namespace qd;

// ---------------------------------------------------------------------------
// Rectangles and points

static inline int16_t ptV(uint32_t pt) { return (int16_t)(pt & 0xFFFF); }
static inline int16_t ptH(uint32_t pt) { return (int16_t)(pt >> 16); }

IMPORT(SetRect) {
	Rect r;
	r.left = c.sarg16(1);
	r.top = c.sarg16(2);
	r.right = c.sarg16(3);
	r.bottom = c.sarg16(4);
	writeRect(c.arg(0), r);
}
IMPORT(OffsetRect) { writeRect(c.arg(0), offset(readRect(c.arg(0)), c.sarg16(1), c.sarg16(2))); }
IMPORT(InsetRect) {
	Rect r = readRect(c.arg(0));
	r.left += c.sarg16(1);
	r.right -= c.sarg16(1);
	r.top += c.sarg16(2);
	r.bottom -= c.sarg16(2);
	if (r.empty())
		r = Rect();
	writeRect(c.arg(0), r);
}
IMPORT(SectRect) {
	Rect r = sect(readRect(c.arg(0)), readRect(c.arg(1)));
	writeRect(c.arg(2), r);
	c.ret(!r.empty());
}
IMPORT(UnionRect) {
	Rect a = readRect(c.arg(0)), b = readRect(c.arg(1));
	Rect r = a.empty() ? b : b.empty() ? a : bounds({a, b});
	writeRect(c.arg(2), r);
}
IMPORT(EqualRect) {
	Rect a = readRect(c.arg(0)), b = readRect(c.arg(1));
	c.ret(a.top == b.top && a.left == b.left && a.bottom == b.bottom && a.right == b.right);
}
IMPORT(EmptyRect) { c.ret(readRect(c.arg(0)).empty()); }
IMPORT(PtInRect) {
	uint32_t pt = c.arg(0);
	c.ret(readRect(c.arg(1)).contains(ptV(pt), ptH(pt)));
}

// ---------------------------------------------------------------------------
// Regions

IMPORT(NewRgn) { c.ret(newRgn()); }
IMPORT(DisposeRgn) { disposeRgn(c.arg(0)); }
IMPORT(RectRgn) { setRgn(c.arg(0), {readRect(c.arg(1))}); }
IMPORT(SetRectRgn) {
	Rect r;
	r.left = c.sarg16(1);
	r.top = c.sarg16(2);
	r.right = c.sarg16(3);
	r.bottom = c.sarg16(4);
	setRgn(c.arg(0), {r});
}
IMPORT(OffsetRgn) {
	Rects rs = rgnRects(c.arg(0));
	for (Rect &r : rs)
		r = offset(r, c.sarg16(1), c.sarg16(2));
	setRgn(c.arg(0), rs);
}
IMPORT(SectRgn) { setRgn(c.arg(2), rgnOp(rgnRects(c.arg(0)), rgnRects(c.arg(1)), 1)); }
IMPORT(DiffRgn) { setRgn(c.arg(2), rgnOp(rgnRects(c.arg(0)), rgnRects(c.arg(1)), 2)); }
IMPORT(EmptyRgn) { c.ret(rgnRects(c.arg(0)).empty()); }
IMPORT(RectInRgn) {
	Rect r = readRect(c.arg(0));
	bool hit = false;
	for (const Rect &x : rgnRects(c.arg(1)))
		hit = hit || !sect(r, x).empty();
	c.ret(hit);
}
IMPORT(GetRegionBounds) {
	writeRect(c.arg(1), bounds(rgnRects(c.arg(0))));
	c.ret(c.arg(1));
}

// ---------------------------------------------------------------------------
// Ports and GWorlds

static Port *argPort(Ctx &c, int n, const char *who) {
	Port *p = port(c.arg(n));
	if (!p)
		emu::fatal("%s: unknown port 0x%08x, called from 0x%08x", who, c.arg(n), emu::r32(c.esp));
	return p;
}

IMPORT(GetPort) { emu::w32(c.arg(0), thePort ? thePort->addr : 0); }
IMPORT(SetPort) { setCurrent(c.arg(0)); }
IMPORT(GetQDGlobalsThePort) { c.ret(thePort ? thePort->addr : 0); }
IMPORT(GetGWorld) {
	if (c.arg(0))
		emu::w32(c.arg(0), thePort ? thePort->addr : 0);
	if (c.arg(1))
		emu::w32(c.arg(1), theDevice);
}
IMPORT(SetGWorld) { setCurrent(c.arg(0)); }
IMPORT(NewGWorld) {
	// (GWorldPtr *out, depth, const Rect *bounds, CTabHandle, GDHandle, flags)
	int depth = c.sarg16(1);
	if (depth == 0)
		depth = 32;
	Rect r = readRect(c.arg(2));
	if (depth != 32 && depth != 16 && depth != 8 && depth != 1)
		logf("NewGWorld: depth %d", depth);
	Port *p = newPort(r.width(), r.height(), depth, c.arg(3), r.left, r.top);
	p->offscreen = true;
	logf("NewGWorld %dx%d depth %d ctab 0x%x -> 0x%08x", r.width(), r.height(), depth, c.arg(3), p->addr);
	emu::w32(c.arg(0), p->addr);
	c.ret(0);
}
IMPORT(DisposeGWorld) { disposePort(port(c.arg(0))); }
IMPORT(GetGWorldPixMap) { c.ret(argPort(c, 0, "GetGWorldPixMap")->pixmap); }
IMPORT(GetPortPixMap) { c.ret(argPort(c, 0, "GetPortPixMap")->pixmap); }
IMPORT(LockPixels) { c.ret(1); }
IMPORT(UnlockPixels) {}
IMPORT(GetPixBaseAddr) { c.ret(emu::r32(heap::deref(c.arg(0)))); }
IMPORT(GetPixRowBytes) { c.ret(emu::r16(heap::deref(c.arg(0)) + 4) & 0x3FFF); }
IMPORT(GetPixDepth) { c.ret(emu::r16(heap::deref(c.arg(0)) + 32)); }
IMPORT(GetPortBitMapForCopyBits) { c.ret(heap::deref(argPort(c, 0, "GetPortBitMapForCopyBits")->pixmap)); }
IMPORT(GetPortBounds) {
	writeRect(c.arg(1), argPort(c, 0, "GetPortBounds")->portRect);
	c.ret(c.arg(1));
}
IMPORT(SetOrigin) {
	Port *p = thePort;
	int dh = c.sarg16(0) - p->portRect.left, dv = c.sarg16(1) - p->portRect.top;
	if (!dh && !dv)
		return;
	p->portRect = offset(p->portRect, dh, dv);
	gaddr pm = heap::deref(p->pixmap);
	writeRect(pm + 6, offset(readRect(pm + 6), dh, dv));
	Rects vis = rgnRects(p->visRgn);
	for (Rect &r : vis)
		r = offset(r, dh, dv);
	setRgn(p->visRgn, vis);
}
IMPORT(GetClip) { setRgn(c.arg(0), rgnRects(thePort->clipRgn)); }
IMPORT(SetClip) { setRgn(thePort->clipRgn, rgnRects(c.arg(0))); }
IMPORT(GetMainDevice) { c.ret(theDevice); }
IMPORT(DMGetFirstScreenDevice) { c.ret(theDevice); }
IMPORT(QDFlushPortBuffer) {
	markDirty(port(c.arg(0)));
	wm::presentScreen();
}

// ---------------------------------------------------------------------------
// Colors and pens

IMPORT(RGBForeColor) { thePort->fg = readRGB(c.arg(0)); }
IMPORT(RGBBackColor) { thePort->bg = readRGB(c.arg(0)); }
IMPORT(GetForeColor) { writeRGB(c.arg(0), thePort->fg); }
IMPORT(GetBackColor) { writeRGB(c.arg(0), thePort->bg); }
// The eight colors of the original QuickDraw (blackColor 33, whiteColor 30...)
static RGB oldColor(uint32_t old) {
	switch (old) {
	case 30: return {0xFFFF, 0xFFFF, 0xFFFF}; // white
	case 205: return {0xDD6B, 0x08C2, 0x06A2}; // red
	case 341: return {0x0000, 0x8000, 0x11B0}; // green
	case 409: return {0x0000, 0x0000, 0xD400}; // blue
	case 273: return {0x0241, 0xAB54, 0xEAFF}; // cyan
	case 137: return {0xF2D7, 0x0856, 0x84EC}; // magenta
	case 69: return {0xFC00, 0xF37D, 0x052F}; // yellow
	default: return {0, 0, 0};
	}
}
IMPORT(ForeColor) { thePort->fg = oldColor(c.arg(0)); }
IMPORT(BackColor) { thePort->bg = oldColor(c.arg(0)); }
IMPORT(Index2Color) {
	uint32_t col = palette(0, 8)[c.arg(0) & 0xFF];
	writeRGB(c.arg(1), {(uint16_t)((col >> 16 & 0xFF) * 0x101), (uint16_t)((col >> 8 & 0xFF) * 0x101), (uint16_t)((col & 0xFF) * 0x101)});
}
IMPORT(MoveTo) {
	thePort->penH = c.sarg16(0);
	thePort->penV = c.sarg16(1);
}
IMPORT(PaintRect) { fillRect(thePort, readRect(c.arg(0)), argb(thePort->fg)); }
IMPORT(EraseRect) { fillRect(thePort, readRect(c.arg(0)), argb(thePort->bg)); }
static Rects *recording = nullptr; // OpenRgn ... CloseRgn
IMPORT(FrameRect) {
	if (recording) {
		*recording = rgnOp(*recording, {readRect(c.arg(0))}, 3);
		return;
	}
	frameRect(thePort, readRect(c.arg(0)), argb(thePort->fg), thePort->pnSizeH);
}
// Square corners are close enough for the Assessment's buttons
IMPORT(FrameRoundRect) {
	if (recording) {
		*recording = rgnOp(*recording, {readRect(c.arg(0))}, 3);
		return;
	}
	frameRect(thePort, readRect(c.arg(0)), argb(thePort->fg), thePort->pnSizeH);
}
IMPORT(GetGrayRgn) {
	static gaddr gray = 0;
	if (!gray)
		gray = newRgn();
	Rect r;
	r.left = r.top = 0;
	r.right = (int16_t)host::screenW;
	r.bottom = (int16_t)host::screenH;
	setRgn(gray, {r});
	c.ret(gray);
}
IMPORT(StdRect) {
	Rect r = readRect(c.arg(1));
	switch ((int8_t)c.arg(0)) {
	case 0: frameRect(thePort, r, argb(thePort->fg)); break;
	case 1: case 4: fillRect(thePort, r, argb(thePort->fg)); break;
	case 2: fillRect(thePort, r, argb(thePort->bg)); break;
	default: logf("StdRect verb %d", (int8_t)c.arg(0)); break;
	}
}

IMPORT(CopyBits) {
	// (srcBits, dstBits, srcRect, dstRect, mode, maskRgn)
	Bits src, dst;
	Port *sp = portForBits(c.arg(0)), *dp = portForBits(c.arg(1));
	if (sp && sp == screenPort())
		wm::composeScreen(portBits(sp)); // reading the screen: bring it up to date
	if (!readBits(c.arg(0), src) || !readBits(c.arg(1), dst)) {
		// Empty sprites come as PixMaps without pixels; that draws nothing
		if (emu::valid(c.arg(0), 8) && emu::r32(c.arg(0)) == 0)
			return;
		static int warned = 0;
		if (warned++ < 3) {
			for (gaddr bm : {c.arg(0), c.arg(1)}) {
				if (emu::valid(bm, 16))
					logf("CopyBits: bitmap 0x%08x base 0x%08x rowBytes 0x%04x from 0x%08x", bm, emu::r32(bm), emu::r16(bm + 4),
						emu::r32(c.esp));
				else
					logf("CopyBits: bad bitmap pointer 0x%08x", bm);
			}
		}
		return;
	}
	Rect sr = readRect(c.arg(2)), dr = readRect(c.arg(3));
	int16_t mode = c.sarg16(4);
	uint32_t bg = thePort ? argb(thePort->bg) : 0xFFFFFFFFu;
	if (dp && dp == screenPort()) {
		// Drawing on the screen lands in the windows there
		for (wm::Window *w : wm::windows()) {
			if (!w->visible)
				continue;
			Bits wb = portBits(w->port);
			int dh = w->port->portRect.left - w->bounds.left, dv = w->port->portRect.top - w->bounds.top;
			Rects clip = rgnOp(clipRects(w->port), {offset(dr, dh, dv)}, 1);
			if (c.arg(5)) {
				Rects mask = rgnRects(c.arg(5));
				for (Rect &r : mask)
					r = offset(r, dh, dv);
				clip = rgnOp(clip, mask, 1);
			}
			copyBits(src, wb, sr, offset(dr, dh, dv), mode, &clip, bg);
			markDirty(w->port);
		}
		return;
	}
	Rects clip;
	if (dp)
		clip = clipRects(dp);
	else
		clip = {dst.bounds};
	if (c.arg(5))
		clip = rgnOp(clip, rgnRects(c.arg(5)), 1);
	copyBits(src, dst, sr, dr, mode, &clip, bg);
	markDirty(dp);
}

// ---------------------------------------------------------------------------
// Coordinates and the mouse

IMPORT(GlobalToLocal) {
	gaddr pt = c.arg(0);
	Port *p = thePort;
	if (p && p->window) {
		emu::w16(pt, (uint16_t)(emu::rs16(pt) - p->window->bounds.top + p->portRect.top));
		emu::w16(pt + 2, (uint16_t)(emu::rs16(pt + 2) - p->window->bounds.left + p->portRect.left));
	}
}
IMPORT(GetGlobalMouse) {
	emu::w16(c.arg(0), (uint16_t)host::mouseY);
	emu::w16(c.arg(0) + 2, (uint16_t)host::mouseX);
}
IMPORT(GetMouse) {
	int v = host::mouseY, h = host::mouseX;
	Port *p = thePort;
	if (p && p->window) {
		v = v - p->window->bounds.top + p->portRect.top;
		h = h - p->window->bounds.left + p->portRect.left;
	}
	emu::w16(c.arg(0), (uint16_t)v);
	emu::w16(c.arg(0) + 2, (uint16_t)h);
}

// ---------------------------------------------------------------------------
// Classic drawing: pen size, lines, ovals, regions, masks

namespace {

// Rows of an oval, one rectangle per row
Rects ovalRows(const Rect &r) {
	Rects rows;
	double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
	double rx = r.width() / 2.0, ry = r.height() / 2.0;
	if (rx <= 0 || ry <= 0)
		return rows;
	for (int y = r.top; y < r.bottom; y++) {
		double dy = (y + 0.5 - cy) / ry;
		double half = rx * sqrt(std::max(0.0, 1 - dy * dy));
		Rect row;
		row.top = (int16_t)y;
		row.bottom = (int16_t)(y + 1);
		row.left = (int16_t)lround(cx - half);
		row.right = (int16_t)lround(cx + half);
		if (!row.empty())
			rows.push_back(row);
	}
	return rows;
}

void paintRects(Port *p, const Rects &rs, uint32_t c) {
	for (const Rect &r : rs)
		fillRect(p, r, c);
}

// The outline of a region, pen-sized: pixels inside it with a neighbor outside
void frameRects(Port *p, const Rects &rs, uint32_t c) {
	Rect bb = bounds(rs);
	if (bb.empty())
		return;
	int w = bb.width(), h = bb.height();
	std::vector<uint8_t> in((size_t)w * h);
	for (const Rect &r : rs)
		for (int y = r.top; y < r.bottom; y++)
			for (int x = r.left; x < r.right; x++)
				in[(size_t)(y - bb.top) * w + (x - bb.left)] = 1;
	auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && in[(size_t)y * w + x]; };
	int ph = std::max<int>(p->pnSizeH, 1), pv = std::max<int>(p->pnSizeV, 1);
	Rects out;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (!inside(x, y))
				continue;
			bool edge = false;
			for (int d = 1; d <= ph && !edge; d++)
				edge = !inside(x - d, y) || !inside(x + d, y);
			for (int d = 1; d <= pv && !edge; d++)
				edge = !inside(x, y - d) || !inside(x, y + d);
			if (edge) {
				Rect px;
				px.left = (int16_t)(x + bb.left);
				px.top = (int16_t)(y + bb.top);
				px.right = (int16_t)(px.left + 1);
				px.bottom = (int16_t)(px.top + 1);
				out.push_back(px);
			}
		}
	}
	paintRects(p, out, c);
}

} // namespace

IMPORT(PenSize) {
	thePort->pnSizeH = c.sarg16(0);
	thePort->pnSizeV = c.sarg16(1);
}
IMPORT(LineTo) {
	// Bresenham with the pen as a rectangle hanging below and right
	int x0 = thePort->penH, y0 = thePort->penV, x1 = c.sarg16(0), y1 = c.sarg16(1);
	thePort->penH = (int16_t)x1;
	thePort->penV = (int16_t)y1;
	Rects pts;
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
	for (;;) {
		Rect r;
		r.left = (int16_t)x0;
		r.top = (int16_t)y0;
		r.right = (int16_t)(x0 + thePort->pnSizeH);
		r.bottom = (int16_t)(y0 + thePort->pnSizeV);
		pts.push_back(r);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
	if (recording)
		return;
	paintRects(thePort, pts, argb(thePort->fg));
}
IMPORT(FrameOval) {
	Rects rows = ovalRows(readRect(c.arg(0)));
	if (recording) {
		*recording = rgnOp(*recording, rows, 3);
		return;
	}
	frameRects(thePort, rows, argb(thePort->fg));
}
IMPORT(PaintOval) { paintRects(thePort, ovalRows(readRect(c.arg(0))), argb(thePort->fg)); }
IMPORT(PaintRgn) { paintRects(thePort, rgnRects(c.arg(0)), argb(thePort->fg)); }
IMPORT(EraseRgn) { paintRects(thePort, rgnRects(c.arg(0)), argb(thePort->bg)); }
IMPORT(FrameRgn) { frameRects(thePort, rgnRects(c.arg(0)), argb(thePort->fg)); }

IMPORT(OpenRgn) {
	delete recording;
	recording = new Rects;
}
IMPORT(CloseRgn) {
	setRgn(c.arg(0), recording ? *recording : Rects());
	delete recording;
	recording = nullptr;
}
IMPORT(SetEmptyRgn) { setRgn(c.arg(0), {}); }
IMPORT(CopyRgn) { setRgn(c.arg(1), rgnRects(c.arg(0))); }
IMPORT(UnionRgn) { setRgn(c.arg(2), rgnOp(rgnRects(c.arg(0)), rgnRects(c.arg(1)), 0)); }
IMPORT(XorRgn) { setRgn(c.arg(2), rgnOp(rgnRects(c.arg(0)), rgnRects(c.arg(1)), 3)); }
IMPORT(EqualRgn) { c.ret(rgnOp(rgnRects(c.arg(0)), rgnRects(c.arg(1)), 3).empty()); }
IMPORT(PtInRgn) {
	// (Point by value, rgn)
	int16_t v = (int16_t)(c.arg(0) & 0xFFFF), h = (int16_t)(c.arg(0) >> 16);
	bool hit = false;
	for (const Rect &r : rgnRects(c.arg(1)))
		hit = hit || r.contains(v, h);
	c.ret(hit);
}
IMPORT(ClipRect) { setRgn(thePort->clipRgn, {readRect(c.arg(0))}); }

IMPORT(LocalToGlobal) {
	gaddr pt = c.arg(0);
	Port *p = thePort;
	if (p && p->window) {
		emu::w16(pt, (uint16_t)(emu::rs16(pt) + p->window->bounds.top - p->portRect.top));
		emu::w16(pt + 2, (uint16_t)(emu::rs16(pt + 2) + p->window->bounds.left - p->portRect.left));
	}
}

IMPORT(CopyMask) {
	// (srcBits, maskBits, dstBits, srcRect, maskRect, dstRect): copies the
	// source where the mask is not white
	Bits src, mask, dst;
	if (!readBits(c.arg(0), src) || !readBits(c.arg(1), mask) || !readBits(c.arg(2), dst) || !src.base || !dst.base)
		return;
	Rect sr = readRect(c.arg(3)), mr = readRect(c.arg(4)), dr = readRect(c.arg(5));
	if (sr.empty() || dr.empty() || mr.empty())
		return;
	Port *dp = portForBits(c.arg(2));
	Rects clip = dp ? clipRects(dp) : Rects{dst.bounds};
	for (int y = dr.top; y < dr.bottom; y++) {
		int sy = sr.top + (y - dr.top) * sr.height() / dr.height();
		int my = mr.top + (y - dr.top) * mr.height() / dr.height();
		for (int x = dr.left; x < dr.right; x++) {
			if (!dst.bounds.contains(y, x))
				continue;
			bool inClip = false;
			for (const Rect &cr : clip)
				inClip = inClip || cr.contains(y, x);
			if (!inClip)
				continue;
			int sx = sr.left + (x - dr.left) * sr.width() / dr.width();
			int mx = mr.left + (x - dr.left) * mr.width() / dr.width();
			if (!mask.bounds.contains(my, mx) || !src.bounds.contains(sy, sx))
				continue;
			if ((getPixel(mask, mx - mask.bounds.left, my - mask.bounds.top) & 0xFFFFFF) == 0xFFFFFF)
				continue;
			putPixel(dst, x - dst.bounds.left, y - dst.bounds.top, getPixel(src, sx - src.bounds.left, sy - src.bounds.top));
		}
	}
	markDirty(dp);
}

IMPORT(CreateNewPort) {
	Port *p = newPort(1, 1, 32, 0);
	c.ret(p->addr);
}
IMPORT(DisposePort) {
	if (Port *p = port(c.arg(0)))
		disposePort(p);
}

IMPORT(NewGWorldFromPtr) {
	// (GWorldPtr *out, pixelFormat, const Rect *bounds, CTabHandle, GDHandle, flags, Ptr buffer, rowBytes)
	uint32_t fmt = c.arg(1);
	int depth = fmt == 0x20 || fmt == 0x41524742 || fmt == 0x42475241 ? 32 : fmt == 0x10 || fmt == 0x42353535 ? 16 : (int)fmt;
	if (fmt == 0x42475241)
		logf("NewGWorldFromPtr: BGRA pixels are drawn as ARGB");
	Rect r = readRect(c.arg(2));
	Port *p = newPort(r.width(), r.height(), depth, c.arg(3), r.left, r.top);
	// Draw straight into the caller's buffer
	heap::free(p->pixels);
	p->pixels = c.arg(6);
	p->ownsPixels = false;
	gaddr pm = heap::deref(p->pixmap);
	emu::w32(pm, c.arg(6));
	emu::w16(pm + 4, (uint16_t)(c.arg(7) | 0x8000));
	p->offscreen = true;
	emu::w32(c.arg(0), p->addr);
	c.ret(0);
}
