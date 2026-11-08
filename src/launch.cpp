// Launching other apps. When the Assessment finishes it opens its game, which
// on the host is the game's Universal 2 wrapper. Apple events (used to show
// help) and the Objective-C fallback do nothing.
#include "cf.h"
#include "emu.h"
#include "host.h"

#include <CoreServices/CoreServices.h>
#include <dirent.h>

namespace {

// The wrapper of the game whose original bundle ID is `id`: the one installed
// next to the running wrapper, or else any that Launch Services knows of
std::string findWrapper(const std::string &id) {
	std::string want = id;
	for (char &ch : want)
		ch = (char)tolower((unsigned char)ch);
	want += ".universal";
	std::string found;
	CFStringRef s = CFStringCreateWithCString(nullptr, want.c_str(), kCFStringEncodingUTF8);
	if (!host::wrapperApp.empty()) {
		std::string dir = host::wrapperApp.substr(0, host::wrapperApp.rfind('/'));
		if (DIR *d = opendir(dir.c_str())) {
			while (dirent *e = readdir(d)) {
				std::string app = dir + "/" + e->d_name;
				if (app.size() < 4 || app.compare(app.size() - 4, 4, ".app") != 0)
					continue;
				CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)app.c_str(), app.size(), true);
				if (CFBundleRef b = CFBundleCreate(nullptr, url)) {
					CFStringRef bid = CFBundleGetIdentifier(b);
					if (bid && CFStringCompare(bid, s, kCFCompareCaseInsensitive) == kCFCompareEqualTo)
						found = app;
					CFRelease(b);
				}
				CFRelease(url);
				if (!found.empty())
					break;
			}
			closedir(d);
		}
	}
	if (found.empty()) {
		if (CFArrayRef urls = LSCopyApplicationURLsForBundleIdentifier(s, nullptr)) {
			char buf[PATH_MAX];
			if (CFArrayGetCount(urls) > 0 &&
			    CFURLGetFileSystemRepresentation((CFURLRef)CFArrayGetValueAtIndex(urls, 0), true, (UInt8 *)buf, sizeof buf))
				found = buf;
			CFRelease(urls);
		}
	}
	CFRelease(s);
	return found;
}

} // namespace

IMPORT(LSFindApplicationForInfo) {
	// (creator, bundleID, name, FSRef *outAppRef, CFURLRef *outAppURL)
	std::string id = c.arg(1) ? cf::utf8(c.arg(1)) : std::string();
	std::string app = id.empty() ? std::string() : findWrapper(id);
	logf("LSFindApplicationForInfo %s -> %s", id.c_str(), app.empty() ? "(none)" : app.c_str());
	if (app.empty()) {
		c.ret((uint32_t)kLSApplicationNotFoundErr);
		return;
	}
	if (c.arg(4))
		emu::w32(c.arg(4), cf::makeURL(app));
	c.ret(0);
}
IMPORT(LSOpenCFURLRef) {
	// (CFURLRef, CFURLRef *outLaunchedURL)
	cf::Obj *o = cf::get(c.arg(0));
	if (!o || o->type != cf::kURL) {
		c.ret((uint32_t)paramErr);
		return;
	}
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)o->path.c_str(), o->path.size(), true);
	OSStatus err = LSOpenCFURLRef(url, nullptr);
	CFRelease(url);
	logf("LSOpenCFURLRef %s -> %d", o->path.c_str(), (int)err);
	if (c.arg(1))
		emu::w32(c.arg(1), 0);
	c.ret((uint32_t)err);
}
// The classic fallback only runs when Launch Services found nothing
IMPORT(LaunchApplication) { c.ret((uint32_t)(int32_t)-43); } // fnfErr

// No Cocoa in the guest, so the NSWorkspace fallback reports failure
IMPORT(NSApplicationLoad) { c.ret(0); }
IMPORT(objc_msgSend) { c.ret(0); }
static gaddr objcClass() {
	return emu::sysAlloc(16);
}
static DataReg dregPool(".objc_class_name_NSAutoreleasePool", objcClass);
static DataReg dregWorkspace(".objc_class_name_NSWorkspace", objcClass);

// Apple events go nowhere
IMPORT(AECreateDesc) {
	// (typeCode, dataPtr, dataSize, AEDesc *)
	emu::w32(c.arg(3), 0x6E756C6C); // typeNull
	emu::w32(c.arg(3) + 4, 0);
	c.ret(0);
}
IMPORT(AECreateAppleEvent) {
	// (class, id, target, returnID, transactionID, AppleEvent *)
	emu::w32(c.arg(5), 0x6E756C6C);
	emu::w32(c.arg(5) + 4, 0);
	c.ret(0);
}
IMPORT(AEPutParamPtr) { c.ret(0); }
IMPORT(AEDisposeDesc) { c.ret(0); }
IMPORT(AESend) { c.ret((uint32_t)(int32_t)procNotFound); }
