// CoreFoundation for the guest. Objects live natively; the guest holds the
// address of a small block that identifies them. Property lists on disk
// (Info.plist, .strings, preferences) go through the host's CoreFoundation.
#include "cf.h"
#include "files.h"
#include "guestfmt.h"
#include "heap.h"
#include "host.h"
#include "macho.h"

#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <map>
#include <sys/stat.h>

namespace cf {

namespace {

std::map<gaddr, Obj *> objects;

const char16_t kMacRoman[128] = {
	0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
	0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
	0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
	0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
	0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
	0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
	0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
	0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

const uint32_t kEncUTF8 = 0x08000100, kEncUnicode = 0x0100; // others are 8-bit Mac Roman

std::u16string fromMacRoman(const std::string &s) {
	std::u16string u;
	for (unsigned char ch : s)
		u += ch < 0x80 ? (char16_t)ch : kMacRoman[ch - 0x80];
	return u;
}

std::string toMacRoman(const std::u16string &u) {
	std::string s;
	for (char16_t ch : u) {
		if (ch < 0x80) {
			s += (char)ch;
			continue;
		}
		char out = '?';
		for (int i = 0; i < 128; i++) {
			if (kMacRoman[i] == ch) {
				out = (char)(0x80 + i);
				break;
			}
		}
		s += out;
	}
	return s;
}

std::u16string decode(const std::string &bytes, uint32_t enc) {
	if (enc == kEncUTF8)
		return utf16(bytes);
	return fromMacRoman(bytes);
}

std::string encode(const std::u16string &s, uint32_t enc) {
	if (enc == kEncUTF8)
		return utf8(s);
	return toMacRoman(s);
}

// Constant strings compiled into the image: {isa, flags, const char *, length}
Obj *constantString(gaddr ref) {
	Obj *o = new Obj{kString};
	o->refs = 1 << 30;
	o->str = fromMacRoman(std::string((const char *)emu::ptr(emu::r32(ref + 8)), emu::r32(ref + 12)));
	objects[ref] = o;
	return o;
}

// --- Host CoreFoundation bridging

gaddr fromHost(CFTypeRef v) {
	if (!v)
		return 0;
	CFTypeID t = CFGetTypeID(v);
	if (t == CFStringGetTypeID()) {
		CFStringRef s = (CFStringRef)v;
		std::u16string u(CFStringGetLength(s), 0);
		CFStringGetCharacters(s, CFRangeMake(0, u.size()), (UniChar *)&u[0]);
		return makeString(u);
	}
	if (t == CFNumberGetTypeID()) {
		Obj *o = new Obj{kNumber};
		if (CFNumberIsFloatType((CFNumberRef)v)) {
			o->isFloat = true;
			CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &o->dval);
			o->ival = (int64_t)o->dval;
		} else {
			CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &o->ival);
			o->dval = (double)o->ival;
		}
		return make(o);
	}
	if (t == CFBooleanGetTypeID()) {
		Obj *o = new Obj{kNumber};
		o->ival = CFBooleanGetValue((CFBooleanRef)v);
		o->dval = (double)o->ival;
		return make(o);
	}
	if (t == CFArrayGetTypeID()) {
		Obj *o = new Obj{kArray};
		for (CFIndex i = 0; i < CFArrayGetCount((CFArrayRef)v); i++)
			o->items.push_back(fromHost(CFArrayGetValueAtIndex((CFArrayRef)v, i)));
		return make(o);
	}
	if (t == CFDictionaryGetTypeID()) {
		Obj *o = new Obj{kDictionary};
		CFIndex n = CFDictionaryGetCount((CFDictionaryRef)v);
		std::vector<const void *> keys(n), vals(n);
		CFDictionaryGetKeysAndValues((CFDictionaryRef)v, keys.data(), vals.data());
		for (CFIndex i = 0; i < n; i++)
			o->pairs.push_back({fromHost(keys[i]), fromHost(vals[i])});
		return make(o);
	}
	logf("cf: unsupported host type in property list");
	return 0;
}

CFTypeRef toHost(gaddr ref) {
	Obj *o = get(ref);
	if (!o)
		return nullptr;
	switch (o->type) {
	case kString:
		return CFStringCreateWithCharacters(nullptr, (const UniChar *)o->str.data(), o->str.size());
	case kNumber:
		if (o->isFloat)
			return CFNumberCreate(nullptr, kCFNumberDoubleType, &o->dval);
		return CFNumberCreate(nullptr, kCFNumberSInt64Type, &o->ival);
	case kArray: {
		CFMutableArrayRef a = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
		for (gaddr it : o->items) {
			CFTypeRef h = toHost(it);
			if (h) {
				CFArrayAppendValue(a, h);
				CFRelease(h);
			}
		}
		return a;
	}
	case kDictionary: {
		CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		for (auto &p : o->pairs) {
			CFTypeRef k = toHost(p.first), v = toHost(p.second);
			if (k && v)
				CFDictionarySetValue(d, k, v);
			if (k)
				CFRelease(k);
			if (v)
				CFRelease(v);
		}
		return d;
	}
	default:
		return nullptr;
	}
}

CFPropertyListRef readPlist(const std::string &path) {
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), path.size(), false);
	CFReadStreamRef stream = CFReadStreamCreateWithFile(nullptr, url);
	CFRelease(url);
	if (!stream)
		return nullptr;
	CFPropertyListRef result = nullptr;
	if (CFReadStreamOpen(stream)) {
		result = CFPropertyListCreateWithStream(nullptr, stream, 0, kCFPropertyListImmutable, nullptr, nullptr);
		CFReadStreamClose(stream);
	}
	CFRelease(stream);
	return result;
}

bool fileExists(const std::string &path) {
	struct stat st;
	return ::stat(path.c_str(), &st) == 0;
}

} // namespace

Obj *get(gaddr ref) {
	if (!ref)
		return nullptr;
	auto it = objects.find(ref);
	if (it != objects.end())
		return it->second;
	if (ref >= gImage.cfstringStart && ref < gImage.cfstringEnd)
		return constantString(ref);
	return nullptr;
}

gaddr make(Obj *o) {
	gaddr a = heap::alloc(16, true);
	emu::w32(a, 0x43464F42); // 'CFOB'
	emu::w32(a + 4, o->type);
	objects[a] = o;
	return a;
}

gaddr makeString(const std::u16string &s) {
	Obj *o = new Obj{kString};
	o->str = s;
	return make(o);
}

gaddr makeString(const std::string &s) {
	return makeString(utf16(s));
}

gaddr makeURL(const std::string &path) {
	Obj *o = new Obj{kURL};
	o->path = path;
	return make(o);
}

std::string utf8(const std::u16string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		uint32_t cp = s[i];
		if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size())
			cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
		if (cp < 0x80) {
			out += (char)cp;
		} else if (cp < 0x800) {
			out += (char)(0xC0 | cp >> 6);
			out += (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += (char)(0xE0 | cp >> 12);
			out += (char)(0x80 | ((cp >> 6) & 0x3F));
			out += (char)(0x80 | (cp & 0x3F));
		} else {
			out += (char)(0xF0 | cp >> 18);
			out += (char)(0x80 | ((cp >> 12) & 0x3F));
			out += (char)(0x80 | ((cp >> 6) & 0x3F));
			out += (char)(0x80 | (cp & 0x3F));
		}
	}
	return out;
}

std::u16string utf16(const std::string &s) {
	std::u16string out;
	for (size_t i = 0; i < s.size();) {
		unsigned char c0 = s[i];
		uint32_t cp;
		int n;
		if (c0 < 0x80) { cp = c0; n = 1; }
		else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; n = 2; }
		else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; n = 3; }
		else { cp = c0 & 0x07; n = 4; }
		for (int k = 1; k < n && i + k < s.size(); k++)
			cp = cp << 6 | (s[i + k] & 0x3F);
		i += n;
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out += (char16_t)(0xD800 + (cp >> 10));
			out += (char16_t)(0xDC00 + (cp & 0x3FF));
		} else {
			out += (char16_t)cp;
		}
	}
	return out;
}

std::string utf8(gaddr str) {
	Obj *o = get(str);
	return o && o->type == kString ? utf8(o->str) : std::string();
}

std::string macRomanToUtf8(const std::string &s) { return utf8(fromMacRoman(s)); }
std::string utf8ToMacRoman(const std::string &s) { return toMacRoman(utf16(s)); }

void retain(gaddr ref) {
	Obj *o = get(ref);
	if (o)
		o->refs++;
}

void release(gaddr ref) {
	Obj *o = get(ref);
	if (!o || --o->refs > 0)
		return;
	if (o->cfCallbacks) {
		for (gaddr it : o->items)
			release(it);
		for (auto &p : o->pairs) {
			release(p.first);
			release(p.second);
		}
	}
	objects.erase(ref);
	heap::free(ref);
	delete o;
}

bool equal(gaddr a, gaddr b) {
	if (a == b)
		return true;
	Obj *x = get(a), *y = get(b);
	if (!x || !y || x->type != y->type)
		return false;
	switch (x->type) {
	case kString: return x->str == y->str;
	case kNumber: return x->isFloat || y->isFloat ? x->dval == y->dval : x->ival == y->ival;
	case kURL: return x->path == y->path;
	case kUUID: return memcmp(x->uuid, y->uuid, 16) == 0;
	case kArray:
		if (x->items.size() != y->items.size())
			return false;
		for (size_t i = 0; i < x->items.size(); i++) {
			if (!equal(x->items[i], y->items[i]))
				return false;
		}
		return true;
	default: return false;
	}
}

std::string describe(gaddr ref) {
	Obj *o = get(ref);
	if (!o)
		return "(null)";
	switch (o->type) {
	case kString: return utf8(o->str);
	case kNumber: return o->isFloat ? std::to_string(o->dval) : std::to_string(o->ival);
	case kURL: return o->path;
	default: return "<CF object>";
	}
}

} // namespace cf

using namespace cf;

namespace {

// Keys compare by value with CF callbacks, by pointer without
bool keyMatch(Obj *d, gaddr a, gaddr b) {
	return d->cfCallbacks ? cf::equal(a, b) : a == b;
}

Obj *expect(gaddr ref, Type t, const char *fn) {
	Obj *o = get(ref);
	if (!o || o->type != t)
		emu::fatal("%s: 0x%08x is not the right CF type", fn, ref);
	return o;
}

std::u16string formatString(gaddr fmtStr, gaddr args) {
	return utf16(guestFormat(cf::utf8(fmtStr), args));
}

} // namespace

// ---------------------------------------------------------------------------
// Base

IMPORT_DATA(kCFAllocatorDefault) { return emu::sysAlloc(4); }
IMPORT_DATA(__CFConstantStringClassReference) { return emu::sysAlloc(16); }
IMPORT_DATA(kCFTypeArrayCallBacks) { return emu::sysAlloc(20); }
IMPORT_DATA(kCFTypeDictionaryKeyCallBacks) { return emu::sysAlloc(24); }
IMPORT_DATA(kCFTypeDictionaryValueCallBacks) { return emu::sysAlloc(20); }

IMPORT(CFRetain) { cf::retain(c.arg(0)); c.ret(c.arg(0)); }
IMPORT(CFRelease) { cf::release(c.arg(0)); }
IMPORT(CFEqual) { c.ret(cf::equal(c.arg(0), c.arg(1))); }

// ---------------------------------------------------------------------------
// Strings

IMPORT(CFStringGetSystemEncoding) { c.ret(0); }
IMPORT(CFStringCreateWithCString) {
	c.ret(makeString(decode(emu::cstr(c.arg(1)), c.arg(2))));
}
IMPORT(CFStringCreateWithPascalString) {
	c.ret(makeString(decode(emu::pstr(c.arg(1)), c.arg(2))));
}
IMPORT(CFStringCreateWithBytes) {
	std::string bytes((const char *)emu::ptr(c.arg(1)), c.arg(2));
	uint32_t enc = c.arg(3);
	if (enc == kEncUnicode) {
		std::u16string u(bytes.size() / 2, 0);
		memcpy(&u[0], bytes.data(), u.size() * 2);
		c.ret(makeString(u));
	} else {
		c.ret(makeString(decode(bytes, enc)));
	}
}
IMPORT(CFStringCreateWithCharacters) {
	std::u16string u(c.arg(2), 0);
	if (!u.empty())
		memcpy(&u[0], emu::ptr(c.arg(1)), u.size() * 2);
	c.ret(makeString(u));
}
IMPORT(CFStringCreateCopy) {
	c.ret(makeString(expect(c.arg(1), kString, "CFStringCreateCopy")->str));
}
IMPORT(CFStringCreateMutableCopy) {
	c.ret(makeString(expect(c.arg(2), kString, "CFStringCreateMutableCopy")->str));
}
IMPORT(CFStringCreateWithFormat) {
	// (alloc, formatOptions, format, ...)
	c.ret(makeString(formatString(c.arg(2), c.esp + 16)));
}
IMPORT(CFStringAppendFormat) {
	// (mutable string, formatOptions, format, ...)
	Obj *s = expect(c.arg(0), kString, "CFStringAppendFormat");
	s->str += formatString(c.arg(2), c.esp + 16);
}
IMPORT(CFStringGetLength) { c.ret((uint32_t)expect(c.arg(0), kString, "CFStringGetLength")->str.size()); }
IMPORT(CFStringGetCString) {
	Obj *s = get(c.arg(0));
	if (!s || s->type != kString) {
		c.ret(0);
		return;
	}
	std::string out = encode(s->str, c.arg(3));
	uint32_t room = c.arg(2);
	if (out.size() + 1 > room) {
		c.ret(0);
		return;
	}
	memcpy(emu::ptr(c.arg(1)), out.c_str(), out.size() + 1);
	c.ret(1);
}
IMPORT(CFStringGetPascalString) {
	Obj *s = get(c.arg(0));
	if (!s || s->type != kString) {
		c.ret(0);
		return;
	}
	std::string out = encode(s->str, c.arg(3));
	if (out.size() > 255 || out.size() + 1 > c.arg(2)) {
		c.ret(0);
		return;
	}
	emu::wpstr(c.arg(1), out);
	c.ret(1);
}
IMPORT(CFStringGetPascalStringPtr) { c.ret(0); }
IMPORT(CFStringCompare) {
	std::u16string a = expect(c.arg(0), kString, "CFStringCompare")->str;
	std::u16string b = expect(c.arg(1), kString, "CFStringCompare")->str;
	if (c.arg(2) & 1) { // kCFCompareCaseInsensitive
		for (auto &ch : a) if (ch < 128) ch = (char16_t)tolower(ch);
		for (auto &ch : b) if (ch < 128) ch = (char16_t)tolower(ch);
	}
	int r = a.compare(b);
	c.ret(r < 0 ? (uint32_t)-1 : r > 0 ? 1 : 0);
}
IMPORT(CFStringCreateArrayBySeparatingStrings) {
	std::u16string s = expect(c.arg(1), kString, "CFStringCreateArrayBySeparatingStrings")->str;
	std::u16string sep = expect(c.arg(2), kString, "CFStringCreateArrayBySeparatingStrings")->str;
	Obj *a = new Obj{kArray};
	size_t start = 0;
	for (;;) {
		size_t pos = sep.empty() ? std::u16string::npos : s.find(sep, start);
		a->items.push_back(makeString(s.substr(start, pos == std::u16string::npos ? std::u16string::npos : pos - start)));
		if (pos == std::u16string::npos)
			break;
		start = pos + sep.size();
	}
	c.ret(make(a));
}

// ---------------------------------------------------------------------------
// Numbers

namespace {

// CFNumberType sizes and kinds
int numberSize(uint32_t type, bool *isFloat) {
	*isFloat = false;
	switch (type) {
	case 1: case 7: return 1;
	case 2: case 8: return 2;
	case 3: case 9: case 10: case 14: return 4;
	case 4: case 11: return 8;
	case 5: case 12: *isFloat = true; return 4;
	case 6: case 13: *isFloat = true; return 8;
	}
	return 0;
}

} // namespace

IMPORT(CFNumberCreate) {
	bool isFloat;
	int size = numberSize(c.arg(1), &isFloat);
	gaddr p = c.arg(2);
	Obj *o = new Obj{kNumber};
	o->isFloat = isFloat;
	if (isFloat) {
		o->dval = size == 4 ? emu::rfloat(p) : emu::rdouble(p);
		o->ival = (int64_t)o->dval;
	} else {
		switch (size) {
		case 1: o->ival = (int8_t)emu::r8(p); break;
		case 2: o->ival = (int16_t)emu::r16(p); break;
		case 4: o->ival = (int32_t)emu::r32(p); break;
		default: o->ival = (int64_t)(emu::r32(p) | (uint64_t)emu::r32(p + 4) << 32); break;
		}
		o->dval = (double)o->ival;
	}
	c.ret(make(o));
}
IMPORT(CFNumberGetValue) {
	Obj *o = expect(c.arg(0), kNumber, "CFNumberGetValue");
	bool isFloat;
	int size = numberSize(c.arg(1), &isFloat);
	gaddr p = c.arg(2);
	if (isFloat) {
		if (size == 4)
			emu::wfloat(p, (float)o->dval);
		else
			emu::wdouble(p, o->dval);
	} else {
		int64_t v = o->isFloat ? (int64_t)o->dval : o->ival;
		switch (size) {
		case 1: emu::w8(p, (uint8_t)v); break;
		case 2: emu::w16(p, (uint16_t)v); break;
		case 4: emu::w32(p, (uint32_t)v); break;
		default: emu::w32(p, (uint32_t)v); emu::w32(p + 4, (uint32_t)((uint64_t)v >> 32)); break;
		}
	}
	c.ret(1);
}

// ---------------------------------------------------------------------------
// Arrays

IMPORT(CFArrayCreateMutable) {
	Obj *a = new Obj{kArray};
	a->cfCallbacks = c.arg(2) != 0;
	c.ret(make(a));
}
IMPORT(CFArrayAppendValue) {
	Obj *a = expect(c.arg(0), kArray, "CFArrayAppendValue");
	if (a->cfCallbacks)
		cf::retain(c.arg(1));
	a->items.push_back(c.arg(1));
}
IMPORT(CFArrayGetCount) {
	Obj *a = get(c.arg(0));
	c.ret(a && a->type == kArray ? (uint32_t)a->items.size() : 0);
}
IMPORT(CFArrayGetValueAtIndex) {
	Obj *a = expect(c.arg(0), kArray, "CFArrayGetValueAtIndex");
	uint32_t i = c.arg(1);
	c.ret(i < a->items.size() ? a->items[i] : 0);
}
IMPORT(CFArrayGetFirstIndexOfValue) {
	// (array, CFRange by value, value)
	Obj *a = expect(c.arg(0), kArray, "CFArrayGetFirstIndexOfValue");
	uint32_t loc = c.arg(1), len = c.arg(2);
	gaddr v = c.arg(3);
	for (uint32_t i = loc; i < loc + len && i < a->items.size(); i++) {
		if (keyMatch(a, a->items[i], v)) {
			c.ret(i);
			return;
		}
	}
	c.ret((uint32_t)-1);
}
IMPORT(CFArraySortValues) {
	// (array, CFRange by value, comparator, context)
	Obj *a = expect(c.arg(0), kArray, "CFArraySortValues");
	uint32_t loc = c.arg(1), len = c.arg(2);
	gaddr cmp = c.arg(3), ctx = c.arg(4);
	if (loc + len > a->items.size())
		len = (uint32_t)a->items.size() - loc;
	std::stable_sort(a->items.begin() + loc, a->items.begin() + loc + len, [&](gaddr x, gaddr y) {
		return (int32_t)emu::call(cmp, {x, y, ctx}) < 0;
	});
}

// ---------------------------------------------------------------------------
// Dictionaries

IMPORT(CFDictionaryCreate) {
	// (alloc, keys, values, count, keyCallBacks, valueCallBacks)
	Obj *d = new Obj{kDictionary};
	d->cfCallbacks = c.arg(4) != 0;
	for (uint32_t i = 0; i < c.arg(3); i++) {
		gaddr k = emu::r32(c.arg(1) + 4 * i), v = emu::r32(c.arg(2) + 4 * i);
		if (d->cfCallbacks) {
			cf::retain(k);
			cf::retain(v);
		}
		d->pairs.push_back({k, v});
	}
	c.ret(make(d));
}
IMPORT(CFDictionaryCreateMutable) {
	Obj *d = new Obj{kDictionary};
	d->cfCallbacks = c.arg(2) != 0;
	c.ret(make(d));
}
IMPORT(CFDictionaryCreateMutableCopy) {
	Obj *src = expect(c.arg(2), kDictionary, "CFDictionaryCreateMutableCopy");
	Obj *d = new Obj{kDictionary};
	d->cfCallbacks = src->cfCallbacks;
	d->pairs = src->pairs;
	if (d->cfCallbacks) {
		for (auto &p : d->pairs) {
			cf::retain(p.first);
			cf::retain(p.second);
		}
	}
	c.ret(make(d));
}
IMPORT(CFDictionaryGetCount) {
	Obj *d = get(c.arg(0));
	c.ret(d && d->type == kDictionary ? (uint32_t)d->pairs.size() : 0);
}
IMPORT(CFDictionaryGetValue) {
	Obj *d = get(c.arg(0));
	if (d && d->type == kDictionary) {
		for (auto &p : d->pairs) {
			if (keyMatch(d, p.first, c.arg(1))) {
				c.ret(p.second);
				return;
			}
		}
	}
	c.ret(0);
}
IMPORT(CFDictionaryContainsKey) {
	Obj *d = expect(c.arg(0), kDictionary, "CFDictionaryContainsKey");
	for (auto &p : d->pairs) {
		if (keyMatch(d, p.first, c.arg(1))) {
			c.ret(1);
			return;
		}
	}
	c.ret(0);
}
IMPORT(CFDictionaryContainsValue) {
	Obj *d = expect(c.arg(0), kDictionary, "CFDictionaryContainsValue");
	for (auto &p : d->pairs) {
		if (keyMatch(d, p.second, c.arg(1))) {
			c.ret(1);
			return;
		}
	}
	c.ret(0);
}
IMPORT(CFDictionaryGetKeysAndValues) {
	Obj *d = expect(c.arg(0), kDictionary, "CFDictionaryGetKeysAndValues");
	for (size_t i = 0; i < d->pairs.size(); i++) {
		if (c.arg(1))
			emu::w32(c.arg(1) + 4 * (uint32_t)i, d->pairs[i].first);
		if (c.arg(2))
			emu::w32(c.arg(2) + 4 * (uint32_t)i, d->pairs[i].second);
	}
}
IMPORT(CFDictionarySetValue) {
	Obj *d = expect(c.arg(0), kDictionary, "CFDictionarySetValue");
	gaddr k = c.arg(1), v = c.arg(2);
	if (d->cfCallbacks)
		cf::retain(v);
	for (auto &p : d->pairs) {
		if (keyMatch(d, p.first, k)) {
			if (d->cfCallbacks)
				cf::release(p.second);
			p.second = v;
			return;
		}
	}
	if (d->cfCallbacks)
		cf::retain(k);
	d->pairs.push_back({k, v});
}
IMPORT(CFDictionaryRemoveValue) {
	Obj *d = expect(c.arg(0), kDictionary, "CFDictionaryRemoveValue");
	for (size_t i = 0; i < d->pairs.size(); i++) {
		if (keyMatch(d, d->pairs[i].first, c.arg(1))) {
			if (d->cfCallbacks) {
				cf::release(d->pairs[i].first);
				cf::release(d->pairs[i].second);
			}
			d->pairs.erase(d->pairs.begin() + i);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// URLs and bundles

IMPORT(CFBundleGetMainBundle) {
	static gaddr bundle = 0;
	if (!bundle) {
		Obj *o = new Obj{kBundle};
		o->refs = 1 << 30;
		o->path = host::gameApp;
		bundle = make(o);
	}
	c.ret(bundle);
}
IMPORT(CFBundleCopyBundleURL) { c.ret(makeURL(expect(c.arg(0), kBundle, "CFBundleCopyBundleURL")->path)); }
IMPORT(CFBundleCopyResourcesDirectoryURL) {
	c.ret(makeURL(expect(c.arg(0), kBundle, "CFBundleCopyResourcesDirectoryURL")->path + "/Contents/Resources"));
}
IMPORT(CFBundleCopyResourceURL) {
	// (bundle, name, type, subDir)
	std::string res = expect(c.arg(0), kBundle, "CFBundleCopyResourceURL")->path + "/Contents/Resources";
	std::string name = cf::utf8(c.arg(1));
	if (c.arg(2) && !cf::utf8(c.arg(2)).empty())
		name += "." + cf::utf8(c.arg(2));
	std::string sub = c.arg(3) ? cf::utf8(c.arg(3)) + "/" : "";
	for (const std::string &dir : {res + "/" + sub, res + "/English.lproj/" + sub}) {
		if (fileExists(dir + name)) {
			c.ret(makeURL(dir + name));
			return;
		}
	}
	logf("CFBundleCopyResourceURL: no resource %s", name.c_str());
	c.ret(0);
}
IMPORT(CFBundleGetValueForInfoDictionaryKey) {
	static CFDictionaryRef info = nullptr;
	if (!info)
		info = (CFDictionaryRef)readPlist(expect(c.arg(0), kBundle, "CFBundleGetValueForInfoDictionaryKey")->path + "/Contents/Info.plist");
	std::string key = cf::utf8(c.arg(1));
	CFStringRef k = CFStringCreateWithCString(nullptr, key.c_str(), kCFStringEncodingUTF8);
	CFTypeRef v = info ? CFDictionaryGetValue(info, k) : nullptr;
	CFRelease(k);
	gaddr r = fromHost(v);
	if (Obj *o = get(r))
		o->refs = 1 << 30; // "Get" rule: the bundle keeps it
	c.ret(r);
}
IMPORT_DATA(kCFBundleExecutableKey) {
	gaddr var = emu::sysAlloc(4);
	gaddr s = makeString(std::string("CFBundleExecutable"));
	get(s)->refs = 1 << 30;
	emu::w32(var, s);
	return var;
}
IMPORT(CFBundleCopyLocalizedString) {
	// (bundle, key, value, table)
	std::string base = expect(c.arg(0), kBundle, "CFBundleCopyLocalizedString")->path + "/Contents/Resources/English.lproj/";
	std::string table = c.arg(3) ? cf::utf8(c.arg(3)) : "Localizable";
	static std::map<std::string, CFDictionaryRef> tables;
	if (!tables.count(table))
		tables[table] = (CFDictionaryRef)readPlist(base + table + ".strings");
	CFDictionaryRef dict = tables[table];
	std::string key = cf::utf8(c.arg(1));
	CFStringRef k = CFStringCreateWithCString(nullptr, key.c_str(), kCFStringEncodingUTF8);
	CFTypeRef v = dict ? CFDictionaryGetValue(dict, k) : nullptr;
	CFRelease(k);
	if (v) {
		c.ret(fromHost(v));
	} else {
		gaddr fallback = c.arg(2) ? c.arg(2) : c.arg(1);
		c.ret(makeString(get(fallback) ? get(fallback)->str : std::u16string()));
	}
}

IMPORT(CFURLCreateWithString) {
	std::string s = cf::utf8(c.arg(1));
	if (s.rfind("file://", 0) == 0)
		s = s.substr(7);
	c.ret(makeURL(s));
}
IMPORT(CFURLCreateCopyAppendingPathComponent) {
	// (alloc, url, component, isDirectory)
	std::string p = expect(c.arg(1), kURL, "CFURLCreateCopyAppendingPathComponent")->path;
	if (p.empty() || p.back() != '/')
		p += "/";
	c.ret(makeURL(p + cf::utf8(c.arg(2))));
}
IMPORT(CFURLCreateCopyDeletingLastPathComponent) {
	// (alloc, url)
	std::string p = expect(c.arg(1), kURL, "CFURLCreateCopyDeletingLastPathComponent")->path;
	while (p.size() > 1 && p.back() == '/')
		p.pop_back();
	size_t slash = p.find_last_of('/');
	p = slash == std::string::npos || slash == 0 ? "/" : p.substr(0, slash + 1);
	c.ret(makeURL(p));
}
IMPORT(CFURLGetFileSystemRepresentation) {
	// (url, resolveAgainstBase, buffer, maxLen)
	std::string p = expect(c.arg(0), kURL, "CFURLGetFileSystemRepresentation")->path;
	if (p.size() + 1 > c.arg(3)) {
		c.ret(0);
		return;
	}
	memcpy(emu::ptr(c.arg(2)), p.c_str(), p.size() + 1);
	c.ret(1);
}
IMPORT(CFURLGetFSRef) {
	std::string p = expect(c.arg(0), kURL, "CFURLGetFSRef")->path;
	if (!fileExists(p)) {
		c.ret(0);
		return;
	}
	files::writeFSRef(c.arg(1), p);
	c.ret(1);
}

// ---------------------------------------------------------------------------
// Preferences, stored in the support folder

namespace {

CFMutableDictionaryRef prefs() {
	static CFMutableDictionaryRef d = nullptr;
	if (!d) {
		CFPropertyListRef p = readPlist(host::supportDir + "/Preferences.plist");
		if (p && CFGetTypeID(p) == CFDictionaryGetTypeID())
			d = CFDictionaryCreateMutableCopy(nullptr, 0, (CFDictionaryRef)p);
		else
			d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		if (p)
			CFRelease(p);
	}
	return d;
}

} // namespace

IMPORT_DATA(kCFPreferencesCurrentApplication) {
	gaddr var = emu::sysAlloc(4);
	gaddr s = makeString(std::string("kCFPreferencesCurrentApplication"));
	get(s)->refs = 1 << 30;
	emu::w32(var, s);
	return var;
}
IMPORT(CFPreferencesCopyAppValue) {
	CFTypeRef k = toHost(c.arg(0));
	CFTypeRef v = k ? CFDictionaryGetValue(prefs(), k) : nullptr;
	if (k)
		CFRelease(k);
	c.ret(fromHost(v));
}
IMPORT(CFPreferencesSetAppValue) {
	CFTypeRef k = toHost(c.arg(0));
	if (!k)
		return;
	CFTypeRef v = c.arg(1) ? toHost(c.arg(1)) : nullptr;
	if (v) {
		CFDictionarySetValue(prefs(), k, v);
		CFRelease(v);
	} else {
		CFDictionaryRemoveValue(prefs(), k);
	}
	CFRelease(k);
}
IMPORT(CFPreferencesAppSynchronize) {
	CFDataRef data = CFPropertyListCreateData(nullptr, prefs(), kCFPropertyListXMLFormat_v1_0, 0, nullptr);
	if (data) {
		std::string path = host::supportDir + "/Preferences.plist";
		FILE *f = fopen(path.c_str(), "wb");
		if (f) {
			fwrite(CFDataGetBytePtr(data), 1, CFDataGetLength(data), f);
			fclose(f);
		}
		CFRelease(data);
	}
	c.ret(1);
}

// ---------------------------------------------------------------------------
// UUIDs

IMPORT(CFUUIDCreate) {
	Obj *o = new Obj{kUUID};
	CFUUIDRef u = CFUUIDCreate(nullptr);
	CFUUIDBytes b = CFUUIDGetUUIDBytes(u);
	memcpy(o->uuid, &b, 16);
	CFRelease(u);
	c.ret(make(o));
}
IMPORT(CFUUIDCreateFromString) {
	Obj *o = new Obj{kUUID};
	std::string s = cf::utf8(c.arg(1));
	CFStringRef hs = CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8);
	CFUUIDRef u = CFUUIDCreateFromString(nullptr, hs);
	if (u) {
		CFUUIDBytes b = CFUUIDGetUUIDBytes(u);
		memcpy(o->uuid, &b, 16);
		CFRelease(u);
	}
	CFRelease(hs);
	c.ret(make(o));
}
IMPORT(CFUUIDCreateString) {
	Obj *o = expect(c.arg(1), kUUID, "CFUUIDCreateString");
	CFUUIDBytes b;
	memcpy(&b, o->uuid, 16);
	CFUUIDRef u = CFUUIDCreateFromUUIDBytes(nullptr, b);
	CFStringRef s = CFUUIDCreateString(nullptr, u);
	char buf[64];
	CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
	CFRelease(s);
	CFRelease(u);
	c.ret(makeString(std::string(buf)));
}

// ---------------------------------------------------------------------------
// More strings, arrays, bundles and URLs. CFRange arguments are passed by
// value (two words); a CFRange result comes back in EDX:EAX.

namespace {

std::u16string &mutableStr(gaddr ref, const char *fn) { return expect(ref, kString, fn)->str; }

std::u16string folded(std::u16string s, uint32_t options) {
	if (options & 1) { // kCFCompareCaseInsensitive
		for (auto &ch : s)
			if (ch < 128)
				ch = (char16_t)tolower(ch);
	}
	return s;
}

std::string hfsPath(const std::string &posix) {
	std::string s = "Macintosh HD";
	for (char ch : posix)
		s += ch == '/' ? ':' : ch == ':' ? '/' : ch;
	return s;
}

} // namespace

IMPORT_DATA(kCFAllocatorNull) { return emu::sysAlloc(4); }

IMPORT(CFStringCreateMutable) { c.ret(makeString(std::u16string())); }
IMPORT(CFStringCreateWithCStringNoCopy) { c.ret(makeString(decode(emu::cstr(c.arg(1)), c.arg(2)))); }
IMPORT(CFStringCreateWithSubstring) {
	// (alloc, string, range)
	std::u16string s = expect(c.arg(1), kString, "CFStringCreateWithSubstring")->str;
	uint32_t loc = c.arg(2), len = c.arg(3);
	c.ret(makeString(loc < s.size() ? s.substr(loc, len) : std::u16string()));
}
IMPORT(CFStringAppendCString) {
	mutableStr(c.arg(0), "CFStringAppendCString") += decode(emu::cstr(c.arg(1)), c.arg(2));
}
IMPORT(CFStringGetMaximumSizeForEncoding) { c.ret(c.arg(0) * 3); }
IMPORT(CFStringHasSuffix) {
	std::u16string s = expect(c.arg(0), kString, "CFStringHasSuffix")->str;
	std::u16string x = expect(c.arg(1), kString, "CFStringHasSuffix")->str;
	c.ret(s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0);
}
IMPORT(CFStringCompareWithOptions) {
	// (a, b, range in a, options)
	std::u16string a = expect(c.arg(0), kString, "CFStringCompareWithOptions")->str;
	std::u16string b = expect(c.arg(1), kString, "CFStringCompareWithOptions")->str;
	uint32_t loc = c.arg(2), len = c.arg(3);
	a = loc < a.size() ? a.substr(loc, len) : std::u16string();
	int r = folded(a, c.arg(4)).compare(folded(b, c.arg(4)));
	c.ret(r < 0 ? (uint32_t)-1 : r > 0 ? 1 : 0);
}
IMPORT(CFStringFind) {
	// (string, toFind, options) -> CFRange
	std::u16string s = folded(expect(c.arg(0), kString, "CFStringFind")->str, c.arg(2));
	std::u16string f = folded(expect(c.arg(1), kString, "CFStringFind")->str, c.arg(2));
	size_t pos = (c.arg(2) & 4) ? s.rfind(f) : s.find(f); // kCFCompareBackwards
	if (f.empty() || pos == std::u16string::npos)
		c.ret64((uint64_t)(uint32_t)-1);
	else
		c.ret64((uint32_t)pos | ((uint64_t)f.size() << 32));
}
IMPORT(CFStringFindWithOptions) {
	// (string, toFind, CFRange rangeToSearch, options, CFRange *result) -> Boolean
	std::u16string s = folded(expect(c.arg(0), kString, "CFStringFindWithOptions")->str, c.arg(4));
	std::u16string f = folded(expect(c.arg(1), kString, "CFStringFindWithOptions")->str, c.arg(4));
	size_t loc = std::min<size_t>(c.arg(2), s.size()), len = std::min<size_t>(c.arg(3), s.size() - loc);
	std::u16string r = s.substr(loc, len);
	size_t pos = (c.arg(4) & 4) ? r.rfind(f) : r.find(f); // kCFCompareBackwards
	if ((c.arg(4) & 8) && pos != 0) // kCFCompareAnchored
		pos = std::u16string::npos;
	bool found = !f.empty() && pos != std::u16string::npos;
	if (found && c.arg(5)) {
		emu::w32(c.arg(5), (uint32_t)(loc + pos));
		emu::w32(c.arg(5) + 4, (uint32_t)f.size());
	}
	c.ret(found);
}
IMPORT(CFStringReplace) {
	// (mutable string, range, replacement)
	std::u16string &s = mutableStr(c.arg(0), "CFStringReplace");
	uint32_t loc = c.arg(1), len = c.arg(2);
	if (loc <= s.size())
		s.replace(loc, len, expect(c.arg(3), kString, "CFStringReplace")->str);
}

IMPORT(CFArrayInsertValueAtIndex) {
	Obj *a = expect(c.arg(0), kArray, "CFArrayInsertValueAtIndex");
	uint32_t i = std::min<uint32_t>(c.arg(1), (uint32_t)a->items.size());
	if (a->cfCallbacks)
		cf::retain(c.arg(2));
	a->items.insert(a->items.begin() + i, c.arg(2));
}
IMPORT(CFArrayRemoveValueAtIndex) {
	Obj *a = expect(c.arg(0), kArray, "CFArrayRemoveValueAtIndex");
	uint32_t i = c.arg(1);
	if (i >= a->items.size())
		return;
	if (a->cfCallbacks)
		cf::release(a->items[i]);
	a->items.erase(a->items.begin() + i);
}
IMPORT(CFArraySetValueAtIndex) {
	Obj *a = expect(c.arg(0), kArray, "CFArraySetValueAtIndex");
	uint32_t i = c.arg(1);
	if (a->cfCallbacks)
		cf::retain(c.arg(2));
	if (i < a->items.size()) {
		if (a->cfCallbacks)
			cf::release(a->items[i]);
		a->items[i] = c.arg(2);
	} else {
		a->items.push_back(c.arg(2));
	}
}

IMPORT(CFBundleGetInfoDictionary) {
	static gaddr info = 0;
	if (!info) {
		CFTypeRef d = readPlist(expect(c.arg(0), kBundle, "CFBundleGetInfoDictionary")->path + "/Contents/Info.plist");
		info = fromHost(d);
		if (Obj *o = get(info))
			o->refs = 1 << 30;
	}
	c.ret(info);
}
IMPORT(CFBundleGetIdentifier) {
	static gaddr id = 0;
	if (!id) {
		CFDictionaryRef d = (CFDictionaryRef)readPlist(expect(c.arg(0), kBundle, "CFBundleGetIdentifier")->path + "/Contents/Info.plist");
		CFTypeRef v = d ? CFDictionaryGetValue(d, CFSTR("CFBundleIdentifier")) : nullptr;
		id = v ? fromHost(v) : makeString(std::string("com.mackiev.game"));
		get(id)->refs = 1 << 30;
	}
	c.ret(id);
}
IMPORT(CFBundleCopyExecutableURL) {
	c.ret(makeURL(expect(c.arg(0), kBundle, "CFBundleCopyExecutableURL")->path + "/Contents/MacOS/" + host::gameName));
}

IMPORT(CFURLCreateFromFSRef) {
	std::string p = files::pathOfFSRef(c.arg(1));
	c.ret(p.empty() ? 0 : makeURL(p));
}
IMPORT(CFURLCopyAbsoluteURL) { c.ret(makeURL(expect(c.arg(0), kURL, "CFURLCopyAbsoluteURL")->path)); }
IMPORT(CFURLCopyLastPathComponent) {
	std::string p = expect(c.arg(0), kURL, "CFURLCopyLastPathComponent")->path;
	while (p.size() > 1 && p.back() == '/')
		p.pop_back();
	size_t slash = p.rfind('/');
	c.ret(makeString(slash == std::string::npos || p.size() == 1 ? p : p.substr(slash + 1)));
}
IMPORT(CFURLHasDirectoryPath) {
	std::string p = expect(c.arg(0), kURL, "CFURLHasDirectoryPath")->path;
	struct stat st;
	c.ret(!p.empty() && (p.back() == '/' || (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))));
}
IMPORT(CFURLCopyFileSystemPath) {
	// (url, pathStyle): 0 POSIX, 1 HFS
	std::string p = expect(c.arg(0), kURL, "CFURLCopyFileSystemPath")->path;
	if (p.size() > 1 && p.back() == '/')
		p.pop_back();
	c.ret(makeString(c.arg(1) == 1 ? hfsPath(p) : p));
}
