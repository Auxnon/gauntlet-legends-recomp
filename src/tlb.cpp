// TLB emulation for Gauntlet Legends.
//
// The game runs its code from TLB-mapped addresses: boot maps 0xE0000000-0xE07FFFFF
// onto physical RAM 0-8MB, and the game maps each level overlay (Tower, Boss, ...)
// at a fixed 0xE08xxxxx address on top of wherever its heap put it.
//
// Recompiled code turns an N64 address A into `rdram + (u32)(A - 0x80000000)`, and
// librecomp reserves 4GB for rdram, so every TLB address already has a host address
// inside the reservation (0xE0000000 -> rdram + 0x60000000). We back physical RAM
// with a memfd and mmap the same physical pages at each mapped virtual range, so the
// aliases share memory exactly like the real TLB, with no per-access cost.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <sys/mman.h>
#include <unistd.h>

#include "recomp.h"
#include "librecomp/addresses.hpp"

namespace {
    constexpr uint32_t num_tlb_entries = 32;
    // Only the low 16MB has been written by the time the game initialises (boot code,
    // IPL3 variables, librecomp's patch data); mods and the heap come later.
    constexpr size_t initial_copy_size = 16 * 1024 * 1024;

    struct TlbEntry {
        bool valid = false;
        uint32_t vaddr = 0;       // start of the even page
        uint32_t page_size = 0;   // size of each of the even/odd pages
        uint32_t paddr[2] = {};   // physical address of each page, or UINT32_MAX if unmapped
    };

    std::mutex tlb_mutex;
    std::array<TlbEntry, num_tlb_entries> tlb_entries{};
    int physical_memory_fd = -1;
    uint8_t* rdram_base = nullptr;

    [[noreturn]] void fatal(const char* what) {
        fprintf(stderr, "[TLB] %s: %s\n", what, strerror(errno));
        std::abort();
    }

    uint8_t* host_address(uint32_t n64_address) {
        return rdram_base + (uint32_t)(n64_address - 0x80000000u);
    }

    // kseg0/kseg1 are direct-mapped and already backed by rdram; never remap them.
    bool is_tlb_mapped(uint32_t vaddr) {
        return vaddr < 0x80000000u || vaddr >= 0xC0000000u;
    }

    void unmap_range(uint32_t vaddr, uint32_t size) {
        if (mmap(host_address(vaddr), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
            fatal("unmapping a TLB page");
        }
    }

    void map_range(uint32_t vaddr, uint32_t size, uint32_t paddr) {
        if ((uint64_t)paddr + size > recomp::mem_size) {
            fprintf(stderr, "[TLB] physical range 0x%08X+0x%X is outside RDRAM\n", paddr, size);
            std::abort();
        }
        if (mmap(host_address(vaddr), size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, physical_memory_fd, paddr) == MAP_FAILED) {
            fatal("mapping a TLB page");
        }
    }

    void unmap_entry(TlbEntry& entry) {
        if (!entry.valid) {
            return;
        }
        for (int half = 0; half < 2; half++) {
            if (entry.paddr[half] != UINT32_MAX) {
                unmap_range(entry.vaddr + half * entry.page_size, entry.page_size);
            }
        }
        entry = {};
    }
}

namespace gauntlet {
    // Swap rdram's anonymous backing for a memfd so physical pages can be aliased.
    // Called from the game's on_init callback, before any game code runs.
    void init_tlb(uint8_t* rdram) {
        rdram_base = rdram;
        physical_memory_fd = memfd_create("gauntlet-rdram", 0);
        if (physical_memory_fd < 0) {
            fatal("memfd_create");
        }
        if (ftruncate(physical_memory_fd, recomp::mem_size) != 0) {
            fatal("sizing physical memory");
        }
        if (pwrite(physical_memory_fd, rdram, initial_copy_size, 0) != (ssize_t)initial_copy_size) {
            fatal("copying initial RDRAM contents");
        }
        if (mmap(rdram, recomp::mem_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, physical_memory_fd, 0) == MAP_FAILED) {
            fatal("remapping RDRAM");
        }
    }
}

extern "C" void osMapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t index = (uint32_t)ctx->r4;
    uint32_t page_mask = (uint32_t)ctx->r5;
    uint32_t vaddr = (uint32_t)ctx->r6;
    uint32_t even_paddr = (uint32_t)ctx->r7;
    uint32_t odd_paddr = (uint32_t)MEM_W(0x10, ctx->r29);

    if (index >= num_tlb_entries) {
        fprintf(stderr, "[TLB] osMapTLB index %u out of range\n", index);
        std::abort();
    }

    std::lock_guard lock{ tlb_mutex };
    TlbEntry& entry = tlb_entries[index];
    unmap_entry(entry);

    // Boot invalidates every entry by "mapping" kseg0 with no physical pages.
    if (!is_tlb_mapped(vaddr) || (even_paddr == UINT32_MAX && odd_paddr == UINT32_MAX)) {
        return;
    }

    uint32_t page_size = (page_mask + 0x2000) >> 1;
    entry.valid = true;
    entry.vaddr = vaddr & ~(page_size * 2 - 1);
    entry.page_size = page_size;
    entry.paddr[0] = even_paddr == UINT32_MAX ? UINT32_MAX : (even_paddr & ~0xFFFu);
    entry.paddr[1] = odd_paddr == UINT32_MAX ? UINT32_MAX : (odd_paddr & ~0xFFFu);

    for (int half = 0; half < 2; half++) {
        if (entry.paddr[half] != UINT32_MAX) {
            map_range(entry.vaddr + half * page_size, page_size, entry.paddr[half]);
        }
    }
}

extern "C" void osUnmapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t index = (uint32_t)ctx->r4;
    if (index >= num_tlb_entries) {
        return;
    }
    std::lock_guard lock{ tlb_mutex };
    unmap_entry(tlb_entries[index]);
}

extern "C" void osUnmapTLBAll_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ tlb_mutex };
    for (TlbEntry& entry : tlb_entries) {
        unmap_entry(entry);
    }
}

// Returns the physical address for a TLB-mapped virtual address, or -1.
// The game's own osVirtualToPhysical calls this for non-kseg0/1 addresses.
extern "C" void __osProbeTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t vaddr = (uint32_t)ctx->r4;
    std::lock_guard lock{ tlb_mutex };
    for (const TlbEntry& entry : tlb_entries) {
        if (!entry.valid || vaddr < entry.vaddr || vaddr >= entry.vaddr + entry.page_size * 2) {
            continue;
        }
        uint32_t offset = vaddr - entry.vaddr;
        uint32_t half = offset / entry.page_size;
        if (entry.paddr[half] == UINT32_MAX) {
            break;
        }
        ctx->r2 = (gpr)(int32_t)(entry.paddr[half] + offset % entry.page_size);
        return;
    }
    ctx->r2 = (gpr)(int32_t)-1;
}
