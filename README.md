# Gauntlet Legends Recompiled (spike)

An experimental static recompilation of Gauntlet Legends (N64, US) to native PC code,
built on [N64Recomp](https://github.com/N64Recomp/N64Recomp),
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and
[RT64](https://github.com/rt64/rt64). It uses the
[gauntlet-legends decomp](https://github.com/Drahsid/gauntlet-legends) for symbols and
segment layout.

**Status:** boots, creates the game threads and sends display lists to RT64. Audio is
skipped (the audio microcode isn't recompiled yet), level overlays are untested, and it
currently runs only on Linux (the TLB emulation uses `memfd` + `mmap`).

No ROM data is in this repo. You need your own US ROM
(sha1 `0489dcce749c6a5102681d288ed0616a4b94e99d`).

## Building (Linux)

Dependencies (Fedora names): `clang lld llvm ninja-build cmake gtk3-devel freetype-devel
vulkan-headers vulkan-loader-devel sdl2-compat-devel binutils-mips64-linux-gnu`, plus
[uv](https://docs.astral.sh/uv/).

### 1. Build the decomp next to this repo

```sh
git clone --recursive https://github.com/Drahsid/gauntlet-legends ../gauntlet-legends
cd ../gauntlet-legends
git apply ../gauntlet-legends-recomp/tools/patches/decomp-glz-kwargs.patch
cp /path/to/baserom.z64 files/baserom.z64
uv venv && uv pip install -r requirements.txt 'splat64[mips]==0.40.0'
uv run python ./build.py setup
uv run python ./build.py
cd -
```

splat 0.50 generates symbol files that don't link, so pin 0.40.0. On Fedora the MIPS
binutils are named `mips64-linux-gnu-*`; the decomp expects `mips-linux-gnu-*`, so symlink
each tool into `~/.local/bin`, and make `mips-linux-gnu-ld` a wrapper:

```sh
#!/bin/sh
exec /usr/bin/mips64-linux-gnu-ld -m elf32btsmip "$@"
```

The build must end with a matching `build/gauntlet.z64`.

### 2. Build the tools

```sh
git submodule update --init --recursive
uv tool install cmake
cmake -S tools/N64Recomp -B tools/N64Recomp/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/N64Recomp/build -j6
git -C tools/n64sym apply ../patches/n64sym-no-static.patch
make -C tools/n64sym
```

### 3. Generate and recompile

```sh
mkdir -p generated
tools/n64sym/bin/n64sym ../gauntlet-legends/files/game -s -t -h 0xE0000400 -o generated/n64sym.game.txt
uv run --no-project --with pyelftools python tools/elf2syms.py ../gauntlet-legends generated --libultra generated/n64sym.game.txt
tools/find_unrecompilable.sh
```

This writes `generated/` (synthetic ROM + symbols) and `RecompiledFuncs/`, both derived
from the ROM and gitignored.

### 4. Build and run

```sh
CC=clang CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j6
./build/GauntletRecompiled ../gauntlet-legends/files/baserom.z64
```

Controls: arrows = stick, X = A, C = B, Z = Z, Enter = Start, A/S = L/R, IJKL = C buttons.
Gamepads work too. The window uses XWayland because native Wayland on NVIDIA stalls RT64's
swapchain.

## Layout

- `tools/elf2syms.py`: merges the decomp's ELFs into a synthetic ROM and N64Recomp symbol
  file, with extra function discovery (jal targets, function pointers, branch targets).
- `gauntlet.us.toml`: N64Recomp config, including instruction patches and the ignored list
  that `tools/find_unrecompilable.sh` maintains.
- `src/tlb.cpp`: TLB emulation (the game runs at `0xE0xxxxxx`), via aliased mappings of RDRAM.
- `src/boot.cpp`: native replacement for the boot loader, which drives PI DMA directly.
- `src/game_funcs.c`: native versions of functions the recompiler can't translate.

## License

GPL-3.0. `src/rt64_context.cpp` is adapted from
[Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp), which is GPL-3.0.
