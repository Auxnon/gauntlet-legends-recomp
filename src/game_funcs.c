// Native replacements for game functions N64Recomp can't translate, and for libultra
// internals the runtime doesn't provide.

#include <stdio.h>
#include <stdlib.h>

#include "recomp.h"

// Hand-written atomics (ll/sc loops). Recompiled game threads never run concurrently,
// so plain read-modify-write is enough.

// u32 atomic_fetch_or_masked(u32* ptr, u32 mask): *ptr |= mask, returns old & mask.
RECOMP_FUNC void func_game_E0080C54_80854(uint8_t* rdram, recomp_context* ctx) {
    uint32_t old = MEM_W(0, ctx->r4);
    MEM_W(0, ctx->r4) = old | (uint32_t)ctx->r5;
    ctx->r2 = (gpr)(int32_t)(old & (uint32_t)ctx->r5);
}

// u32 atomic_fetch_clear_masked(u32* ptr, u32 mask): *ptr &= ~mask, returns old & mask.
RECOMP_FUNC void func_game_E0080C74_80874(uint8_t* rdram, recomp_context* ctx) {
    uint32_t old = MEM_W(0, ctx->r4);
    MEM_W(0, ctx->r4) = old & ~(uint32_t)ctx->r5;
    ctx->r2 = (gpr)(int32_t)(old & (uint32_t)ctx->r5);
}

// Data cache writeback of one line, and of an address range. No caches on PC.
RECOMP_FUNC void func_game_E0080CDC_808DC(uint8_t* rdram, recomp_context* ctx) {
}

RECOMP_FUNC void func_game_E00A6420_A6020(uint8_t* rdram, recomp_context* ctx) {
}

// The game reads the current scanline (likely for timing); approximate it by cycling
// through an NTSC field.
void osViGetCurrentLine_recomp(uint8_t* rdram, recomp_context* ctx) {
    static uint32_t line = 0;
    line = (line + 2) % 525;
    ctx->r2 = line;
}

// libultra internals reachable only from code the runtime replaces wholesale (the VI
// manager thread, the thread dispatcher, controller pak I/O). Fail loudly if that
// assumption is ever wrong.
#define UNREACHABLE(name)                                                       \
    void name(uint8_t* rdram, recomp_context* ctx) {                            \
        fprintf(stderr, "[gauntlet] unexpected call to %s\n", #name);          \
        abort();                                                                \
    }

UNREACHABLE(__osContAddressCrc_recomp)
UNREACHABLE(__osContDataCrc_recomp)
UNREACHABLE(__osContRamRead_recomp)
UNREACHABLE(__osContRamWrite_recomp)
UNREACHABLE(__osEnqueueThread_recomp)
UNREACHABLE(__osPfsSelectBank_recomp)
UNREACHABLE(__osPopThread_recomp)
UNREACHABLE(__osSiRawStartDma_recomp)
UNREACHABLE(__osTimerInterrupt_recomp)
UNREACHABLE(__osViGetCurrentContext_recomp)
UNREACHABLE(__osViSwapContext_recomp)

// s32 osPiWriteIo(u32 devAddr, u32 data): a raw cartridge-bus write. Nothing on PC
// listens there, so drop it, but log the first few so it shows up if it matters.
void osPiWriteIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int logged = 0;
    if (logged < 8) {
        fprintf(stderr, "[gauntlet] ignoring osPiWriteIo(0x%08X, 0x%08X)\n", (uint32_t)ctx->r4, (uint32_t)ctx->r5);
        logged++;
    }
    ctx->r2 = 0;
}
