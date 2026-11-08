// Text, theme drawing and CoreGraphics. Ports are wrapped in host
// CGBitmapContexts over their guest pixels, so CoreText and the host's JPEG
// decoder draw straight into the game's buffers.
#include "cf.h"
#include "emu.h"
#include "files.h"
#include "heap.h"
#include "host.h"
#include "qd.h"
#include "window.h"

#include <ApplicationServices/ApplicationServices.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

using namespace qd;

namespace {

// ---------------------------------------------------------------------------
// CG contexts over ports

struct PortContext {
	Port *port = nullptr;
	CGContextRef ctx = nullptr;
	std::vector<uint8_t> temp; // for ports that are not 32-bit
	Bits bits;
};

PortContext *beginPort(Port *p, bool qdCoordinates, bool clip = true) {
	PortContext *pc = new PortContext;
	pc->port = p;
	pc->bits = portBits(p);
	Bits &b = pc->bits;
	int w = b.bounds.width(), h = b.bounds.height();
	static CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	uint8_t *base = b.base;
	size_t rowBytes = b.rowBytes;
	if (b.depth != 32) {
		pc->temp.resize((size_t)w * h * 4);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				uint32_t c = getPixel(b, x, y);
				uint8_t *q = &pc->temp[((size_t)y * w + x) * 4];
				q[0] = 0xFF;
				q[1] = (uint8_t)(c >> 16);
				q[2] = (uint8_t)(c >> 8);
				q[3] = (uint8_t)c;
			}
		base = pc->temp.data();
		rowBytes = (size_t)w * 4;
	}
	pc->ctx = CGBitmapContextCreate(base, w, h, 8, rowBytes, rgb, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big);
	if (clip) {
		Rects cr = clipRects(p);
		std::vector<CGRect> rects;
		for (const qd::Rect &r : cr) {
			qd::Rect s = sect(r, b.bounds);
			if (!s.empty())
				rects.push_back(CGRectMake(s.left - b.bounds.left, h - (s.bottom - b.bounds.top), s.width(), s.height()));
		}
		if (rects.empty())
			rects.push_back(CGRectZero);
		CGContextClipToRects(pc->ctx, rects.data(), rects.size());
	}
	if (qdCoordinates) {
		// Local QuickDraw coordinates: y down, origin at the port's bounds
		CGContextTranslateCTM(pc->ctx, -b.bounds.left, h + b.bounds.top);
		CGContextScaleCTM(pc->ctx, 1, -1);
	}
	return pc;
}

void endPort(PortContext *pc) {
	CGContextFlush(pc->ctx);
	CGContextRelease(pc->ctx);
	Bits &b = pc->bits;
	if (!pc->temp.empty()) {
		int w = b.bounds.width(), h = b.bounds.height();
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				const uint8_t *q = &pc->temp[((size_t)y * w + x) * 4];
				uint32_t c = 0xFF000000u | q[1] << 16 | q[2] << 8 | q[3];
				if (getPixel(b, x, y) != c)
					putPixel(b, x, y, c);
			}
	}
	markDirty(pc->port);
	delete pc;
}

// ---------------------------------------------------------------------------
// Fonts

std::map<int16_t, std::string> familyNames = {
	{0, "LucidaGrande"}, {1, "Geneva"}, {2, "Times"}, {3, "Geneva"}, {4, "Monaco"}, {20, "Times"},
	{21, "Helvetica"}, {22, "Courier"}, {23, "Symbol"},
};
std::map<std::string, int16_t> familyIDs;
// Families from fonts the game activated (FMActivateFonts): name -> PostScript name
std::map<std::string, std::string> activated;

std::string psName(const std::string &family) {
	// QuickDraw family names to fonts that exist on current macOS
	static const std::map<std::string, std::string> subst = {
		{"Lucida Grande", "LucidaGrande"}, {"New York", "Times"}, {"Chicago", "LucidaGrande-Bold"},
		{"Charcoal", "LucidaGrande-Bold"}, {"Comic Sans MS", "ComicSansMS"}, {"Arial", "ArialMT"},
		{"Arial Black", "Arial-Black"}, {"Verdana", "Verdana"}, {"Trebuchet MS", "TrebuchetMS"},
		{"Georgia", "Georgia"}, {"Impact", "Impact"}, {"Times New Roman", "TimesNewRomanPSMT"},
	};
	auto a = activated.find(family);
	if (a != activated.end())
		return a->second;
	auto it = subst.find(family);
	return it == subst.end() ? family : it->second;
}

CTFontRef makeFont(int16_t family, int size, int face) {
	struct Key {
		int16_t f;
		int s, face;
		bool operator<(const Key &o) const { return std::tie(f, s, face) < std::tie(o.f, o.s, o.face); }
	};
	static std::map<Key, CTFontRef> cache;
	if (size <= 0)
		size = family == 0 ? 13 : 12;
	Key k{family, size, face & 3};
	auto it = cache.find(k);
	if (it != cache.end())
		return it->second;
	std::string name = familyNames.count(family) ? psName(familyNames[family]) : "LucidaGrande";
	CFStringRef n = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
	CTFontRef f = CTFontCreateWithName(n, size, nullptr);
	CFRelease(n);
	CTFontSymbolicTraits traits = 0;
	if (face & 1)
		traits |= kCTFontBoldTrait;
	if (face & 2)
		traits |= kCTFontItalicTrait;
	if (traits) {
		CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(f, size, nullptr, traits, traits);
		if (styled) {
			CFRelease(f);
			f = styled;
		}
	}
	cache[k] = f;
	return f;
}

CGColorRef cgColor(uint32_t argb) {
	static CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGFloat comps[4] = {(argb >> 16 & 0xFF) / 255.0, (argb >> 8 & 0xFF) / 255.0, (argb & 0xFF) / 255.0, 1.0};
	return CGColorCreate(rgb, comps);
}

CFAttributedStringRef attributed(CFStringRef s, CTFontRef font, uint32_t color, bool underline) {
	CGColorRef col = cgColor(color);
	int32_t u = underline ? kCTUnderlineStyleSingle : kCTUnderlineStyleNone;
	CFNumberRef un = CFNumberCreate(nullptr, kCFNumberSInt32Type, &u);
	const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTUnderlineStyleAttributeName};
	const void *vals[] = {font, col, un};
	CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef as = CFAttributedStringCreate(nullptr, s, attrs);
	CFRelease(attrs);
	CFRelease(un);
	CGColorRelease(col);
	return as;
}

CFStringRef hostString(const std::string &macRoman) {
	return CFStringCreateWithBytes(nullptr, (const UInt8 *)macRoman.data(), macRoman.size(), kCFStringEncodingMacRoman, false);
}

double lineWidth(CFStringRef s, CTFontRef font) {
	CFAttributedStringRef as = attributed(s, font, 0, false);
	CTLineRef line = CTLineCreateWithAttributedString(as);
	double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
	CFRelease(line);
	CFRelease(as);
	return w;
}

// Draws a single line at the pen position (baseline) and advances the pen
void drawLine(Port *p, const std::string &macRoman) {
	CFStringRef s = hostString(macRoman);
	if (!s)
		return;
	CTFontRef font = makeFont(p->txFont, p->txSize, p->txFace);
	CFAttributedStringRef as = attributed(s, font, argb(p->fg), p->txFace & 4);
	CTLineRef line = CTLineCreateWithAttributedString(as);
	double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
	PortContext *pc = beginPort(p, true);
	CGContextSetTextMatrix(pc->ctx, CGAffineTransformMakeScale(1, -1));
	CGContextSetTextPosition(pc->ctx, p->penH, p->penV);
	CTLineDraw(line, pc->ctx);
	endPort(pc);
	CFRelease(line);
	CFRelease(as);
	CFRelease(s);
	p->penH = (int16_t)(p->penH + (int)(w + 0.5));
}

// Lays out text in a rectangle (local coordinates)
void drawBox(Port *p, CFAttributedStringRef as, const qd::Rect &r, int just, bool wrap, int scrollV = 0) {
	CFMutableAttributedStringRef m = CFAttributedStringCreateMutableCopy(nullptr, 0, as);
	CTTextAlignment align = just == 1 ? kCTTextAlignmentCenter : just == -1 ? kCTTextAlignmentRight : kCTTextAlignmentLeft;
	CTLineBreakMode lb = wrap ? kCTLineBreakByWordWrapping : kCTLineBreakByTruncatingTail;
	CTParagraphStyleSetting set[2] = {{kCTParagraphStyleSpecifierAlignment, sizeof(align), &align},
		{kCTParagraphStyleSpecifierLineBreakMode, sizeof(lb), &lb}};
	CTParagraphStyleRef ps = CTParagraphStyleCreate(set, 2);
	CFAttributedStringSetAttribute(m, CFRangeMake(0, CFAttributedStringGetLength(m)), kCTParagraphStyleAttributeName, ps);
	CFRelease(ps);
	CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(m);
	PortContext *pc = beginPort(p, false);
	Bits &b = pc->bits;
	int h = b.bounds.height();
	// The frame starts at the top of the rectangle (scrolled) and runs down
	double top = h - (r.top - b.bounds.top) + scrollV;
	CGContextClipToRect(pc->ctx, CGRectMake(r.left - b.bounds.left, h - (r.bottom - b.bounds.top), r.width(), r.height()));
	CGMutablePathRef path = CGPathCreateMutable();
	CGPathAddRect(path, nullptr, CGRectMake(r.left - b.bounds.left, top - 100000, r.width(), 100000));
	CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, nullptr);
	CTFrameDraw(frame, pc->ctx);
	CFRelease(frame);
	CGPathRelease(path);
	endPort(pc);
	CFRelease(fs);
	CFRelease(m);
}

double boxHeight(CFAttributedStringRef as, int width) {
	CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(as);
	CGSize s = CTFramesetterSuggestFrameSizeWithConstraints(fs, CFRangeMake(0, 0), nullptr, CGSizeMake(width, 1e6), nullptr);
	CFRelease(fs);
	return s.height;
}

CTFontRef themeFont(int16_t id, Port *p) {
	switch (id) {
	case 1: return makeFont(0, 11, 0);  // small system
	case 2: return makeFont(0, 11, 1);  // small emphasized
	case 4: return makeFont(0, 13, 1);  // emphasized system
	case 3: return makeFont(0, 12, 0);  // views
	case 5: return makeFont(0, 9, 0);   // mini
	case 7: return makeFont(0, 10, 0);  // label
	case 200: return makeFont(p->txFont, p->txSize, p->txFace); // current port
	default: return makeFont(0, 13, 0);
	}
}

// Host objects handed to the guest
std::unordered_map<gaddr, CFTypeRef> hostObjects;
gaddr wrap(CFTypeRef o) {
	if (!o)
		return 0;
	gaddr a = heap::alloc(16, true);
	hostObjects[a] = o;
	return a;
}
CFTypeRef unwrap(gaddr a) {
	auto it = hostObjects.find(a);
	return it == hostObjects.end() ? nullptr : it->second;
}

std::unordered_map<gaddr, PortContext *> cgContexts;

} // namespace

// ---------------------------------------------------------------------------
// Text drawing

static int16_t familyID(const std::string &name) {
	for (auto &it : familyNames) {
		if (it.second == name)
			return it.first;
	}
	auto it = familyIDs.find(name);
	if (it != familyIDs.end())
		return it->second;
	int16_t id = (int16_t)(1000 + familyIDs.size());
	familyIDs[name] = id;
	familyNames[id] = name;
	logf("font family %s -> %d", name.c_str(), id);
	return id;
}

IMPORT(GetFNum) { emu::w16(c.arg(1), (uint16_t)familyID(cf::macRomanToUtf8(emu::pstr(c.arg(0))))); }
IMPORT(GetFontName) {
	int16_t id = c.sarg16(0);
	emu::wpstr(c.arg(1), familyNames.count(id) ? cf::utf8ToMacRoman(familyNames[id]) : std::string());
}
IMPORT(GetFontInfo) {
	// FontInfo: ascent, descent, widMax, leading
	Port *p = current();
	CTFontRef f = makeFont(p->txFont, p->txSize, p->txFace);
	emu::w16(c.arg(0), (uint16_t)lround(CTFontGetAscent(f)));
	emu::w16(c.arg(0) + 2, (uint16_t)lround(CTFontGetDescent(f)));
	emu::w16(c.arg(0) + 4, (uint16_t)lround(CTFontGetBoundingBox(f).size.width));
	emu::w16(c.arg(0) + 6, (uint16_t)lround(CTFontGetLeading(f)));
}
IMPORT(TextWidth) {
	// (textBuf, firstByte, byteCount)
	Port *p = current();
	std::string text((const char *)emu::ptr(c.arg(0) + c.sarg16(1)), (size_t)std::max<int16_t>(c.sarg16(2), 0));
	CFStringRef s = hostString(text);
	double w = s ? lineWidth(s, makeFont(p->txFont, p->txSize, p->txFace)) : 0;
	if (s)
		CFRelease(s);
	c.ret((uint16_t)(int)(w + 0.5));
}
IMPORT(OutlineMetrics) {
	// (byteCount, textPtr, numer, denom, short *yMax, short *yMin, Fixed *awArray,
	//  Fixed *lsbArray, Rect *boundsArray): per-character glyph metrics
	Port *p = current();
	int n = std::max<int16_t>(c.sarg16(0), 0);
	CTFontRef f = makeFont(p->txFont, p->txSize, p->txFace);
	CFStringRef s = hostString(std::string((const char *)emu::ptr(c.arg(1)), n));
	CFIndex len = s ? CFStringGetLength(s) : 0;
	std::vector<UniChar> chars(len);
	if (len)
		CFStringGetCharacters(s, CFRangeMake(0, len), chars.data());
	std::vector<CGGlyph> glyphs(len);
	if (len)
		CTFontGetGlyphsForCharacters(f, chars.data(), glyphs.data(), len);
	double yMax = 0, yMin = 0;
	for (CFIndex i = 0; i < len && i < n; i++) {
		CGRect b = CTFontGetBoundingRectsForGlyphs(f, kCTFontOrientationHorizontal, &glyphs[i], nullptr, 1);
		CGSize adv;
		CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &glyphs[i], &adv, 1);
		yMax = std::max(yMax, (double)CGRectGetMaxY(b));
		yMin = std::min(yMin, (double)CGRectGetMinY(b));
		if (gaddr aw = c.arg(6))
			emu::w32(aw + 4 * i, (uint32_t)lround(adv.width * 65536));
		if (gaddr lsb = c.arg(7))
			emu::w32(lsb + 4 * i, (uint32_t)lround(CGRectGetMinX(b) * 65536));
		if (gaddr r = c.arg(8)) {
			// y up from the baseline, as in the glyph's own coordinates
			emu::w16(r + 8 * i, (uint16_t)(int16_t)ceil(CGRectGetMaxY(b)));
			emu::w16(r + 8 * i + 2, (uint16_t)(int16_t)floor(CGRectGetMinX(b)));
			emu::w16(r + 8 * i + 4, (uint16_t)(int16_t)floor(CGRectGetMinY(b)));
			emu::w16(r + 8 * i + 6, (uint16_t)(int16_t)ceil(CGRectGetMaxX(b)));
		}
	}
	if (s)
		CFRelease(s);
	if (c.arg(4))
		emu::w16(c.arg(4), (uint16_t)(int16_t)ceil(yMax));
	if (c.arg(5))
		emu::w16(c.arg(5), (uint16_t)(int16_t)floor(yMin));
	c.ret(0);
}
IMPORT(IsOutline) { c.ret(1); }
IMPORT(SetOutlinePreferred) {}

IMPORT(FMActivateFonts) {
	// (const FSSpec *, filter, refCon, options): registers the fonts in the
	// file for this process and remembers their family names
	std::string path = files::pathOfFSSpec(c.arg(0));
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), path.size(), false);
	CFErrorRef err = nullptr;
	if (!CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &err) && err) {
		CFRelease(err);
		err = nullptr;
	}
	if (CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL(url)) {
		for (CFIndex i = 0; i < CFArrayGetCount(descs); i++) {
			CTFontDescriptorRef d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, i);
			CFStringRef fam = (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontFamilyNameAttribute);
			CFStringRef ps = (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute);
			char f[256] = "", n[256] = "";
			if (fam)
				CFStringGetCString(fam, f, sizeof f, kCFStringEncodingUTF8);
			if (ps)
				CFStringGetCString(ps, n, sizeof n, kCFStringEncodingUTF8);
			// The plain style wins when a family has several
			if (*f && *n && (!activated.count(f) || std::string(n).find('-') == std::string::npos))
				activated[f] = n;
			logf("font %s: family %s, PostScript %s", path.c_str(), f, n);
			if (fam)
				CFRelease(fam);
			if (ps)
				CFRelease(ps);
		}
		CFRelease(descs);
	}
	CFRelease(url);
	c.ret(0);
}
IMPORT(FMDeactivateFonts) { c.ret(0); }

IMPORT(FMGetFontFamilyFromName) {
	std::string name = emu::pstr(c.arg(0));
	for (auto &it : familyNames) {
		if (it.second == name) {
			c.ret((uint16_t)it.first);
			return;
		}
	}
	auto it = familyIDs.find(name);
	if (it == familyIDs.end()) {
		int16_t id = (int16_t)(1000 + familyIDs.size());
		familyIDs[name] = id;
		familyNames[id] = name;
		logf("font family %s -> %d", name.c_str(), id);
		c.ret((uint16_t)id);
		return;
	}
	c.ret((uint16_t)it->second);
}

IMPORT(TextFont) { current()->txFont = c.sarg16(0); }
IMPORT(TextSize) { current()->txSize = c.sarg16(0); }
IMPORT(TextFace) { current()->txFace = (uint8_t)c.arg(0); }
IMPORT(TextMode) { current()->txMode = c.sarg16(0); }
IMPORT(GetPortTextFont) { c.ret((uint16_t)port(c.arg(0))->txFont); }
IMPORT(GetPortTextSize) { c.ret((uint16_t)port(c.arg(0))->txSize); }
IMPORT(GetPortTextFace) { c.ret(port(c.arg(0))->txFace); }
IMPORT(GetPortTextMode) { c.ret((uint16_t)port(c.arg(0))->txMode); }

IMPORT(DrawString) {
	if (emu::traceImports)
		logf("DrawString \"%s\"", emu::pstr(c.arg(0)).c_str());
	drawLine(current(), emu::pstr(c.arg(0)));
}
IMPORT(DrawText) {
	// (const void *text, first, byteCount)
	drawLine(current(), std::string((const char *)emu::ptr(c.arg(0) + c.sarg16(1)), (size_t)c.sarg16(2)));
}
IMPORT(StringWidth) {
	Port *p = current();
	CFStringRef s = hostString(emu::pstr(c.arg(0)));
	double w = s ? lineWidth(s, makeFont(p->txFont, p->txSize, p->txFace)) : 0;
	if (s)
		CFRelease(s);
	c.ret((uint16_t)(int)(w + 0.5));
}

IMPORT(DrawThemeTextBox) {
	// (CFStringRef, ThemeFontID, ThemeDrawState, Boolean wrap, const qd::Rect *, SInt16 just, void *)
	Port *p = current();
	std::u16string u = cf::utf16(cf::utf8(c.arg(0)));
	CFStringRef s = CFStringCreateWithCharacters(nullptr, (const UniChar *)u.data(), u.size());
	CFAttributedStringRef as = attributed(s, themeFont(c.sarg16(1), p), argb(p->fg), false);
	drawBox(p, as, readRect(c.arg(4)), c.sarg16(5), (c.arg(3) & 0xFF) != 0);
	CFRelease(as);
	CFRelease(s);
	c.ret(0);
}

IMPORT(SetThemeTextColor) {
	int16_t color = c.sarg16(0);
	Port *p = current();
	p->fg = color == -2 || color == 8 ? RGB{0xFFFF, 0xFFFF, 0xFFFF} : color == 2 || color == 4 ? RGB{0x7FFF, 0x7FFF, 0x7FFF} : RGB{};
	c.ret(0);
}

// ---------------------------------------------------------------------------
// Theme drawing

IMPORT(SetThemeBackground) {
	int16_t brush = c.sarg16(0);
	RGB bg = {0xFFFF, 0xFFFF, 0xFFFF};
	if (brush == -2)
		bg = RGB{};
	else if (brush > 0 && brush < 15)
		bg = {0xECEC, 0xECEC, 0xECEC};
	current()->bg = bg;
	c.ret(0);
}
IMPORT(ApplyThemeBackground) {
	current()->bg = {0xECEC, 0xECEC, 0xECEC};
	c.ret(0);
}
IMPORT(DrawThemeEditTextFrame) {
	qd::Rect r = readRect(c.arg(0));
	qd::Rect o = r;
	o.top -= 1;
	o.left -= 1;
	o.bottom += 1;
	o.right += 1;
	frameRect(current(), o, 0xFF8E8E8E);
	c.ret(0);
}
IMPORT(DrawThemePlacard) {
	qd::Rect r = readRect(c.arg(0));
	fillRect(current(), r, 0xFFE8E8E8);
	frameRect(current(), r, 0xFFA0A0A0);
	c.ret(0);
}

// The drawing state is saved in a small guest block
IMPORT(GetThemeDrawingState) {
	Port *p = current();
	gaddr s = heap::alloc(32, true);
	writeRGB(s, p->fg);
	writeRGB(s + 6, p->bg);
	emu::w16(s + 12, (uint16_t)p->txFont);
	emu::w16(s + 14, (uint16_t)p->txSize);
	emu::w16(s + 16, p->txFace);
	emu::w16(s + 18, (uint16_t)p->txMode);
	emu::w32(c.arg(0), s);
	c.ret(0);
}
IMPORT(SetThemeDrawingState) {
	Port *p = current();
	gaddr s = c.arg(0);
	if (s) {
		p->fg = readRGB(s);
		p->bg = readRGB(s + 6);
		p->txFont = (int16_t)emu::r16(s + 12);
		p->txSize = (int16_t)emu::r16(s + 14);
		p->txFace = (uint8_t)emu::r16(s + 16);
		p->txMode = (int16_t)emu::r16(s + 18);
		if (c.arg(1) & 0xFF)
			heap::free(s);
	}
	c.ret(0);
}
IMPORT(NormalizeThemeDrawingState) {
	Port *p = current();
	p->fg = RGB{};
	p->bg = {0xFFFF, 0xFFFF, 0xFFFF};
	p->txFont = 0;
	p->txSize = 0;
	p->txFace = 0;
	p->txMode = 1;
	c.ret(0);
}

// ---------------------------------------------------------------------------
// CoreGraphics

IMPORT(QDBeginCGContext) {
	Port *p = port(c.arg(0));
	if (!p) {
		c.ret((uint32_t)-50);
		return;
	}
	PortContext *pc = beginPort(p, false, true);
	gaddr ref = wrap(pc->ctx);
	CGContextRetain(pc->ctx);
	cgContexts[ref] = pc;
	emu::w32(c.arg(1), ref);
	c.ret(0);
}
IMPORT(QDEndCGContext) {
	gaddr ref = emu::r32(c.arg(1));
	auto it = cgContexts.find(ref);
	if (it != cgContexts.end()) {
		CGContextRelease(it->second->ctx);
		endPort(it->second);
		cgContexts.erase(it);
		hostObjects.erase(ref);
	}
	emu::w32(c.arg(1), 0);
	c.ret(0);
}
IMPORT(CGContextSynchronize) {}
IMPORT(CGContextDrawImage) {
	// (CGContextRef, CGRect by value as four floats, CGImageRef)
	CGContextRef ctx = (CGContextRef)unwrap(c.arg(0));
	CGImageRef img = (CGImageRef)unwrap(c.arg(5));
	if (!ctx || !img)
		return;
	CGRect r = CGRectMake(c.argf(1), c.argf(2), c.argf(3), c.argf(4));
	CGContextDrawImage(ctx, r, img);
	auto it = cgContexts.find(c.arg(0));
	if (it != cgContexts.end())
		markDirty(it->second->port);
}
IMPORT(CGDataProviderCreateWithURL) {
	std::string path = cf::get(c.arg(0)) ? cf::get(c.arg(0))->path : std::string();
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), path.size(), false);
	CGDataProviderRef dp = CGDataProviderCreateWithURL(url);
	CFRelease(url);
	c.ret(wrap(dp));
}
IMPORT(CGDataProviderRelease) {}
IMPORT(CGImageCreateWithJPEGDataProvider) {
	CGDataProviderRef dp = (CGDataProviderRef)unwrap(c.arg(0));
	CGImageRef img = dp ? CGImageCreateWithJPEGDataProvider(dp, nullptr, c.arg(2) & 0xFF, kCGRenderingIntentDefault) : nullptr;
	c.ret(wrap(img));
}
IMPORT(CGImageGetWidth) {
	CGImageRef img = (CGImageRef)unwrap(c.arg(0));
	c.ret(img ? (uint32_t)CGImageGetWidth(img) : 0);
}
IMPORT(CGImageGetHeight) {
	CGImageRef img = (CGImageRef)unwrap(c.arg(0));
	c.ret(img ? (uint32_t)CGImageGetHeight(img) : 0);
}

IMPORT(CGDataProviderCreateWithData) {
	// (info, data, size, releaseCallback): the provider reads guest memory in place
	CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, emu::ptr(c.arg(1)), c.arg(2), nullptr);
	c.ret(wrap(dp));
}
IMPORT(CGColorSpaceCreateDeviceRGB) { c.ret(wrap(CGColorSpaceCreateDeviceRGB())); }
IMPORT(CGColorSpaceCreateWithPlatformColorSpace) { c.ret(wrap(CGColorSpaceCreateDeviceRGB())); }
IMPORT(CGColorSpaceRelease) {}
IMPORT(CGImageRelease) {}
IMPORT(CGImageCreate) {
	// (width, height, bitsPerComponent, bitsPerPixel, bytesPerRow, space, bitmapInfo,
	//  provider, decode, shouldInterpolate, intent)
	CGColorSpaceRef space = (CGColorSpaceRef)unwrap(c.arg(5));
	CGDataProviderRef dp = (CGDataProviderRef)unwrap(c.arg(7));
	if (!space || !dp) {
		c.ret(0);
		return;
	}
	CGImageRef img = CGImageCreate(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4), space, c.arg(6), dp, nullptr,
		c.arg(9) & 0xFF, (CGColorRenderingIntent)c.arg(10));
	c.ret(wrap(img));
}
IMPORT(CGContextFlush) {
	auto it = cgContexts.find(c.arg(0));
	if (it != cgContexts.end())
		markDirty(it->second->port);
}

// ---------------------------------------------------------------------------
// QuickTime graphics importers, on ImageIO

namespace {

struct Importer {
	CGImageRef image = nullptr;
	gaddr port = 0;
	qd::Rect bounds; // where it draws in the port
};
std::map<gaddr, Importer> importers;

} // namespace

IMPORT(GetGraphicsImporterForFile) {
	// (const FSSpec *, ComponentInstance *)
	std::string path = files::pathOfFSSpec(c.arg(0));
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), path.size(), false);
	CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
	CFRelease(url);
	CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
	if (src)
		CFRelease(src);
	if (!img) {
		logf("GetGraphicsImporterForFile: can't read %s", path.c_str());
		emu::w32(c.arg(1), 0);
		c.ret((uint32_t)-2005); // badComponentType
		return;
	}
	gaddr gi = heap::alloc(16, true);
	Importer &im = importers[gi];
	im.image = img;
	im.bounds.right = (int16_t)CGImageGetWidth(img);
	im.bounds.bottom = (int16_t)CGImageGetHeight(img);
	logf("graphics importer for %s: %dx%d", path.c_str(), im.bounds.right, im.bounds.bottom);
	emu::w32(c.arg(1), gi);
	c.ret(0);
}
IMPORT(GraphicsImportGetImageDescription) {
	// ImageDescription: idSize, cType, ..., width 32, height 34, hRes 36, vRes 40,
	// dataSize 44, frameCount 48, name 50, depth 82, clutID 84
	auto it = importers.find(c.arg(0));
	if (it == importers.end()) {
		c.ret((uint32_t)-50);
		return;
	}
	gaddr h = heap::newHandle(86, true);
	gaddr d = heap::deref(h);
	emu::w32(d, 86);
	emu::w32(d + 4, 0x74696666); // 'tiff'
	emu::w16(d + 32, (uint16_t)it->second.bounds.right);
	emu::w16(d + 34, (uint16_t)it->second.bounds.bottom);
	emu::w32(d + 36, 72 << 16);
	emu::w32(d + 40, 72 << 16);
	emu::w16(d + 48, 1);
	CGImageAlphaInfo a = CGImageGetAlphaInfo(it->second.image);
	bool alpha = a != kCGImageAlphaNone && a != kCGImageAlphaNoneSkipFirst && a != kCGImageAlphaNoneSkipLast;
	emu::w16(d + 82, alpha ? 32 : 24);
	emu::w16(d + 84, (uint16_t)-1);
	emu::w32(c.arg(1), h);
	c.ret(0);
}
IMPORT(GraphicsImportGetNaturalBounds) {
	auto it = importers.find(c.arg(0));
	qd::Rect r;
	if (it != importers.end()) {
		r.right = (int16_t)CGImageGetWidth(it->second.image);
		r.bottom = (int16_t)CGImageGetHeight(it->second.image);
	}
	qd::writeRect(c.arg(1), r);
	c.ret(0);
}
IMPORT(GraphicsImportSetBoundsRect) {
	auto it = importers.find(c.arg(0));
	if (it != importers.end())
		it->second.bounds = qd::readRect(c.arg(1));
	c.ret(0);
}
IMPORT(GraphicsImportGetColorSyncProfile) {
	emu::w32(c.arg(1), 0); // no embedded profile: device RGB
	c.ret(0);
}
IMPORT(GraphicsImportSetGWorld) {
	auto it = importers.find(c.arg(0));
	if (it != importers.end())
		it->second.port = c.arg(1);
	c.ret(0);
}
IMPORT(GraphicsImportDraw) {
	auto it = importers.find(c.arg(0));
	Port *p = it == importers.end() ? nullptr : port(it->second.port ? it->second.port : current()->addr);
	if (!p) {
		c.ret((uint32_t)-50);
		return;
	}
	PortContext *pc = beginPort(p, true, true);
	const qd::Rect &r = it->second.bounds;
	// The context is flipped to QuickDraw coordinates, so flip the image back
	CGContextSaveGState(pc->ctx);
	CGContextTranslateCTM(pc->ctx, r.left, r.bottom);
	CGContextScaleCTM(pc->ctx, 1, -1);
	CGContextDrawImage(pc->ctx, CGRectMake(0, 0, r.width(), r.height()), it->second.image);
	CGContextRestoreGState(pc->ctx);
	endPort(pc);
	c.ret(0);
}
IMPORT(CloseComponent) {
	auto it = importers.find(c.arg(0));
	if (it != importers.end()) {
		CGImageRelease(it->second.image);
		importers.erase(it);
		heap::free(c.arg(0));
	}
	c.ret(0);
}
IMPORT(CMOpenProfile) { c.ret((uint32_t)-171); } // cmProfileError: the caller falls back to device RGB
IMPORT(CMValidateProfile) { c.ret((uint32_t)-171); }

// ---------------------------------------------------------------------------
// TextEdit: enough to show styled text read from TEXT/styl resources

namespace {

struct TERec {
	qd::Rect dest, view;
	std::string text;
	struct Run {
		int start;
		int16_t font, size;
		uint8_t face;
		RGB color;
	};
	std::vector<Run> runs;
	int just = 0;
	int scrollV = 0;
	gaddr handle;
};
std::unordered_map<gaddr, TERec *> teRecs;

CFAttributedStringRef teString(TERec *te) {
	CFStringRef s = hostString(te->text);
	CFMutableAttributedStringRef m = CFAttributedStringCreateMutable(nullptr, 0);
	CFAttributedStringReplaceString(m, CFRangeMake(0, 0), s);
	CFIndex len = CFStringGetLength(s);
	CFRelease(s);
	std::vector<TERec::Run> runs = te->runs;
	if (runs.empty())
		runs.push_back({0, 0, 12, 0, RGB{}});
	for (size_t i = 0; i < runs.size(); i++) {
		CFIndex a = std::min<CFIndex>(runs[i].start, len);
		CFIndex b = i + 1 < runs.size() ? std::min<CFIndex>(runs[i + 1].start, len) : len;
		if (b <= a)
			continue;
		CTFontRef font = makeFont(runs[i].font, runs[i].size, runs[i].face);
		CGColorRef col = cgColor(argb(runs[i].color));
		CFAttributedStringSetAttribute(m, CFRangeMake(a, b - a), kCTFontAttributeName, font);
		CFAttributedStringSetAttribute(m, CFRangeMake(a, b - a), kCTForegroundColorAttributeName, col);
		CGColorRelease(col);
	}
	return m;
}

TERec *te(gaddr h) {
	auto it = teRecs.find(h);
	if (it == teRecs.end())
		emu::fatal("unknown TEHandle 0x%08x", h);
	return it->second;
}

} // namespace

IMPORT(TEStyleNew) {
	TERec *t = new TERec;
	t->dest = readRect(c.arg(0));
	t->view = readRect(c.arg(1));
	t->handle = heap::newHandle(128, true);
	teRecs[t->handle] = t;
	c.ret(t->handle);
}
IMPORT(TEDispose) {
	auto it = teRecs.find(c.arg(0));
	if (it != teRecs.end()) {
		heap::disposeHandle(it->first);
		delete it->second;
		teRecs.erase(it);
	}
}
IMPORT(TESetText) {
	TERec *t = te(c.arg(2));
	t->text.assign((const char *)emu::ptr(c.arg(0)), c.arg(1));
	for (char &ch : t->text) {
		if (ch == '\r')
			ch = '\n';
	}
}
IMPORT(TEUseStyleScrap) {
	// (start, end, StScrpHandle, redraw, hTE): native entries of
	// {startChar 4, height 2, ascent 2, font 2, face 1, pad 1, size 2, color 6}
	TERec *t = te(c.arg(4));
	gaddr h = c.arg(2);
	if (!h)
		return;
	gaddr p = heap::deref(h);
	int n = emu::r16(p);
	t->runs.clear();
	for (int i = 0; i < n; i++) {
		gaddr e = p + 2 + 20 * i;
		TERec::Run r;
		r.start = (int)emu::r32(e);
		r.font = (int16_t)emu::r16(e + 8);
		r.face = emu::r8(e + 10);
		r.size = (int16_t)emu::r16(e + 12);
		r.color = qd::readRGB(e + 14);
		t->runs.push_back(r);
	}
}
IMPORT(TESetAlignment) { te(c.arg(1))->just = c.sarg16(0); }
IMPORT(TECalText) {}
IMPORT(TEGetHeight) {
	TERec *t = te(c.arg(2));
	CFAttributedStringRef as = teString(t);
	double h = boxHeight(as, t->dest.width());
	CFRelease(as);
	c.ret((uint32_t)(int)(h + 0.5));
}
IMPORT(TEScroll) {
	TERec *t = te(c.arg(2));
	t->scrollV -= c.sarg16(1);
	qd::Rect v = t->view;
	fillRect(current(), v, argb(current()->bg));
	CFAttributedStringRef as = teString(t);
	drawBox(current(), as, sect(t->view, t->dest.empty() ? t->view : t->view), t->just, true, t->scrollV);
	CFRelease(as);
}
IMPORT(TEUpdate) {
	TERec *t = te(c.arg(1));
	CFAttributedStringRef as = teString(t);
	drawBox(current(), as, t->view, t->just, true, t->scrollV);
	CFRelease(as);
}

// ---------------------------------------------------------------------------
// MLTE: text objects that only display

namespace {
struct TXN {
	qd::Rect frame;
	gaddr window;
	std::u16string text;
};
std::unordered_map<gaddr, TXN *> txns;
}

IMPORT(TXNInitTextension) { c.ret(0); }
IMPORT(TXNNewObject) {
	// (FSSpec *, WindowRef, const qd::Rect *frame, options, frameType, fileType, encoding, TXNObject *, TXNFrameID *, refcon)
	TXN *t = new TXN;
	t->window = c.arg(1);
	if (c.arg(2))
		t->frame = readRect(c.arg(2));
	gaddr ref = heap::alloc(16, true);
	txns[ref] = t;
	emu::w32(c.arg(7), ref);
	if (c.arg(8))
		emu::w32(c.arg(8), 1);
	c.ret(0);
}
IMPORT(TXNDeleteObject) {
	auto it = txns.find(c.arg(0));
	if (it != txns.end()) {
		delete it->second;
		txns.erase(it);
	}
}
IMPORT(TXNSetData) {
	// (obj, dataType, data, size, start, end)
	auto it = txns.find(c.arg(0));
	if (it == txns.end()) {
		c.ret((uint32_t)-50);
		return;
	}
	const uint8_t *d = emu::ptr(c.arg(2));
	if (c.arg(1) == 0x75747874) // 'utxt'
		it->second->text.assign((const char16_t *)d, c.arg(3) / 2);
	else
		it->second->text = cf::utf16(cf::macRomanToUtf8(std::string((const char *)d, c.arg(3))));
	c.ret(0);
}
IMPORT(TXNDraw) {
	auto it = txns.find(c.arg(0));
	if (it == txns.end())
		return;
	TXN *t = it->second;
	wm::Window *w = wm::window(t->window);
	Port *p = w ? w->port : current();
	CFStringRef s = CFStringCreateWithCharacters(nullptr, (const UniChar *)t->text.data(), t->text.size());
	CFAttributedStringRef as = attributed(s, makeFont(0, 12, 0), 0xFF000000u, false);
	fillRect(p, t->frame, 0xFFFFFFFFu);
	drawBox(p, as, t->frame, 0, true);
	CFRelease(as);
	CFRelease(s);
}
IMPORT(TXNActivate) { c.ret(0); }
IMPORT(TXNFocus) {}
IMPORT(TXNSetSelection) { c.ret(0); }
IMPORT(TXNShowSelection) {}
IMPORT(TXNSave) { c.ret((uint32_t)-50); }
IMPORT(TXNPrint) { c.ret((uint32_t)-128); }

namespace txt {

// A label for controls: UTF-8 text in a rectangle of the port
void drawLabel(Port *p, const std::string &utf8, const qd::Rect &r, int just, int size, bool bold, uint32_t color) {
	if (utf8.empty())
		return;
	std::u16string u = cf::utf16(utf8);
	CFStringRef s = CFStringCreateWithCharacters(nullptr, (const UniChar *)u.data(), u.size());
	CFAttributedStringRef as = attributed(s, makeFont(0, size, bold ? 1 : 0), color, false);
	// Center vertically for single lines
	double lh = CTFontGetAscent(makeFont(0, size, 0)) + CTFontGetDescent(makeFont(0, size, 0)) + 2;
	qd::Rect box = r;
	if (r.height() < lh * 1.8) {
		int pad = (int)((r.height() - lh) / 2);
		box.top = (int16_t)(r.top + std::max(pad, 0));
	}
	drawBox(p, as, box, just, r.height() >= lh * 1.8);
	CFRelease(as);
	CFRelease(s);
}

int labelWidth(const std::string &utf8, int size) {
	std::u16string u = cf::utf16(utf8);
	CFStringRef s = CFStringCreateWithCharacters(nullptr, (const UniChar *)u.data(), u.size());
	double w = lineWidth(s, makeFont(0, size, 0));
	CFRelease(s);
	return (int)(w + 0.5);
}

} // namespace txt
