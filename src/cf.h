// CoreFoundation objects as seen by the guest
#pragma once

#include "emu.h"

#include <string>
#include <utility>
#include <vector>

namespace cf {

enum Type { kString = 1, kNumber, kArray, kDictionary, kURL, kBundle, kUUID, kNotification, kOther };

struct Obj {
	Type type;
	int refs = 1;
	std::u16string str;               // kString
	bool isFloat = false;             // kNumber
	double dval = 0;
	int64_t ival = 0;
	std::vector<gaddr> items;         // kArray
	std::vector<std::pair<gaddr, gaddr>> pairs; // kDictionary
	bool cfCallbacks = true;          // items are CF objects (retain, CFEqual)
	std::string path;                 // kURL, kBundle (POSIX path)
	uint8_t uuid[16] = {};            // kUUID
};

// The object behind a guest CFTypeRef (constant strings included), or null
Obj *get(gaddr ref);
gaddr make(Obj *o);
gaddr makeString(const std::u16string &s);
gaddr makeString(const std::string &utf8);
gaddr makeURL(const std::string &path);
std::string utf8(gaddr str);
std::string utf8(const std::u16string &s);
std::u16string utf16(const std::string &utf8);
std::string macRomanToUtf8(const std::string &s);
std::string utf8ToMacRoman(const std::string &s);
void retain(gaddr ref);
void release(gaddr ref);
bool equal(gaddr a, gaddr b);
std::string describe(gaddr ref);

} // namespace cf
