# Builds cfrun for one architecture.
#   make DEPS=<prefix with unicorn and SDL2> ARCH=arm64|x86_64 OUT=<build dir>
ARCH ?= $(shell uname -m)
OUT ?= ../build/$(ARCH)
DEPS ?= ../deps/prefix
MIN ?= 11.0

CXX = clang++ -arch $(ARCH) -mmacosx-version-min=$(MIN)
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wno-unused-function -I$(DEPS)/include -I$(DEPS)/include/SDL2 -D_THREAD_SAFE
LDFLAGS = $(DEPS)/lib/libunicorn.a $(DEPS)/lib/libSDL2.a -lz -liconv \
	-framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox \
	-framework CoreVideo -framework Carbon -framework ForceFeedback -framework GameController \
	-framework CoreHaptics -framework Metal -framework QuartzCore -framework ImageIO -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreText -weak_framework CoreHaptics

SRCS = $(wildcard *.cpp)
OBJS = $(patsubst %.cpp,$(OUT)/%.o,$(SRCS))

$(OUT)/cfrun: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

$(OUT)/%.o: %.cpp $(wildcard *.h) | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)
