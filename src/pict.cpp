// DrawPicture: PICT version 2 bitmaps (PackBits/DirectBits opcodes) and the
// QuickTime opcodes around them. The game's pictures wrap their bitmaps in
// an UncompressedQuickTime opcode whose matte, coded with the Animation
// codec, supplies the alpha channel.
#include "heap.h"
#include "qd.h"

#include <ImageIO/ImageIO.h>
#include <algorithm>

namespace qd {

namespace {

struct Reader {
	const uint8_t *p, *end;
	bool ok() const { return p <= end; }
	uint8_t u8() { return p < end ? *p++ : (p++, 0); }
	uint16_t u16() { uint16_t v = (uint16_t)(u8() << 8); return (uint16_t)(v | u8()); }
	uint32_t u32() { uint32_t v = (uint32_t)u16() << 16; return v | u16(); }
	int16_t s16() { return (int16_t)u16(); }
	Rect rect() {
		Rect r;
		r.top = s16();
		r.left = s16();
		r.bottom = s16();
		r.right = s16();
		return r;
	}
	void skip(size_t n) { p += n; }
};

// The picture being drawn: an ARGB canvas in picture-frame coordinates
struct Canvas {
	Rect frame;
	int w, h;
	std::vector<uint32_t> px;
	std::vector<uint8_t> matte; // alpha for the next bitmap, in frame coordinates
	Rect matteRect;
	bool hasMatte = false;
};

void unpackBits(Reader &r, std::vector<uint8_t> &out, size_t want, int packedBytes, int unit) {
	const uint8_t *stop = r.p + packedBytes;
	out.clear();
	while (r.p < stop && r.ok()) {
		int8_t n = (int8_t)r.u8();
		if (n >= 0) {
			size_t len = (size_t)(n + 1) * unit;
			for (size_t i = 0; i < len; i++)
				out.push_back(r.u8());
		} else if (n != -128) {
			int count = 1 - n;
			uint8_t v[2] = {r.u8(), unit == 2 ? r.u8() : (uint8_t)0};
			for (int i = 0; i < count; i++)
				for (int k = 0; k < unit; k++)
					out.push_back(v[k]);
		}
	}
	r.p = stop;
	out.resize(want);
}

void blitToCanvas(Canvas &cv, const std::vector<uint32_t> &img, int iw, int ih, const Rect &bounds, const Rect &src,
	const Rect &dst) {
	// src is in bounds coordinates; dst in frame coordinates
	int sw = src.width(), sh = src.height(), dw = dst.width(), dh = dst.height();
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	for (int y = 0; y < dh; y++) {
		int fy = dst.top + y - cv.frame.top;
		if (fy < 0 || fy >= cv.h)
			continue;
		int sy = src.top - bounds.top + y * sh / dh;
		if (sy < 0 || sy >= ih)
			continue;
		for (int x = 0; x < dw; x++) {
			int fx = dst.left + x - cv.frame.left;
			if (fx < 0 || fx >= cv.w)
				continue;
			int sx = src.left - bounds.left + x * sw / dw;
			if (sx < 0 || sx >= iw)
				continue;
			uint32_t c = img[(size_t)sy * iw + sx];
			if (cv.hasMatte) {
				int my = dst.top + y - cv.matteRect.top, mx = dst.left + x - cv.matteRect.left;
				uint8_t a = (my >= 0 && my < cv.matteRect.height() && mx >= 0 && mx < cv.matteRect.width())
					? cv.matte[(size_t)my * cv.matteRect.width() + mx] : 0;
				c = (c & 0xFFFFFF) | (uint32_t)a << 24;
			}
			cv.px[(size_t)fy * cv.w + fx] = c;
		}
	}
	cv.hasMatte = false;
}

void bitsOp(Reader &r, Canvas &cv, bool direct, bool rgn) {
	if (direct)
		r.u32(); // baseAddr
	uint16_t rowBytesRaw = r.u16();
	bool pixmap = (rowBytesRaw & 0x8000) != 0;
	int rowBytes = rowBytesRaw & 0x3FFF;
	Rect bounds = r.rect();
	int depth = 1, packType = 0, cmpCount = 1;
	std::vector<uint32_t> pal;
	if (pixmap) {
		r.u16(); // version
		packType = r.u16();
		r.u32(); // packSize
		r.u32(); // hRes
		r.u32(); // vRes
		r.u16(); // pixelType
		depth = r.u16();
		cmpCount = r.u16();
		r.u16(); // cmpSize
		r.u32(); // planeBytes
		r.u32(); // pmTable
		r.u32(); // reserved
		if (!direct) {
			r.u32(); // ctSeed
			uint16_t flags = r.u16();
			int n = r.u16() + 1;
			pal.assign(256, 0xFF000000u);
			for (int i = 0; i < n; i++) {
				int idx = r.u16();
				uint16_t cr = r.u16(), cg = r.u16(), cb = r.u16();
				int at = (flags & 0x8000) ? i : idx;
				if (at < 256)
					pal[at] = 0xFF000000u | (cr >> 8) << 16 | (cg >> 8) << 8 | (cb >> 8);
			}
		}
	} else {
		pal = {0xFFFFFFFFu, 0xFF000000u};
	}
	Rect src = r.rect(), dst = r.rect();
	r.u16(); // mode
	if (rgn) {
		uint16_t size = r.u16();
		r.skip(size - 2);
	}
	int w = bounds.width(), h = bounds.height();
	std::vector<uint32_t> img((size_t)std::max(w, 0) * std::max(h, 0));
	std::vector<uint8_t> row;
	for (int y = 0; y < h && r.ok(); y++) {
		size_t want = (size_t)rowBytes;
		int unit = 1;
		if (depth == 16 && packType != 1)
			unit = 2;
		if (depth == 32 && packType == 4)
			want = (size_t)w * cmpCount;
		else if (depth == 32 && packType == 2)
			want = (size_t)w * 3;
		if (rowBytes < 8 || packType == 1 || (depth == 32 && packType == 2)) {
			row.assign(r.p, r.p + want);
			r.skip(want);
		} else {
			int n = rowBytes > 250 ? r.u16() : r.u8();
			unpackBits(r, row, want, n, unit);
		}
		uint32_t *out = &img[(size_t)y * w];
		for (int x = 0; x < w; x++) {
			uint32_t c = 0xFF000000u;
			switch (depth) {
			case 1: c = pal[(row[x >> 3] >> (7 - (x & 7))) & 1]; break;
			case 2: c = pal[(row[x >> 2] >> (6 - 2 * (x & 3))) & 3]; break;
			case 4: c = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; break;
			case 8: c = pal[row[x]]; break;
			case 16: {
				uint16_t v = (uint16_t)(row[x * 2] << 8 | row[x * 2 + 1]);
				uint32_t r5 = (v >> 10) & 31, g5 = (v >> 5) & 31, b5 = v & 31;
				c = 0xFF000000u | (r5 << 3 | r5 >> 2) << 16 | (g5 << 3 | g5 >> 2) << 8 | (b5 << 3 | b5 >> 2);
				break;
			}
			case 32:
				if (packType == 4) {
					int off = cmpCount == 4 ? w : 0;
					c = 0xFF000000u | row[off + x] << 16 | row[off + w + x] << 8 | row[off + 2 * w + x];
				} else if (packType == 2) {
					c = 0xFF000000u | row[x * 3] << 16 | row[x * 3 + 1] << 8 | row[x * 3 + 2];
				} else {
					c = 0xFF000000u | row[x * 4 + 1] << 16 | row[x * 4 + 2] << 8 | row[x * 4 + 3];
				}
				break;
			}
			out[x] = c;
		}
	}
	blitToCanvas(cv, img, w, h, bounds, src, dst);
}

} // namespace

// QuickTime Animation ('rle ') frames, decoded to 8-bit values (gray or
// palette indices) or ARGB depending on depth. Frames only change the lines
// they code, so movies keep `out` from frame to frame.
bool decodeAnimation(const uint8_t *d, size_t size, int width, int height, int depth, std::vector<uint32_t> &out) {
	if (out.size() != (size_t)width * height)
		out.assign((size_t)width * height, 0);
	Reader r{d, d + size};
	uint32_t chunk = r.u32() & 0x3FFFFFFF;
	if (chunk < 8)
		return true;
	uint16_t header = r.u16();
	int startLine = 0, lines = height;
	if (header & 8) {
		startLine = r.u16();
		r.u16();
		lines = r.u16();
		r.u16();
	}
	int bpp = depth > 32 ? depth - 32 : depth;
	for (int y = startLine; y < startLine + lines && y < height && r.ok(); y++) {
		uint32_t *row = &out[(size_t)y * width];
		int x = 0;
		int unit = bpp == 8 ? 4 : bpp == 16 || bpp == 24 || bpp == 32 ? 1 : 0;
		if (!unit) {
			logf("PICT: Animation codec depth %d", depth);
			return false;
		}
		x += (r.u8() - 1) * unit;
		for (;;) {
			int8_t code = (int8_t)r.u8();
			if (!r.ok())
				return false;
			if (code == -1)
				break;
			if (code == 0) {
				x += (r.u8() - 1) * unit;
				continue;
			}
			auto readPixel = [&]() -> uint32_t {
				switch (bpp) {
				case 16: {
					uint16_t v = r.u16();
					uint32_t r5 = (v >> 10) & 31, g5 = (v >> 5) & 31, b5 = v & 31;
					return 0xFF000000u | (r5 << 3 | r5 >> 2) << 16 | (g5 << 3 | g5 >> 2) << 8 | (b5 << 3 | b5 >> 2);
				}
				case 24: {
					uint32_t c = (uint32_t)r.u8() << 16;
					c |= (uint32_t)r.u8() << 8;
					return 0xFF000000u | c | r.u8();
				}
				default:
					return r.u32();
				}
			};
			if (code > 0) {
				for (int i = 0; i < code; i++) {
					if (unit == 4) {
						for (int k = 0; k < 4; k++, x++) {
							uint8_t v = r.u8();
							if (x < width)
								row[x] = v;
						}
					} else {
						uint32_t c = readPixel();
						if (x < width)
							row[x] = c;
						x++;
					}
				}
			} else {
				int count = -code;
				if (unit == 4) {
					uint8_t v[4] = {r.u8(), r.u8(), r.u8(), r.u8()};
					for (int i = 0; i < count; i++)
						for (int k = 0; k < 4; k++, x++)
							if (x < width)
								row[x] = v[k];
				} else {
					uint32_t c = readPixel();
					for (int i = 0; i < count; i++, x++)
						if (x < width)
							row[x] = c;
				}
			}
		}
	}
	return true;
}

namespace {

// Reads an ImageDescription and its data; fills `out` with ARGB, or with
// alpha values when `alphaOnly`
bool decodeImage(Reader &r, int &w, int &h, std::vector<uint32_t> &out, bool alphaOnly) {
	const uint8_t *idsc = r.p;
	uint32_t size = r.u32();
	uint32_t codec = r.u32();
	Reader f{idsc + 32, idsc + size};
	w = (int16_t)f.u16();
	h = (int16_t)f.u16();
	Reader dz{idsc + 44, idsc + size};
	uint32_t dataSize = dz.u32();
	Reader dp{idsc + 82, idsc + size};
	int depth = dp.u16();
	const uint8_t *data = idsc + size;
	if (!dataSize)
		dataSize = (uint32_t)(r.end - data);
	r.p = data + dataSize;
	if (codec == 0x726C6520) { // 'rle '
		std::vector<uint32_t> raw;
		if (!decodeAnimation(data, dataSize, w, h, depth, raw))
			return false;
		out.resize(raw.size());
		for (size_t i = 0; i < raw.size(); i++) {
			if (depth == 40) // 8-bit gray, 0 = white
				out[i] = alphaOnly ? raw[i] : 0xFF000000u | (255 - raw[i]) * 0x010101u;
			else if (depth == 32)
				out[i] = alphaOnly ? raw[i] >> 24 : raw[i];
			else
				out[i] = alphaOnly ? (raw[i] ? 255 : 0) : raw[i];
		}
		return true;
	}
	if (codec == 0x6A706567) { // 'jpeg'
		CFDataRef cd = CFDataCreate(nullptr, data, dataSize);
		CGImageSourceRef src = CGImageSourceCreateWithData(cd, nullptr);
		CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
		bool ok = false;
		if (img) {
			out.assign((size_t)w * h, 0);
			CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
			CGContextRef ctx = CGBitmapContextCreate(out.data(), w, h, 8, w * 4, rgb,
				kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
			CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
			CGContextRelease(ctx);
			CGColorSpaceRelease(rgb);
			CGImageRelease(img);
			ok = true;
		}
		if (src)
			CFRelease(src);
		CFRelease(cd);
		return ok;
	}
	logf("PICT: QuickTime codec %c%c%c%c", codec >> 24, codec >> 16, codec >> 8, codec);
	return false;
}

void quickTimeOp(Reader &r, Canvas &cv, bool compressed) {
	uint32_t len = r.u32();
	const uint8_t *end = r.p + len;
	r.u16(); // version
	r.skip(36); // matrix
	uint32_t matteSize = r.u32();
	Rect matteRect = r.rect();
	if (compressed) {
		r.u16(); // mode
		r.rect(); // srcRect
		r.u32(); // accuracy
		uint32_t maskSize = r.u32();
		if (matteSize) {
			Reader m{r.p, r.p + matteSize};
			int mw, mh;
			std::vector<uint32_t> alpha;
			if (decodeImage(m, mw, mh, alpha, true)) {
				cv.matte.assign(alpha.begin(), alpha.end());
				cv.matteRect = matteRect;
				cv.matteRect.right = (int16_t)(matteRect.left + mw);
				cv.matteRect.bottom = (int16_t)(matteRect.top + mh);
				cv.hasMatte = true;
			}
			r.skip(matteSize);
		}
		r.skip(maskSize);
		Reader img{r.p, end};
		int w, h;
		std::vector<uint32_t> px;
		if (decodeImage(img, w, h, px, false)) {
			Rect b;
			b.right = (int16_t)w;
			b.bottom = (int16_t)h;
			blitToCanvas(cv, px, w, h, b, b, cv.frame);
		}
	} else if (matteSize) {
		// The bitmap follows as ordinary opcodes; the matte gives its alpha
		Reader m{r.p, r.p + matteSize};
		int mw, mh;
		std::vector<uint32_t> alpha;
		if (decodeImage(m, mw, mh, alpha, true)) {
			cv.matte.resize(alpha.size());
			for (size_t i = 0; i < alpha.size(); i++)
				cv.matte[i] = (uint8_t)alpha[i];
			cv.matteRect = matteRect;
			cv.matteRect.right = (int16_t)(matteRect.left + mw);
			cv.matteRect.bottom = (int16_t)(matteRect.top + mh);
			cv.hasMatte = true;
		}
	}
	r.p = end;
}

} // namespace

void drawPicture(Port *port, gaddr picH, const Rect &dst) {
	if (!picH || !heap::deref(picH))
		return;
	uint32_t size = heap::handleSize(picH);
	const uint8_t *data = emu::ptr(heap::deref(picH));
	Reader r{data, data + size};
	r.u16(); // picSize
	Canvas cv;
	cv.frame = r.rect();
	cv.w = cv.frame.width();
	cv.h = cv.frame.height();
	if (cv.w <= 0 || cv.h <= 0)
		return;
	cv.px.assign((size_t)cv.w * cv.h, 0);
	bool v2 = false;
	while (r.ok() && r.p + 1 < r.end) {
		if (v2 && ((r.p - data) & 1))
			r.u8();
		uint16_t op = v2 ? r.u16() : r.u8();
		if (op == 0x0011) {
			uint16_t ver = v2 ? r.u16() : r.u8();
			v2 = ver == 0x02FF || ver == 0x2FF;
			continue;
		}
		switch (op) {
		case 0x0000: break;
		case 0x00FF: r.p = r.end; break;
		case 0x0C00: r.skip(24); break;
		case 0x001E: break;
		case 0x0001: { uint16_t n = r.u16(); r.skip(n - 2); break; }
		case 0x0003: case 0x0004: case 0x0005: case 0x0008: case 0x000D: case 0x0015: case 0x0016:
		case 0x0023: case 0x00A0:
			r.skip(2);
			break;
		case 0x0007: case 0x000B: case 0x000C: case 0x000E: case 0x000F: case 0x0021: r.skip(4); break;
		case 0x001A: case 0x001B: case 0x001D: case 0x001F: r.skip(6); break;
		case 0x0002: case 0x0009: case 0x000A: case 0x0010: case 0x0020: r.skip(8); break;
		case 0x00A1: { r.skip(2); uint16_t n = r.u16(); r.skip(n); break; }
		case 0x0090: case 0x0091: case 0x0098: case 0x0099:
			bitsOp(r, cv, false, op & 1);
			break;
		case 0x009A: case 0x009B:
			bitsOp(r, cv, true, op & 1);
			break;
		case 0x8200: quickTimeOp(r, cv, true); break;
		case 0x8201: quickTimeOp(r, cv, false); break;
		case 0x0030: case 0x0031: case 0x0032: case 0x0033: case 0x0034: // rect ops: skip
			r.skip(8);
			break;
		case 0x0038: case 0x0039: case 0x003A: case 0x003B: case 0x003C: break;
		default:
			if (op >= 0x0100 && op <= 0x7FFF) {
				// Reserved opcodes carry their length in the high byte
				r.skip((size_t)(op >> 8) * 2);
				break;
			}
			logf("PICT: unhandled opcode 0x%04x", op);
			r.p = r.end;
			break;
		}
	}
	drawImage(port, cv.px.data(), cv.w, cv.h, dst);
}

} // namespace qd

IMPORT(DrawPicture) { qd::drawPicture(qd::current(), c.arg(0), qd::readRect(c.arg(1))); }
