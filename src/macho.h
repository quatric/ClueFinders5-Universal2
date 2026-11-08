#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

typedef uint32_t gaddr;

struct MachO {
	gaddr entry = 0;
	gaddr imageEnd = 0;
	gaddr cfstringStart = 0, cfstringEnd = 0;
	std::vector<gaddr> initFuncs;
	std::vector<std::string> importNames;
	std::map<gaddr, std::string> symbols;

	bool load(const std::string &path);
	std::string nameFor(gaddr a) const;
};

extern MachO gImage;
