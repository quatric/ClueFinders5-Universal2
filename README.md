# ClueFinders 5th Grade Adventures — Universal 2

Runs Software MacKiev's Mac OS X release of *The ClueFinders 5th Grade
Adventures: Secret of the Living Volcano* (a 32-bit Intel Carbon app) on current
macOS, on both Apple silicon and Intel.

The original `i386` executable is not modified. `cfrun` loads it into an
embedded x86 CPU emulator ([Unicorn](https://www.unicorn-engine.org/)) and
supplies its own versions of the system calls it imports: classic and Carbon
events, windows, QuickDraw with 8-bit palettes, the Sound Manager, QuickTime
movies and graphics importers, the Resource and File Managers, CoreFoundation,
libc and so on. Graphics, input and audio go through SDL2.

It shares its compatibility layer with the ClueFinders 3rd and 4th Grade
ports (`ClueFinders3-Universal2`, `ClueFinders4-Universal2`), which grew out
of the Thinkin' Science port. 5th Grade needed only four more calls: `fscanf`
and `rename` for its data files, and `GetSharedLibrary`/`PBHGetFInfoAsync`.
The game asks `GetSharedLibrary` for a PowerPC code fragment, which can't load
here, so it gets "no library" and goes on without it.

## Building

Needs the Xcode command line tools, CMake and `ffmpeg` with libx264
(Homebrew's works). Everything else is in this folder:

| Folder | Contents |
| --- | --- |
| `src/` | `cfrun`, the compatibility layer |
| `scripts/` | build scripts, the movie converter and the installer unpacker |
| `deps/prefix/` | universal static `libSDL2.a` and `libunicorn.a` with their headers |
| `deps/unicorn/` | Unicorn source (with one patch, below), for rebuilding `libunicorn.a` |
| `game/` | the original game as installed from the MacKiev disc (not for redistribution; ignored by git) |
| `test/` | headless play-testing scripts (see below) |
| `analysis/` | the game's i386 executable and its disassembly, for looking up call sites |

Build the app:

```bash
scripts/build_app.sh deps/prefix "game/ClueFinders 5th Grade/ClueFinders 5th Grade.app" dist
```

This builds `cfrun` for both architectures, joins them with `lipo` and copies
the original game into `Contents/Resources/Game/ClueFinders 5th Grade`. It
then converts the movies (below), writes an `Info.plist` and signs the bundle
ad hoc. The result is `dist/ClueFinders 5th Grade.app`.

Build the placement test the same way:

```bash
scripts/build_assessment.sh deps/prefix "game/ClueFinders 5th Grade/Assessment 5.app" dist
```

The result is `dist/ClueFinders 5th Grade Assessment.app`. Install it in the
same folder as the game.

### Movies

39 of the game's 41 movies are Sorenson Video 3, which nothing on current
macOS decodes. `scripts/transcode_movies.sh <MOVIES folder>` converts them in
place to H.264 without B-frames, copying the audio tracks unchanged. One movie
already uses H.264 with B-frames and is converted too. `cfrun` decodes H.264
with VideoToolbox, one sample at a time in file order, and Animation (`rle `)
movies itself. The build script runs the converter on the copy inside the app.
The converter skips files that are already converted, so running it again
does nothing.

### Unicorn

To rebuild Unicorn:

```bash
scripts/build_unicorn.sh deps/unicorn deps/prefix
```

Unicorn has to be built once per architecture and then combined with `lipo`;
the script does this.

`deps/unicorn` carries one fix, in `qemu/include/tcg/tcg-apple-jit.h`.
`JIT_CALLBACK_GUARD` restored the thread's JIT write protection after a hook
but not `uc->current_executable`. When a hook ran guest code itself (a nested
`uc_emu_start`, which `cfrun` does for event handlers, timers and sound
callbacks), the two disagreed afterwards. The next translated block then ran
from a page that was still writable, which crashes with SIGBUS on Apple
silicon. The guard now updates both.

### Getting the game

The disc image (`ClueFinders 5rd Grade Hybrid.iso`) holds a Mill
InstallerMaker installer. Its payload is
`ClueFinders 5th Grade Installer.app/Contents/Resources/Archive.zp`, and the
manifest is `Archive.zplist` next to it. To unpack:

1. Decompress `Archive.zplist` with zlib.
2. Convert the result to XML with `scripts/ts2plist.m`
   (`clang -framework Foundation`).
3. Run `scripts/unpack_zp.py <manifest.plist> Archive.zp game/`.

This gives `game/ClueFinders 5th Grade/` with the game and
`Assessment 5.app`.

## Running

Open the built app. Started without arguments, `cfrun` runs the copy of the
game inside its own bundle. You can also give it the path to an original
`ClueFinders 5th Grade.app`.

The game runs full screen over its own backdrop; **Cmd-F** switches between
full screen and a window. Cmd-Q goes to the game's own "Are you sure you want
to quit?" dialog. Player files go to
`~/Library/Application Support/ClueFinders 5th Grade`.

### The Assessment

`ClueFinders 5th Grade Assessment.app` runs the disc's `Assessment 5.app`, a
placement test that sets the starting level of each activity. It writes its
results to `Software MacKiev/ClueFinders 5/CF5Assessment.txt` in the Application
Support folder. The game reads that file when a new player signs in, and then
deletes it.

So that both apps see the same file, the Assessment's `Info.plist` sets
`TSSupportName` to `ClueFinders 5th Grade`. Its files then go into the game's
support folder instead of one of its own.

At the end of the test, **Yes** opens the game. The Assessment asks Launch
Services for `com.mackiev.ClueFinders5`. `cfrun` answers with the game's
Universal 2 app (`com.mackiev.cluefinders5.universal`): first the copy next to
the Assessment, otherwise any copy Launch Services knows about.

Environment variables used for testing:

| Variable | Effect |
| --- | --- |
| `TS_WINDOWED=1` | start in a window |
| `TS_SCREEN=WxH` | size of the virtual screen (default: display aspect at 800 high) |
| `TS_SUPPORT=<dir>` | where preferences and player files go |
| `TS_HEADLESS=1` | no window; frames are only kept for screenshots |
| `TS_SCRIPT=<file>` / `TS_CONTROL=<fifo>` | scripted input: `wait`, `click`, `down`, `up`, `move`, `key`, `shot`, `clickcolor`, `trace on/off`, `prof on/off`, `quit` (see `src/host.cpp`) |
| `TS_LOG=1` | log calls, missing resources and host crashes to stderr |
| `TS_TRACE=1` | log every imported call (very verbose) |
| `TS_LENIENT=1` | porting aid: an unimplemented import logs `MISSING` and returns 0 instead of stopping |
| `TS_BREAK=addr,...` | porting aid: log each time the guest executes one of these (hex) addresses |

`prof on` / `prof off` count the guest's basic blocks in between and log the
60 busiest addresses. This shows what the game is doing when it seems stuck.

## Testing

`test/run.sh [secs]` builds an arm64 `cfrun` into `build/` and runs the game
for a while, logging to `test/run.log`. Set `TS_HEADLESS=1` to keep it off
screen and `TS_SCRIPT` to feed it input (`test/play.txt` skips the opening
movie and signs in a new player; run it from `test/`, where its screenshots go).
`test/bg.sh start|cmd|stop` keeps a headless game running behind a control
FIFO (set `TS_APP` to the path of `Assessment 5.app` to run the test instead), and `test/do.sh` sends it commands (`do.sh "click 712 535" "wait 5" "shot
name"`). Screenshots go to `test/shots/`.

## Layout

| File | Role |
| --- | --- |
| `src/emu.*`, `macho.*`, `heap.*` | Mach-O loader, guest memory, import thunks, calls back into guest code, profiling |
| `src/window.cpp` | Carbon and classic Event Manager, Window Manager, window compositing |
| `src/qd.cpp`, `palette.cpp`, `pict.cpp` | QuickDraw, regions, GWorlds, palettes and color tables, PICT decoding |
| `src/text.cpp` | text via CoreText, font activation, CoreGraphics images, QuickTime graphics importers |
| `src/display.cpp` | display modes (the game's 1280×800 check), menu bar and system UI mode |
| `src/controls.cpp` | Control Manager and nib loading |
| `src/sound.cpp`, `movie.cpp` | Sound Manager channels; QuickTime movies (H.264 and Animation) |
| `src/resources.cpp`, `files.cpp`, `cf.cpp` | Resource Manager, File Manager, CoreFoundation |
| `src/posix.cpp`, `libc.cpp` | stdio, file descriptors, strings, math |
| `src/misc.cpp`, `toolbox.cpp` | alerts, color cursors, menus, timing |
| `src/launch.cpp` | Launch Services, used by the Assessment to open the game |
| `src/host.cpp` | SDL window, input, audio device, screenshots, scripted input |

## Status

Tested on arm64 and x86_64 (Rosetta). Working:

- the publisher logos and the opening movies (a click skips movies)
- sign-in with a new player and resuming a saved one
- the beach hub: the walk cursors, hotspots, and walking to Sulfur River
- Sulfur River's intro movie and dragging blocks in the activity
- Practice Mode: all ten activities (Shark Crossing, Sulfur River, Faces within
  the Face, Tube Cavern, Spelling Catwalks, CrypTile Stories, Fish within the
  Fish, Biosphere Chambers, In the Minisub, Poison Garden) load, and the raft
  icon goes back to the menu
- a Shark Crossing puzzle played through: wrong pieces are turned back with a
  hint call from Tony; the right path turns into a bridge, and the next round
  starts
- the game's quit dialog
- the Assessment: every question to the results screen, the results file, and
  **Yes** opening the installed game; a new player signing in to the game uses
  the results

Not seen yet: playing the other activities through, and the rest of the story.

Known gaps:

- The PowerPC code fragment the game looks for (`GetSharedLibrary`) never
  loads.
- The Assessment's Apple-event help launcher does nothing.
- Printing reports "cancelled".
- Classic black-and-white cursors (`SetCursor`) show the system arrow. The
  game's color cursors are converted to SDL cursors at their original 16-point
  size.
