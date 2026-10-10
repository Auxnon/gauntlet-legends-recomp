// Native replacement for boot's loader (func_8020010C in src/boot/1050.c of the decomp).
//
// The original DMAs the compressed game image by programming the PI registers
// directly, which recompiled code can't do. This performs the same steps, reading
// the ROM through librecomp instead, and calls the recompiled pieces for the rest.

#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "librecomp/game.hpp"

extern "C" {
    void MapTLB(uint8_t* rdram, recomp_context* ctx);
    void inflate_decompress(uint8_t* rdram, recomp_context* ctx);
    void game_entry_kseg0(uint8_t* rdram, recomp_context* ctx);
}

namespace {
    // MEM_* macros need addresses sign-extended to 64 bits, as MIPS registers hold them.
    constexpr gpr address(uint32_t n64_address) {
        return (gpr)(int64_t)(int32_t)n64_address;
    }

    // Addresses from the decomp's build/boot.elf.
    constexpr gpr boot_metadata_copy_start = address(0x802002B0); // sBootMetadataCopyStart
    constexpr gpr boot_rom_file_table = address(0x802002C4);      // sBootRomFileTable
    constexpr gpr game_metadata_dest = address(0xE01301C0);       // D_E01301C0
    constexpr int32_t boot_metadata_copy_size = 0x1E0;

    constexpr gpr game_compressed_dram = address(0x80300000);     // PHYS_TO_K0(0x300000)
    constexpr uint32_t pi_cart_dom1_base = 0x10000000;
    constexpr uint32_t max_pi_transfer_length = 0xFFFF8;

    // RomFileEntry field offsets (sizeof = 0x30).
    constexpr int32_t rom_file_entry_size = 0x30;
    constexpr int32_t entry_rom_offset = 0x10;
    constexpr int32_t entry_compressed_size = 0x14;
    constexpr int32_t entry_decompressed_vram = 0x18;
    constexpr int32_t entry_bss_start = 0x20;
    constexpr int32_t entry_bss_size = 0x24;
    constexpr int32_t table_entries_offset = 0x1C;

    bool entry_name_is(uint8_t* rdram, gpr entry, const char* name) {
        for (int32_t i = 0;; i++) {
            char c = (char)MEM_B(i, entry);
            if (c != name[i]) {
                return false;
            }
            if (c == '\0') {
                return true;
            }
        }
    }
}

extern "C" void func_8020010C(uint8_t* rdram, recomp_context* ctx) {
    int32_t boot_arg = (int32_t)ctx->r4;

    // Locate the compressed "game" binary in the boot ROM file table.
    uint32_t entry_count = (uint32_t)MEM_W(0, boot_rom_file_table);
    gpr game_file = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        game_file = boot_rom_file_table + table_entries_offset + (int32_t)i * rom_file_entry_size;
        if (entry_name_is(rdram, game_file, "game")) {
            break;
        }
    }

    // DMA the compressed image to physical 0x300000, as boot does through the PI.
    uint32_t rom_offset = (uint32_t)MEM_W(entry_rom_offset, game_file);
    uint32_t dma_length = ((uint32_t)MEM_W(entry_compressed_size, game_file) + 7) & ~7u;
    if (dma_length > max_pi_transfer_length) {
        dma_length = max_pi_transfer_length;
    }
    recomp::do_rom_read(rdram, game_compressed_dram, pi_cart_dom1_base + rom_offset, dma_length);

    MapTLB(rdram, ctx);

    ctx->r4 = game_compressed_dram;
    ctx->r5 = address((uint32_t)MEM_W(entry_decompressed_vram, game_file));
    inflate_decompress(rdram, ctx);

    // Clear the game BSS.
    gpr bss_start = address((uint32_t)MEM_W(entry_bss_start, game_file));
    int32_t bss_size = MEM_W(entry_bss_size, game_file);
    for (int32_t i = 0; i < bss_size; i++) {
        MEM_B(i, bss_start) = 0;
    }

    // Copy the boot ROM file table into the freshly inflated game image.
    for (int32_t i = 0; i < boot_metadata_copy_size; i++) {
        MEM_B(i, game_metadata_dest) = MEM_B(i, boot_metadata_copy_start);
    }

    ctx->r4 = (gpr)(int64_t)boot_arg;
    ctx->r5 = boot_metadata_copy_start + boot_metadata_copy_size;
    game_entry_kseg0(rdram, ctx);
}
