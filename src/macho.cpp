// Loads the i386 slice of a Mac OS X 10.4-era executable (MH_EXECUTE, fixed
// addresses) and binds its imports to native thunks.
#include "macho.h"
#include "emu.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace {

uint32_t rd32(const std::vector<uint8_t> &d, size_t off) {
	uint32_t v;
	memcpy(&v, &d[off], 4);
	return v;
}

uint32_t rdBE32(const std::vector<uint8_t> &d, size_t off) {
	return (uint32_t)d[off] << 24 | (uint32_t)d[off + 1] << 16 | (uint32_t)d[off + 2] << 8 | d[off + 3];
}

} // namespace

bool MachO::load(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	// Pick the i386 slice from a fat binary
	size_t base = 0;
	if (file.size() > 8 && rdBE32(file, 0) == 0xCAFEBABE) {
		uint32_t n = rdBE32(file, 4);
		bool found = false;
		for (uint32_t i = 0; i < n; i++) {
			if (rdBE32(file, 8 + i * 20) == 7) { // CPU_TYPE_X86
				base = rdBE32(file, 8 + i * 20 + 8);
				found = true;
			}
		}
		if (!found)
			emu::fatal("%s has no i386 slice", path.c_str());
	}
	std::vector<uint8_t> d(file.begin() + base, file.end());
	if (rd32(d, 0) != 0xFEEDFACE || rd32(d, 4) != 7)
		emu::fatal("%s is not an i386 Mach-O", path.c_str());

	uint32_t ncmds = rd32(d, 16);
	size_t off = 28;
	uint32_t symoff = 0, nsyms = 0, stroff = 0, indirectoff = 0, extreloff = 0, nextrel = 0;
	struct Sect {
		std::string seg, name;
		uint32_t addr, size, flags, reserved1, reserved2;
	};
	std::vector<Sect> sects;

	for (uint32_t i = 0; i < ncmds; i++) {
		uint32_t cmd = rd32(d, off), cmdsize = rd32(d, off + 4);
		if (cmd == 1) { // LC_SEGMENT
			std::string seg((const char *)&d[off + 8], strnlen((const char *)&d[off + 8], 16));
			uint32_t vmaddr = rd32(d, off + 24), vmsize = rd32(d, off + 28);
			uint32_t fileoff = rd32(d, off + 32), filesize = rd32(d, off + 36);
			uint32_t nsects = rd32(d, off + 48);
			if (vmsize && seg != "__PAGEZERO") {
				if (vmaddr < kImageBase || vmaddr + vmsize > kImageEnd)
					emu::fatal("segment %s at 0x%08x does not fit", seg.c_str(), vmaddr);
				memset(emu::ptr(vmaddr), 0, vmsize);
				memcpy(emu::ptr(vmaddr), &d[fileoff], filesize);
				if (vmaddr + vmsize > imageEnd)
					imageEnd = vmaddr + vmsize;
			}
			for (uint32_t s = 0; s < nsects; s++) {
				size_t so = off + 56 + s * 68;
				Sect sec;
				sec.name.assign((const char *)&d[so], strnlen((const char *)&d[so], 16));
				sec.seg.assign((const char *)&d[so + 16], strnlen((const char *)&d[so + 16], 16));
				sec.addr = rd32(d, so + 32);
				sec.size = rd32(d, so + 36);
				sec.flags = rd32(d, so + 56);
				sec.reserved1 = rd32(d, so + 60);
				sec.reserved2 = rd32(d, so + 64);
				sects.push_back(sec);
			}
		} else if (cmd == 2) { // LC_SYMTAB
			symoff = rd32(d, off + 8);
			nsyms = rd32(d, off + 12);
			stroff = rd32(d, off + 16);
		} else if (cmd == 0xB) { // LC_DYSYMTAB
			indirectoff = rd32(d, off + 56);
			extreloff = rd32(d, off + 64);
			nextrel = rd32(d, off + 68);
		} else if (cmd == 5) { // LC_UNIXTHREAD: i386_thread_state_t, eip is field 10
			entry = rd32(d, off + 16 + 10 * 4);
		}
		off += cmdsize;
	}

	auto symName = [&](uint32_t idx) -> std::string {
		uint32_t strx = rd32(d, symoff + idx * 12);
		std::string n((const char *)&d[stroff + strx]);
		if (!n.empty() && n[0] == '_')
			n = n.substr(1);
		return n;
	};

	// Symbols defined in the image itself (including coalesced templates
	// that the import tables still go through) bind to their definition
	auto definedAt = [&](uint32_t idx) -> gaddr {
		uint8_t type = d[symoff + idx * 12 + 4];
		return (type & 0x0E) == 0x0E ? rd32(d, symoff + idx * 12 + 8) : 0;
	};

	// Defined symbols, for backtraces
	for (uint32_t i = 0; i < nsyms; i++) {
		uint8_t type = d[symoff + i * 12 + 4];
		uint32_t value = rd32(d, symoff + i * 12 + 8);
		if ((type & 0x0E) == 0x0E && value) // N_SECT
			symbols[value] = symName(i);
	}

	for (const Sect &s : sects) {
		uint32_t type = s.flags & 0xFF;
		if (s.name == "__mod_init_func") {
			for (uint32_t a = s.addr; a < s.addr + s.size; a += 4)
				initFuncs.push_back(emu::r32(a));
		} else if (s.name == "__cfstring") {
			cfstringStart = s.addr;
			cfstringEnd = s.addr + s.size;
		} else if (s.seg == "__DATA" && s.name == "__dyld") {
			emu::w32(s.addr, imports::thunk("__dyld_lazy_symbol_binding_entry_point"));
			emu::w32(s.addr + 4, imports::thunk("__dyld_func_lookup"));
		} else if (type == 6 || type == 8) {
			// S_NON_LAZY_SYMBOL_POINTERS or S_SYMBOL_STUBS (self-modifying jump table)
			uint32_t stride = type == 6 ? 4 : s.reserved2;
			uint32_t count = s.size / stride;
			for (uint32_t k = 0; k < count; k++) {
				uint32_t idx = rd32(d, indirectoff + (s.reserved1 + k) * 4);
				if (idx & 0xC0000000)
					continue; // local or absolute: already correct
				std::string name = symName(idx);
				gaddr slot = s.addr + k * stride;
				gaddr local = definedAt(idx);
				if (type == 6) {
					bool isData = false;
					gaddr v = local ? local : imports::data(name, &isData);
					emu::w32(slot, local || isData ? v : imports::thunk(name));
				} else {
					gaddr t = local ? local : imports::thunk(name);
					emu::w8(slot, 0xE9); // jmp rel32
					emu::w32(slot + 1, t - (slot + 5));
				}
				importNames.push_back(name);
			}
		}
	}
	// External relocations: 32-bit absolute references to imported symbols in
	// data (type_info vtables, CFString isa, pure virtual slots). The value
	// already there is the addend.
	for (uint32_t i = 0; i < nextrel; i++) {
		uint32_t address = rd32(d, extreloff + i * 8);
		uint32_t info = rd32(d, extreloff + i * 8 + 4);
		uint32_t symnum = info & 0xFFFFFF;
		bool pcrel = (info >> 24) & 1;
		uint32_t length = (info >> 25) & 3;
		bool external = (info >> 27) & 1;
		if (address & 0x80000000 || !external || pcrel || length != 2)
			emu::fatal("unsupported relocation %u at 0x%08x", i, address);
		std::string name = symName(symnum);
		bool isData = false;
		gaddr v = definedAt(symnum);
		if (!v)
			v = imports::data(name, &isData);
		if (!v && !isData)
			v = imports::thunk(name);
		emu::w32(address, emu::r32(address) + v);
	}
	return true;
}

std::string MachO::nameFor(gaddr a) const {
	auto it = symbols.upper_bound(a);
	if (it == symbols.begin())
		return std::string();
	--it;
	char buf[32];
	snprintf(buf, sizeof(buf), "+0x%x", a - it->first);
	return it->second + buf;
}
