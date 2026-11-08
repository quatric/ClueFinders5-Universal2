// QuickDraw: ports, offscreen GWorlds, regions, CopyBits and pictures.
// Carbon structs on Intel are native little-endian; 32-bit pixels are stored
// A,R,G,B (k32ARGBPixelFormat) and resource data stays big-endian.
#pragma once

#include "emu.h"

#include <cstdint>
#include <vector>

namespace wm {
struct Window;
}

namespace qd {

struct Rect {
	int16_t top = 0, left = 0, bottom = 0, right = 0;
	int width() const { return right - left; }
	int height() const { return bottom - top; }
	bool empty() const { return bottom <= top || right <= left; }
	bool contains(int v, int h) const { return v >= top && v < bottom && h >= left && h < right; }
};
Rect readRect(gaddr a);
void writeRect(gaddr a, const Rect &r);
Rect sect(const Rect &a, const Rect &b);
Rect offset(const Rect &r, int dh, int dv);

struct RGB {
	uint16_t r = 0, g = 0, b = 0;
};
inline RGB readRGB(gaddr a) { return {emu::r16(a), emu::r16(a + 2), emu::r16(a + 4)}; }
inline void writeRGB(gaddr a, RGB c) { emu::w16(a, c.r); emu::w16(a + 2, c.g); emu::w16(a + 4, c.b); }
// 0xAARRGGBB
inline uint32_t argb(RGB c) { return 0xFF000000u | (uint32_t)(c.r >> 8) << 16 | (uint32_t)(c.g >> 8) << 8 | (c.b >> 8); }

// Regions are rectangle lists; the guest handle holds rgnSize and rgnBBox
typedef std::vector<Rect> Rects;
gaddr newRgn();
void disposeRgn(gaddr rgn);
const Rects &rgnRects(gaddr rgn);
void setRgn(gaddr rgn, const Rects &rects);
Rects rgnOp(const Rects &a, const Rects &b, int op); // 0 union, 1 sect, 2 diff, 3 xor
Rect bounds(const Rects &r);

// A PixMap or BitMap as found in memory
struct Bits {
	uint8_t *base = nullptr;
	uint32_t rowBytes = 0;
	Rect bounds;
	int depth = 1;
	gaddr ctab = 0; // CTabHandle for indexed depths
	gaddr addr = 0; // the BitMap/PixMap itself
};
bool readBits(gaddr bitmap, Bits &b);
uint32_t getPixel(const Bits &b, int x, int y); // 0xAARRGGBB
void putPixel(const Bits &b, int x, int y, uint32_t argb);

// A palette (0xAARRGGBB) for an indexed CTabHandle, or the system palette
const std::vector<uint32_t> &palette(gaddr ctab, int depth);
uint8_t nearestIndex(gaddr ctab, int depth, uint32_t argb);
// Call when a color table's entries change (CTabChanged)
void colorsChanged();

struct Port {
	gaddr addr = 0;      // the CGrafPtr handed to the guest
	gaddr pixmap = 0;    // PixMapHandle
	gaddr pixels = 0;
	Rect portRect;
	gaddr clipRgn = 0;
	gaddr visRgn = 0;
	RGB fg, bg = {0xFFFF, 0xFFFF, 0xFFFF};
	int16_t penH = 0, penV = 0;
	int16_t pnSizeH = 1, pnSizeV = 1;
	int16_t txFont = 0, txSize = 0, txMode = 1;
	uint8_t txFace = 0;
	bool offscreen = false;
	bool ownsPixels = true; // false for NewGWorldFromPtr
	gaddr device = 0;
	wm::Window *window = nullptr;
	bool dirty = false;
};

Port *port(gaddr grafPtr); // null if not a port we made
Port *current();
void setCurrent(gaddr grafPtr);
Port *newPort(int width, int height, int depth, gaddr ctab, int16_t left = 0, int16_t top = 0);
void disposePort(Port *p);
// Rebuilds a port's pixel buffer for a new size, keeping its identity
void resizePort(Port *p, int width, int height);
Bits portBits(Port *p);
// The port's clip and visible regions intersected, in local coordinates
Rects clipRects(Port *p);
// The port whose PixMap this is, if any
Port *portForBits(gaddr bitmap);

gaddr mainDevice();
// The port of the whole screen: GetWindowPort(NULL)
Port *screenPort();
void init();

// Drawing in local coordinates of a port
void fillRect(Port *p, const Rect &r, uint32_t argb);
void frameRect(Port *p, const Rect &r, uint32_t argb, int pen = 1);
void copyBits(const Bits &src, const Bits &dst, const Rect &srcRect, const Rect &dstRect, int mode,
	const Rects *clip, uint32_t bgColor);
// Draws a 0xAARRGGBB image into a port with alpha blending, scaled to dst
void drawImage(Port *p, const uint32_t *img, int w, int h, const Rect &dst);
void drawPicture(Port *p, gaddr picHandle, const Rect &dst);
bool decodeAnimation(const uint8_t *data, size_t size, int width, int height, int depth, std::vector<uint32_t> &out);
void markDirty(Port *p);

} // namespace qd
