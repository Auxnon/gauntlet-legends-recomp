#!/usr/bin/env python3
"""Merge the decomp's per-segment ELFs into one N64Recomp symbol-file input.

Gauntlet Legends ships its game code and level overlays compressed, and maps them
through the TLB at 0xE0xxxxxx. N64Recomp needs the decompressed bytes at a ROM
offset, so this writes a synthetic ROM: the real baserom followed by each
decompressed segment, plus a syms TOML describing every section and function.

The appended segments live past the end of the real ROM, so their ROM addresses
can never collide with a real PI DMA; the runtime registers them by hand after
the game decompresses them.
"""

import argparse
import re
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

# Boot code is loaded by IPL3 straight from ROM; everything else is decompressed.
BOOT_ROM = 0x1000
BOOT_SECTIONS = (".entry", ".main")  # prefixes of the boot code/data sections
SEGMENTS = ["game", "Transitions", "Tower", "Boss"]
# game's statically linked libultra, and n64sym matches in it that are data, not code.
LIBULTRA_START = 0xE0098000
LIBULTRA_END = 0xE00A4000
LIBULTRA_NOT_CODE = {"rspbootTextStart"}
# libultra functions to keep as recompiled game code rather than letting N64Recomp swap
# in the runtime's version. The runtime's osVirtualToPhysical ignores the TLB; the
# game's own one asks __osProbeTLB, which our runtime answers from its TLB table.
LIBULTRA_KEEP = {"osVirtualToPhysical": "game_osVirtualToPhysical"}

ALIGN = 0x1000


def Align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def Loaded_Range(elf: ELFFile, prefixes) -> tuple[int, bytes]:
    """Return (vram, bytes) spanning every allocated PROGBITS section with a matching name."""
    sections = [
        s for s in elf.iter_sections()
        if s["sh_type"] == "SHT_PROGBITS"
        and s["sh_flags"] & 2  # SHF_ALLOC
        and s["sh_size"] > 0
        and s.name.startswith(prefixes)
        and s["sh_addr"] >= 0x80000000
    ]
    start = min(s["sh_addr"] for s in sections)
    end = max(s["sh_addr"] + s["sh_size"] for s in sections)
    image = bytearray(end - start)
    for s in sections:
        offset = s["sh_addr"] - start
        image[offset : offset + s["sh_size"]] = s.data()
    return start, bytes(image)


def Glabel_Functions(elf: ELFFile, start: int, end: int) -> dict[int, tuple[str, int]]:
    """Sized FUNC symbols (from glabel/endlabel): the decomp's reliable code, keyed by vram."""
    symtab = elf.get_section_by_name(".symtab")
    assert isinstance(symtab, SymbolTableSection)
    funcs: dict[int, tuple[str, int]] = {}
    for sym in symtab.iter_symbols():
        if sym["st_info"]["type"] != "STT_FUNC" or sym["st_size"] == 0 or "." in sym.name:
            continue
        vram = sym["st_value"]
        if start <= vram < end and vram % 4 == 0:
            funcs.setdefault(vram, (sym.name, sym["st_size"]))
    return funcs


def Symbol_Names(elf: ELFFile) -> dict[int, str]:
    """Any func_* name the decomp has for an address, used to label discovered functions."""
    symtab = elf.get_section_by_name(".symtab")
    assert isinstance(symtab, SymbolTableSection)
    names: dict[int, str] = {}
    for sym in symtab.iter_symbols():
        if sym.name.startswith("func_") and "." not in sym.name:
            names.setdefault(sym["st_value"], sym.name)
    return names


def Word(image: bytes, vram: int, addr: int) -> int:
    return int.from_bytes(image[addr - vram : addr - vram + 4], "big")


def Jal_Targets(image: bytes, vram: int, func_vram: int, size: int) -> set[int]:
    targets = set()
    for addr in range(func_vram, func_vram + size, 4):
        word = Word(image, vram, addr)
        if word >> 26 == 3:  # jal
            targets.add((addr & 0xF0000000) | ((word & 0x03FFFFFF) << 2))
    return targets


def Is_Prologue(word: int) -> bool:
    """addiu $sp, $sp, -N: how nearly every non-leaf function starts."""
    return word >> 16 == 0x27BD and word & 0x8000 != 0


def Address_References(image: bytes, vram: int) -> set[int]:
    """Addresses the program can form: lui/addiu (or ori) pairs, and raw data words."""
    refs = set()
    count = len(image) // 4
    for i in range(count):
        addr = vram + i * 4
        word = Word(image, vram, addr)
        refs.add(word)
        if word >> 26 != 0x0F:  # lui
            continue
        reg = (word >> 16) & 0x1F
        hi = (word & 0xFFFF) << 16
        for j in range(i + 1, min(i + 8, count)):
            lo_word = Word(image, vram, vram + j * 4)
            op = lo_word >> 26
            if op in (0x09, 0x0D) and (lo_word >> 21) & 0x1F == reg:  # addiu/ori rt, reg, lo
                lo = lo_word & 0xFFFF
                if op == 0x09 and lo & 0x8000:
                    lo -= 0x10000
                refs.add((hi + lo) & 0xFFFFFFFF)
    return refs


def Branch_Target(word: int, addr: int):
    """Target of a conditional/unconditional PC-relative branch, or None."""
    op = word >> 26
    rt = (word >> 16) & 0x1F
    offset = word & 0xFFFF
    if offset & 0x8000:
        offset -= 0x10000
    is_branch = (
        op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17)  # beq/bne/blez/bgtz (+likely)
        or (op == 0x01 and rt in (0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13))  # REGIMM
        or (op == 0x11 and ((word >> 21) & 0x1F) == 0x08)  # bc1x
    )
    # bltz/bltzl/bltzal/bltzall on $zero are never taken; bltzal $zero is the idiom for
    # loading the address of the data table that follows into $ra.
    rs = (word >> 21) & 0x1F
    if op == 0x01 and rs == 0 and rt in (0x00, 0x02, 0x10, 0x12):
        return None
    return addr + 4 + offset * 4 if is_branch else None


def Discovered_Size(image: bytes, vram: int, func_vram: int, limit: int) -> int:
    """Walk forward to the jr $ra that ends the function (no branch reaches past it)."""
    furthest = func_vram
    addr = func_vram
    while addr + 8 <= limit:
        word = Word(image, vram, addr)
        target = Branch_Target(word, addr)
        if target is not None and target > furthest:
            furthest = target
        if word == 0x03E00008 and addr >= furthest:  # jr $ra
            return addr + 8 - func_vram
        addr += 4
    return limit - func_vram


def Trim_Jump_Table(image: bytes, vram: int, func_vram: int, size: int) -> int:
    """Drop a jump table the decomp counted as the tail of a function.

    Its entries are 0xE0xxxxxx addresses, which decode as `sc rt, imm($zero)`;
    real code never stores-conditional to an absolute address.
    """
    while True:
        # Alignment padding may follow the table; only drop it if a table precedes it.
        end = size
        while end > 4 and Word(image, vram, func_vram + end - 4) == 0:
            end -= 4
        if end <= 4 or Word(image, vram, func_vram + end - 4) >> 24 != 0xE0:
            return size
        while end > 4 and Word(image, vram, func_vram + end - 4) >> 24 == 0xE0:
            end -= 4
        size = end


def Extend_Over_Branch_Targets(image: bytes, vram: int, funcs: dict) -> None:
    """Grow functions whose branches land past their end (the decomp split one function
    into several, or functions share an epilogue) to cover the function containing the
    target. N64Recomp recompiles each function from its own bytes, so overlaps are fine.
    """
    starts = sorted(funcs)
    changed = True
    while changed:
        changed = False
        for func_vram in starts:
            end = func_vram + funcs[func_vram][1]
            for addr in range(func_vram, end, 4):
                target = Branch_Target(Word(image, vram, addr), addr)
                if target is None or target < end:
                    continue
                container = next((v for v in starts if v <= target < v + funcs[v][1]), None)
                if container is None:
                    continue
                new_end = container + funcs[container][1]
                if new_end > end:
                    funcs[func_vram][1] = new_end - func_vram
                    end = new_end
                    changed = True


def Has_Exit(image: bytes, vram: int, func_vram: int, size: int) -> bool:
    """A function must be able to leave: jr, j, or an always-taken beq $zero, $zero."""
    if size < 8:
        return False
    for addr in range(func_vram, func_vram + size - 4, 4):
        word = Word(image, vram, addr)
        if word >> 26 == 0 and word & 0x3F == 0x08:  # jr
            return True
        if word >> 26 == 0x02:  # j
            return True
        if word >> 16 == 0x1000:  # beq $zero, $zero (b)
            return True
    return False


def Toml_Section(name: str, rom: int, vram: int, size: int, functions) -> str:
    lines = [
        "[[section]]",
        f'name = "{name}"',
        f"rom = 0x{rom:08X}",
        f"vram = 0x{vram:08X}",
        f"size = 0x{size:X}",
        "",
        "functions = [",
    ]
    lines += [f'    {{ name = "{n}", vram = 0x{v:08X}, size = 0x{s:X} }},' for n, v, s in functions]
    lines += ["]", ""]
    return "\n".join(lines)


def Main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("decomp", type=Path, help="path to the gauntlet-legends decomp checkout")
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--libultra", type=Path, help="n64sym output for the game binary")
    args = parser.parse_args()

    build = args.decomp / "build"
    rom = bytearray((args.decomp / "files" / "baserom.z64").read_bytes())
    toml_parts = []
    layout = []

    # Load every segment's image, its glabel functions and any names the decomp has.
    segments = []
    for name in ["boot"] + SEGMENTS:
        prefixes = BOOT_SECTIONS if name == "boot" else (".main",)
        with open(build / f"{name}.elf", "rb") as f:
            elf = ELFFile(f)
            vram, image = Loaded_Range(elf, prefixes)
            funcs = {v: [n, s] for v, (n, s) in Glabel_Functions(elf, vram, vram + len(image)).items()}
            names = Symbol_Names(elf)
        if name == "boot":
            assert rom[BOOT_ROM : BOOT_ROM + len(image)] == image, "boot.elf does not match the baserom"
        segments.append({"name": name, "vram": vram, "image": image, "funcs": funcs, "names": names})

    def Segment_For(addr):
        return next((seg for seg in segments if seg["vram"] <= addr < seg["vram"] + len(seg["image"])), None)

    def Add_Function(tseg, target, label):
        """Make target a function entry: split the function containing it, or discover a new one."""
        funcs = tseg["funcs"]
        container = next((v for v, (_, size) in funcs.items() if v < target < v + size), None)
        if container is not None:
            old_end = container + funcs[container][1]
            funcs[container][1] = target - container
            funcs[target] = [label, old_end - target]
            return "split"
        limit = min([v for v in funcs if v > target], default=tseg["vram"] + len(tseg["image"]))
        funcs[target] = [label, Discovered_Size(tseg["image"], tseg["vram"], target, limit)]
        return "discovered"

    # libultra names matter: N64Recomp swaps known libultra functions for the
    # runtime's native versions by name. Only trust unique matches inside game's
    # libultra block; small leaf functions elsewhere match signatures by accident.
    if args.libultra:
        game = next(seg for seg in segments if seg["name"] == "game")
        matches = [line.split() for line in args.libultra.read_text().splitlines() if line.strip()]
        counts = {}
        for _, n in matches:
            counts[n] = counts.get(n, 0) + 1
        applied = 0
        for addr_text, n in matches:
            addr = int(addr_text, 16)
            if counts[n] > 1 or not LIBULTRA_START <= addr < LIBULTRA_END:
                continue
            if re.search(r"_(text|data|bss|rodata)_[0-9A-F]{4}$", n) or n in LIBULTRA_NOT_CODE:
                continue
            n = LIBULTRA_KEEP.get(n, n)
            if addr in game["funcs"]:
                game["funcs"][addr][0] = n
            else:
                Add_Function(game, addr, n)
            applied += 1
        print(f"applied {applied} libultra names")

    # Function pointers: code the decomp labeled as data (e.g. thread entry points)
    # is only reached indirectly, so seed any referenced address that starts with a
    # stack-allocating prologue.
    pointers = 0
    for seg in segments:
        for ref in Address_References(seg["image"], seg["vram"]):
            tseg = Segment_For(ref)
            if tseg is None or ref % 4 or ref in tseg["funcs"]:
                continue
            if Is_Prologue(Word(tseg["image"], tseg["vram"], ref)):
                Add_Function(tseg, ref, tseg["names"].get(ref, f"func_{tseg['name']}_{ref:08X}"))
                pointers += 1
    print(f"discovered {pointers} functions from address references")

    # Follow jal targets from known code to a fixed point. Targets can land in the
    # middle of a function the decomp merged, or inside a .data section holding
    # code (overlays call 0xE0002640 there), so split or discover as needed.
    worklist = [(seg, v) for seg in segments for v in seg["funcs"]]
    split = discovered = 0
    while worklist:
        seg, func_vram = worklist.pop()
        for target in Jal_Targets(seg["image"], seg["vram"], func_vram, seg["funcs"][func_vram][1]):
            tseg = Segment_For(target)
            if tseg is None or target in tseg["funcs"] or target % 4:
                continue
            label = tseg["names"].get(target, f"func_{tseg['name']}_{target:08X}")
            if Add_Function(tseg, target, label) == "split":
                split += 1
            else:
                discovered += 1
            worklist.append((tseg, target))
    print(f"split {split} merged functions, discovered {discovered} unlabeled functions")

    for seg in segments:
        name, vram, image = seg["name"], seg["vram"], seg["image"]
        if name == "boot":
            rom_addr = BOOT_ROM
        else:
            rom_addr = Align(len(rom), ALIGN)
            rom.extend(bytes(rom_addr - len(rom)))
            rom.extend(image)
        for func_vram, entry in seg["funcs"].items():
            entry[1] = Trim_Jump_Table(image, vram, func_vram, entry[1])
        Extend_Over_Branch_Targets(image, vram, seg["funcs"])
        # Drop entries that can't be functions: data words the decomp labeled func_*,
        # or what's left of a trimmed jump table.
        funcs = sorted(
            ((n, v, s) for v, (n, s) in seg["funcs"].items() if Has_Exit(image, vram, v, s)),
            key=lambda f: f[1],
        )
        toml_parts.append(Toml_Section(name, rom_addr, vram, len(image), funcs))
        layout.append((name, rom_addr, vram, len(image), len(funcs)))

    # Boot enters the game through its kseg0 alias (jal 0x80002850, which is 0xE0002850
    # in TLB space). A jal can't encode the switch, so give the recompiler a trampoline
    # at the alias that does an indirect jump; the runtime resolves it via function lookup.
    trampoline_vram = 0x80002850
    trampoline = bytes.fromhex(
        "3C19E000"  # lui   $t9, 0xE000
        "37392850"  # ori   $t9, $t9, 0x2850
        "03200008"  # jr    $t9
        "00000000"  # nop
    )
    rom_addr = Align(len(rom), ALIGN)
    rom.extend(bytes(rom_addr - len(rom)))
    rom.extend(trampoline)
    funcs = [("game_entry_kseg0", trampoline_vram, len(trampoline))]
    toml_parts.append(Toml_Section("game_entry_kseg0", rom_addr, trampoline_vram, len(trampoline), funcs))
    layout.append(("game_entry_kseg0", rom_addr, trampoline_vram, len(trampoline), 1))

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "gauntlet.syms.toml").write_text("\n".join(toml_parts))
    (args.out_dir / "gauntlet.recomp.z64").write_bytes(rom)

    # Also emit a header so the runtime knows where each segment sits in the synthetic ROM.
    header = ["#pragma once", "// Generated by tools/elf2syms.py", ""]
    for name, rom_addr, vram, size, _ in layout:
        upper = name.upper()
        header.append(f"#define SEGMENT_{upper}_ROM  0x{rom_addr:08X}u")
        header.append(f"#define SEGMENT_{upper}_VRAM 0x{vram:08X}u")
        header.append(f"#define SEGMENT_{upper}_SIZE 0x{size:08X}u")
    (args.out_dir / "segments.h").write_text("\n".join(header) + "\n")

    for name, rom_addr, vram, size, count in layout:
        print(f"{name:12} rom 0x{rom_addr:08X} vram 0x{vram:08X} size 0x{size:06X} {count:5} funcs")
    return 0


if __name__ == "__main__":
    sys.exit(Main())
