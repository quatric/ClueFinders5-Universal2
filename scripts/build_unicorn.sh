#!/bin/zsh
# Builds a universal static libunicorn.a (x86 guest only) into <deps prefix>/lib.
#   build_unicorn.sh <unicorn source> <deps prefix>
# Each architecture gets its own CMake build: a single build for both picks
# one host code generator, and the other slice then runs the wrong machine
# code. Unicorn's CMake script probes the host by running the C compiler
# without -arch flags, hence the per-architecture compiler wrappers.
set -e
src=${1:A}; prefix=${2:A}
wrappers=$prefix/../bin
mkdir -p $wrappers $prefix/lib $prefix/include
for a in arm64 x86_64; do
	printf '#!/bin/sh\nexec /usr/bin/clang -arch %s "$@"\n' $a > $wrappers/cc-$a
	chmod +x $wrappers/cc-$a
	ARCHFLAGS="-arch $a" cmake -S $src -B $src/build-$a -DCMAKE_BUILD_TYPE=Release \
		-DUNICORN_ARCH=x86 -DBUILD_SHARED_LIBS=OFF -DCMAKE_OSX_ARCHITECTURES=$a \
		-DCMAKE_C_COMPILER=$wrappers/cc-$a -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
	cmake --build $src/build-$a -j8
done
lipo -create $src/build-arm64/libunicorn.a $src/build-x86_64/libunicorn.a -output $prefix/lib/libunicorn.a
rsync -a $src/include/unicorn $prefix/include/
lipo -info $prefix/lib/libunicorn.a
