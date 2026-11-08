#include <dlfcn.h>
// Runs the original i386 executable of a Carbon game on any Mac by
// emulating the CPU and reimplementing the system calls it makes.
#include "emu.h"
#include "macho.h"
#include "host.h"
#include "qd.h"
#include "res.h"
#include "sound.h"
#include "window.h"

#include <cstdio>
#include <CoreFoundation/CoreFoundation.h>
#include <climits>
#include <dirent.h>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

MachO gImage;

namespace {

// Reports where a host crash happened (TS_LOG only); usually generated code
void onCrash(int sig, siginfo_t *info, void *ctx) {
	ucontext_t *uc = (ucontext_t *)ctx;
	char buf[2048];
	int n;
#if defined(__x86_64__)
	auto &ss = uc->uc_mcontext->__ss;
	n = snprintf(buf, sizeof buf, "crash: signal %d at pc 0x%llx, address %p\n"
		"rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
		sig, ss.__rip, info->si_addr, ss.__rax, ss.__rbx, ss.__rcx, ss.__rdx, ss.__rsi, ss.__rdi, ss.__rbp,
		ss.__rsp, ss.__r12, ss.__r13, ss.__r14, ss.__r15);
	const uint8_t *p = (const uint8_t *)ss.__rip;
	for (int i = -16; i < 16; i++)
		n += snprintf(buf + n, sizeof buf - n, i == 0 ? "[%02x] " : "%02x ", p[i]);
	buf[n++] = '\n';
#else
	auto &ss = uc->uc_mcontext->__ss;
	Dl_info di = {};
	dladdr((void *)ss.__lr, &di);
	n = snprintf(buf, sizeof buf, "crash: signal %d at pc 0x%llx, address %p, lr 0x%llx (%s+0x%lx), fp 0x%llx\n", sig,
		ss.__pc, info->si_addr, ss.__lr, di.dli_sname ? di.dli_sname : "?",
		di.dli_saddr ? (long)(ss.__lr - (uint64_t)di.dli_saddr) : 0, ss.__fp);
	// Return addresses up the frame chain
	const uint64_t *fp = (const uint64_t *)ss.__fp;
	for (int i = 0; i < 12 && fp; i++, fp = (const uint64_t *)fp[0]) {
		Dl_info d = {};
		dladdr((void *)fp[1], &d);
		n += snprintf(buf + n, sizeof buf - n, "  %s\n", d.dli_sname ? d.dli_sname : "?");
	}
#endif
	write(2, buf, n);
	signal(sig, SIG_DFL);
}

// A string from an app's Info.plist
std::string plistString(const std::string &app, CFStringRef key) {
	std::string name;
	std::string path = app + "/Contents/Info.plist";
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), path.size(), false);
	CFReadStreamRef in = CFReadStreamCreateWithFile(nullptr, url);
	CFRelease(url);
	if (in && CFReadStreamOpen(in)) {
		CFPropertyListRef plist = CFPropertyListCreateWithStream(nullptr, in, 0, kCFPropertyListImmutable, nullptr, nullptr);
		if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
			CFStringRef s = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)plist, key);
			char buf[256];
			if (s && CFGetTypeID(s) == CFStringGetTypeID() && CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8))
				name = buf;
		}
		if (plist)
			CFRelease(plist);
		CFReadStreamClose(in);
	}
	if (in)
		CFRelease(in);
	return name;
}

} // namespace

int main(int argc, char **argv) {
	// Inside the app bundle the original game is the one folder in
	// Resources/Game; Finder may pass a -psn_ argument, which is not a path
	if (argc >= 2 && strncmp(argv[1], "-psn", 4) != 0) {
		host::gameApp = argv[1];
	} else {
		char buf[PATH_MAX], real[PATH_MAX];
		uint32_t size = sizeof buf;
		if (_NSGetExecutablePath(buf, &size) != 0 || !realpath(buf, real)) {
			fprintf(stderr, "usage: %s <path to the game's .app>\n", argv[0]);
			return 1;
		}
		std::string contents = real; // .../Contents/MacOS/cfrun
		contents.erase(contents.rfind('/'));
		contents.erase(contents.rfind('/'));
		host::wrapperApp = contents.substr(0, contents.rfind('/'));
		// A companion app (the Assessment) shares its game's support folder
		host::supportName = plistString(host::wrapperApp, CFSTR("TSSupportName"));
		std::string dir = contents + "/Resources/Game";
		if (DIR *d = opendir(dir.c_str())) {
			while (dirent *e = readdir(d)) {
				if (e->d_name[0] != '.') {
					host::gameApp = dir + "/" + e->d_name;
					break;
				}
			}
			closedir(d);
		}
	}
	while (!host::gameApp.empty() && host::gameApp.back() == '/')
		host::gameApp.pop_back();
	host::gameName = plistString(host::gameApp, CFSTR("CFBundleExecutable"));
	if (host::supportName.empty())
		host::supportName = host::gameName;
	std::string exe = host::gameApp + "/Contents/MacOS/" + host::gameName;
	struct stat st;
	if (::stat(exe.c_str(), &st) != 0) {
		fprintf(stderr, "no game executable at %s\n", exe.c_str());
		return 1;
	}

	if (getenv("TS_LOG")) {
		struct sigaction sa = {};
		sa.sa_sigaction = onCrash;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
	}
	emu::init();
	imports::installHooks();
	if (!gImage.load(exe))
		emu::fatal("could not load %s", exe.c_str());
	host::init();
	qd::init();
	ev::init();
	res::init();
	snd::init();

	// Initial stack for crt1's start: argc, argv[], NULL, envp[], NULL, apple[], NULL
	gaddr exePath = emu::sysString(exe);
	uint32_t esp = kStackBase + kStackSize - 0x1000;
	esp -= 7 * 4;
	esp &= ~15u;
	uint32_t init[] = {1, exePath, 0, 0, exePath, 0, 0};
	for (int i = 0; i < 7; i++)
		emu::w32(esp + 4 * i, init[i]);
	emu::setReg(UC_X86_REG_ESP, esp);

	uc_err err = uc_emu_start(emu::uc, gImage.entry, 0, 0, 0);
	if (err != UC_ERR_OK)
		emu::fatal("emulation stopped: %s", uc_strerror(err));
	return 0;
}
