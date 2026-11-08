// libSystem, libstdc++, libgcc and dyld entry points the game imports
#include "emu.h"
#include "heap.h"
#include "macho.h"
#include "guestfmt.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// dyld

IMPORT(__dyld_func_lookup) {
	std::string name = emu::cstr(c.arg(0));
	gaddr result = 0;
	if (name == "__dyld_make_delayed_module_initializer_calls")
		result = imports::thunk("dyld_run_initializers");
	else
		logf("dyld_func_lookup(%s): not provided", name.c_str());
	emu::w32(c.arg(1), result);
	c.ret(result ? 1 : 0);
}

IMPORT(dyld_run_initializers) {
	for (gaddr fn : gImage.initFuncs)
		emu::call(fn, {});
}

IMPORT(__keymgr_dwarf2_register_sections) {}
IMPORT(_keymgr_get_and_lock_processwide_ptr) { c.ret(0); }
IMPORT(_keymgr_get_and_lock_processwide_ptr_2) { emu::w32(c.arg(1), 0); c.ret(0); }
IMPORT(_keymgr_set_and_unlock_processwide_ptr) { c.ret(0); }

IMPORT_DATA(mach_init_routine) { return emu::sysAlloc(4); }
IMPORT_DATA(_cthread_init_routine) { return emu::sysAlloc(4); }

// ---------------------------------------------------------------------------
// Process

IMPORT(exit) {
	logf("exit(%d)", c.sarg(0));
	fflush(stdout);
	exit(c.sarg(0));
}
IMPORT(abort) { emu::fatal("guest called abort()"); }
IMPORT(geteuid) { c.ret(geteuid()); }

gaddr errnoAddr() {
	static gaddr a = emu::sysAlloc(4);
	return a;
}
IMPORT(__error) { c.ret(errnoAddr()); }
IMPORT_DATA(errno) { return errnoAddr(); }

IMPORT(dlopen) { logf("dlopen(%s)", emu::cstr(c.arg(0)).c_str()); c.ret(0); }
IMPORT(dlsym) { logf("dlsym(%s)", emu::cstr(c.arg(1)).c_str()); c.ret(0); }

// Single-threaded: mutexes always succeed
IMPORT(pthread_mutex_init) { c.ret(0); }
IMPORT(pthread_mutex_destroy) { c.ret(0); }
IMPORT(pthread_mutex_lock) { c.ret(0); }
IMPORT(pthread_mutex_unlock) { c.ret(0); }
IMPORT(pthread_self) { c.ret(1); }
IMPORT(pthread_equal) { c.ret(c.arg(0) == c.arg(1)); }

// ---------------------------------------------------------------------------
// Strings and memory

IMPORT(memcpy) {
	if (c.arg(2))
		memmove(emu::ptr(c.arg(0)), emu::ptr(c.arg(1)), c.arg(2));
	c.ret(c.arg(0));
}
IMPORT(memmove) {
	if (c.arg(2))
		memmove(emu::ptr(c.arg(0)), emu::ptr(c.arg(1)), c.arg(2));
	c.ret(c.arg(0));
}
IMPORT(memset) {
	if (c.arg(2))
		memset(emu::ptr(c.arg(0)), (int)c.arg(1), c.arg(2));
	c.ret(c.arg(0));
}
IMPORT(strcpy) {
	std::string s = emu::cstr(c.arg(1));
	memcpy(emu::ptr(c.arg(0)), s.c_str(), s.size() + 1);
	c.ret(c.arg(0));
}
IMPORT(atoi) { c.ret((uint32_t)atoi(emu::cstr(c.arg(0)).c_str())); }

static uint32_t randState = 1;
IMPORT(srand) { randState = c.arg(0); }
IMPORT(rand) {
	// Same LCG as the Mac OS X libc rand()
	randState = randState * 1103515245 + 12345;
	c.ret((randState >> 16) & 0x7FFF);
}

// ---------------------------------------------------------------------------
// stdio: only stderr-style logging and sprintf are used

IMPORT_DATA(__sF) { return emu::sysAlloc(3 * 88); }

IMPORT(sprintf) {
	std::string s = guestFormat(emu::cstr(c.arg(1)), c.esp + 12);
	memcpy(emu::ptr(c.arg(0)), s.c_str(), s.size() + 1);
	c.ret((uint32_t)s.size());
}
IMPORT(fprintf) {
	std::string s = guestFormat(emu::cstr(c.arg(1)), c.esp + 12);
	logf("guest: %s", s.c_str());
	c.ret((uint32_t)s.size());
}
IMPORT(fflush) { c.ret(0); }

// ---------------------------------------------------------------------------
// Files

IMPORT(stat) {
	std::string path = emu::cstr(c.arg(0));
	struct stat st;
	if (::stat(path.c_str(), &st) != 0) {
		emu::w32(errnoAddr(), errno);
		c.ret((uint32_t)-1);
		return;
	}
	// struct stat of the i386 Mac OS X 10.4 ABI (96 bytes)
	gaddr b = c.arg(1);
	memset(emu::ptr(b), 0, 96);
	emu::w32(b + 0, (uint32_t)st.st_dev);
	emu::w32(b + 4, (uint32_t)st.st_ino);
	emu::w16(b + 8, st.st_mode);
	emu::w16(b + 10, (uint16_t)st.st_nlink);
	emu::w32(b + 12, st.st_uid);
	emu::w32(b + 16, st.st_gid);
	emu::w32(b + 24, (uint32_t)st.st_atimespec.tv_sec);
	emu::w32(b + 32, (uint32_t)st.st_mtimespec.tv_sec);
	emu::w32(b + 40, (uint32_t)st.st_ctimespec.tv_sec);
	emu::w32(b + 48, (uint32_t)st.st_size);
	emu::w32(b + 52, (uint32_t)((uint64_t)st.st_size >> 32));
	emu::w32(b + 56, (uint32_t)st.st_blocks);
	emu::w32(b + 64, (uint32_t)st.st_blksize);
	c.ret(0);
}

IMPORT(statfs) {
	// struct statfs (10.4, 32-bit): plenty of free space everywhere
	gaddr b = c.arg(1);
	memset(emu::ptr(b), 0, 272);
	emu::w32(b + 0, 0);           // f_otype/f_oflags
	emu::w32(b + 4, 4096);        // f_bsize
	emu::w32(b + 8, 4096);        // f_iosize
	emu::w32(b + 12, 0x1000000);  // f_blocks
	emu::w32(b + 16, 0x800000);   // f_bfree
	emu::w32(b + 20, 0x800000);   // f_bavail
	c.ret(0);
}

// ---------------------------------------------------------------------------
// Math. Results go back in ST(0) through the thunk.

IMPORT_F(sin) { c.retd(sin(c.argd(0))); }
IMPORT_F(cos) { c.retd(cos(c.argd(0))); }
IMPORT_F(atan) { c.retd(atan(c.argd(0))); }
IMPORT_F(atan2) { c.retd(atan2(c.argd(0), c.argd(2))); }
IMPORT_F(floor) { c.retd(floor(c.argd(0))); }
IMPORT_F(fmod) { c.retd(fmod(c.argd(0), c.argd(2))); }
IMPORT_F(sinf) { c.retd(sinf(c.argf(0))); }
IMPORT_F(cosf) { c.retd(cosf(c.argf(0))); }

// ---------------------------------------------------------------------------
// C++ runtime

IMPORT(__cxa_guard_acquire) { c.ret(emu::r8(c.arg(0)) == 0 ? 1 : 0); }
IMPORT(__cxa_guard_release) { emu::w8(c.arg(0), 1); }
IMPORT(__cxa_guard_abort) {}
IMPORT(__cxa_pure_virtual) { emu::fatal("pure virtual function called"); }
IMPORT(__cxa_begin_catch) { c.ret(c.arg(0)); }
IMPORT(__cxa_end_catch) {}
IMPORT(_Unwind_Resume) { emu::fatal("C++ exception unwinding is not supported"); }
IMPORT(_ZSt17__throw_bad_allocv) { emu::fatal("std::bad_alloc"); }
IMPORT(_ZSt20__throw_length_errorPKc) { emu::fatal("std::length_error: %s", emu::cstr(c.arg(0)).c_str()); }
IMPORT(_ZSt20__throw_out_of_rangePKc) { emu::fatal("std::out_of_range: %s", emu::cstr(c.arg(0)).c_str()); }
IMPORT(__gxx_personality_v0) { emu::fatal("__gxx_personality_v0 called"); }

// std::list node links: {next, prev}
IMPORT(_ZNSt15_List_node_base4hookEPS_) {
	gaddr self = c.arg(0), pos = c.arg(1);
	gaddr prev = emu::r32(pos + 4);
	emu::w32(self, pos);
	emu::w32(self + 4, prev);
	emu::w32(prev, self);
	emu::w32(pos + 4, self);
}
IMPORT(_ZNSt15_List_node_base6unhookEv) {
	gaddr self = c.arg(0);
	gaddr next = emu::r32(self), prev = emu::r32(self + 4);
	emu::w32(prev, next);
	emu::w32(next + 4, prev);
}

// The type_info vtables only need distinct addresses: __dynamic_cast tells the
// class kinds apart by them
static gaddr vtClass, vtSi, vtVmi;
IMPORT_DATA(_ZTVN10__cxxabiv117__class_type_infoE) { return vtClass = emu::sysAlloc(64); }
IMPORT_DATA(_ZTVN10__cxxabiv120__si_class_type_infoE) { return vtSi = emu::sysAlloc(64); }
IMPORT_DATA(_ZTVN10__cxxabiv121__vmi_class_type_infoE) { return vtVmi = emu::sysAlloc(64); }

static bool sameType(gaddr a, gaddr b) {
	return a == b || emu::cstr(emu::r32(a + 4)) == emu::cstr(emu::r32(b + 4));
}

// Finds the subobject of type dst inside the object of type t at obj
static gaddr findBase(gaddr obj, gaddr t, gaddr dst, int depth = 0) {
	if (sameType(t, dst))
		return obj;
	if (depth > 32)
		return 0;
	gaddr vt = emu::r32(t);
	if (vt == vtSi + 8)
		return findBase(obj, emu::r32(t + 8), dst, depth + 1);
	if (vt == vtVmi + 8) {
		uint32_t count = emu::r32(t + 12);
		for (uint32_t i = 0; i < count; i++) {
			gaddr base = emu::r32(t + 16 + i * 8);
			int32_t flags = (int32_t)emu::r32(t + 20 + i * 8);
			if (!(flags & 2))
				continue; // not public
			int32_t offset = flags >> 8;
			gaddr sub;
			if (flags & 1) // virtual base: offset is in the vtable
				sub = obj + (int32_t)emu::r32(emu::r32(obj) + offset);
			else
				sub = obj + offset;
			gaddr r = findBase(sub, base, dst, depth + 1);
			if (r)
				return r;
		}
	}
	return 0;
}

IMPORT(__dynamic_cast) {
	gaddr sub = c.arg(0), dst = c.arg(2);
	if (!sub) {
		c.ret(0);
		return;
	}
	gaddr vptr = emu::r32(sub);
	int32_t toTop = (int32_t)emu::r32(vptr - 8);
	gaddr whole = sub + toTop;
	gaddr wholeType = emu::r32(vptr - 4);
	c.ret(findBase(whole, wholeType, dst));
}
