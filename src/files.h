// File Manager: FSRefs and FSSpecs name host paths
#pragma once

#include "emu.h"

#include <string>

namespace files {

void writeFSRef(gaddr ref, const std::string &path);
std::string pathOfFSRef(gaddr ref); // empty if unknown
std::string pathOfFSSpec(gaddr spec);
void writeFSSpec(gaddr spec, const std::string &path);
// Host path of a file named by a directory ID and a Mac name or partial path
std::string resolvePath(uint32_t dirID, const std::string &macName);

} // namespace files
