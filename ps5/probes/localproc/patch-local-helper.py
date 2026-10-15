#!/usr/bin/env python3
"""Rewrite the probe child's app process parameters to the PS5 local-helper layout.

The layout comes from firmware 5.50's NKUIProcess.self, NKNetworkProcess.self,
NKWebProcess.self and webrtc_daemon.self. It is deliberately probe-local until
that profile has run successfully on the console.
"""

import argparse
import hashlib
import struct
from pathlib import Path

PT_LOAD = 1
PT_DYNAMIC = 2
PT_SCE_PROCPARAM = 0x61000001
PT_GNU_RELRO = 0x6474E552
DT_NULL = 0
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_RELACOUNT = 0x6FFFFFF9
DT_SCE_SYMTABSZ = 0x6100003F
R_X86_64_64 = 1
R_X86_64_RELATIVE = 8
STT_OBJECT = 1
NEED_LIBC_NID = "ZT4ODD2Ts9o#"
LOCAL_COMPANION_SDK = 0x09690001
LOCAL_MODULE_SDK = 0x05500008
TITLE_COMPANION_SDK = 0x08050001
TITLE_MODULE_SDK = 0x02000009
FSELF_MAGIC = 0x1D3D154F
APP_PROGRAM_TYPE = 0x00000101
LOCAL_PROGRAM_TYPE = 0x10000301



def require(condition, message):
    if not condition:
        raise SystemExit(f"local-helper patch: {message}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--container", action="store_true",
                        help="rewrite a signed probe FSELF's program classification")
    parser.add_argument("--sdk", choices=("local", "title"), default="local",
                        help="process SDK fields: 'local' copies 5.50's firmware helpers "
                             "(0x09690001/0x05500008); 'title' uses the title eboot's "
                             "(0x08050001/0x02000009)")
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    data = bytearray(args.input.read_bytes())
    if args.container:
        require(len(data) >= 0x20 and struct.unpack_from("<I", data, 0)[0] == FSELF_MAGIC,
                "input is not the probe's FSELF format")
        require(struct.unpack_from("<I", data, 8)[0] == APP_PROGRAM_TYPE,
                "unexpected input program type")
        struct.pack_into("<I", data, 8, LOCAL_PROGRAM_TYPE)
        args.output.write_bytes(data)
        print(f"local-helper container: {args.output} "
              f"sha256 {hashlib.sha256(data).hexdigest()}")
        return

    require(data[:4] == b"\x7fELF" and data[4:6] == b"\x02\x01",
            "input is not little-endian ELF64")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    require(phentsize == 56, "unexpected program-header size")
    headers = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize) for i in range(phnum)]

    def header(kind):
        matches = [entry for entry in headers if entry[0] == kind]
        require(len(matches) == 1, f"expected one {kind:#x} program header")
        return matches[0]

    def file_offset(address, size=1):
        for kind, flags, offset, virtual, physical, file_size, memory_size, alignment in headers:
            del flags, physical, memory_size, alignment
            if kind == PT_LOAD and virtual <= address and address + size <= virtual + file_size:
                return offset + address - virtual
        raise SystemExit(f"local-helper patch: address {address:#x}+{size:#x} is not file-backed")

    proc = header(PT_SCE_PROCPARAM)
    require(proc[5] == 0x60, "unexpected process-parameter size")
    proc_file, proc_address = proc[2], proc[3]
    require(data[proc_file + 8:proc_file + 12] == b"ORBI", "unexpected process-parameter magic")

    dynamic = header(PT_DYNAMIC)
    tags = {}
    for offset in range(0, dynamic[5], 16):
        tag, value = struct.unpack_from("<qQ", data, dynamic[2] + offset)
        tags[tag] = value
        if tag == DT_NULL:
            break
    require(tags.get(DT_RELAENT) == 24, "unexpected RELA entry size")
    require(tags.get(DT_SYMENT) == 24, "unexpected dynamic-symbol entry size")
    required_tags = (DT_RELA, DT_RELASZ, DT_RELACOUNT, DT_SYMTAB, DT_STRTAB, DT_STRSZ,
                     DT_SCE_SYMTABSZ)
    require(all(tag in tags for tag in required_tags), "missing dynamic metadata")
    rela_file = file_offset(tags[DT_RELA], tags[DT_RELASZ])
    rela_count = tags[DT_RELASZ] // 24
    relative_count = tags[DT_RELACOUNT]
    require(relative_count <= rela_count, "RELACOUNT exceeds the RELA table")

    relocations = []
    relatives = {}
    for index in range(rela_count):
        offset, info, addend = struct.unpack_from("<QQq", data, rela_file + index * 24)
        relocations.append((offset, info, addend))
        if (info & 0xFFFFFFFF) == R_X86_64_RELATIVE:
            require(offset not in relatives, f"duplicate relative relocation at {offset:#x}")
            relatives[offset] = (index, info, addend)

    def relative(offset):
        require(offset in relatives, f"missing relative relocation at {offset:#x}")
        return relatives[offset]
    require(tags[DT_SCE_SYMTABSZ] % 24 == 0, "unaligned dynamic-symbol table")
    symbol_file = file_offset(tags[DT_SYMTAB], tags[DT_SCE_SYMTABSZ])
    string_file = file_offset(tags[DT_STRTAB], tags[DT_STRSZ])
    symbols = []
    for index in range(tags[DT_SCE_SYMTABSZ] // 24):
        name_offset, info, other, section, value, size = struct.unpack_from(
            "<IBBHQQ", data, symbol_file + index * 24)
        del other, value, size
        require(name_offset < tags[DT_STRSZ], f"symbol {index} has an invalid name")
        end = data.find(0, string_file + name_offset, string_file + tags[DT_STRSZ])
        require(end >= 0, f"symbol {index} has an unterminated name")
        name = data[string_file + name_offset:end].decode("ascii")
        symbols.append((name, info, section))
    need_symbols = [(index, info, section) for index, (name, info, section) in enumerate(symbols)
                    if name.startswith(NEED_LIBC_NID)]
    require(len(need_symbols) == 1, "expected one Need_sceLibcInternal import")
    need_index, need_info, need_section = need_symbols[0]
    require((need_info & 0x0F) == STT_OBJECT and need_section == 0,
            "Need_sceLibcInternal is not an undefined object")
    need_relocations = [
        (index, offset, info, addend)
        for index, (offset, info, addend) in enumerate(relocations)
        if (info >> 32) == need_index and (info & 0xFFFFFFFF) == R_X86_64_64
    ]
    require(len(need_relocations) == 1, "expected one Need_sceLibcInternal relocation")
    need_relocation = need_relocations[0]
    require(need_relocation[0] >= relative_count and need_relocation[3] == 0,
            "unexpected Need_sceLibcInternal relocation")


    block0 = relative(proc_address + 0x38)[2]
    block1 = relative(proc_address + 0x40)[2]
    block2 = relative(proc_address + 0x48)[2]
    require(block1 == block0 + 0xA8 and block2 == block0 + 0xE0, "unexpected root block layout")
    require(struct.unpack_from("<Q", data, file_offset(block0, 8))[0] == 0xA8, "unexpected block 0")
    require(struct.unpack_from("<Q", data, file_offset(block1, 8))[0] == 0x38, "unexpected block 1")
    require(struct.unpack_from("<Q", data, file_offset(block2, 8))[0] == 0x10, "unexpected app block 2")

    nested_offsets = (0x10, 0x20, 0x30, 0x38, 0x60)
    nested = {offset: relative(block0 + offset) for offset in nested_offsets}
    require(struct.unpack_from("<Q", data, file_offset(nested[0x30][2], 8))[0] == 0x78,
            "unexpected block 3")
    require(struct.unpack_from("<Q", data, file_offset(nested[0x38][2], 8))[0] == 0xC0,
            "unexpected block 4")
    require(struct.unpack_from("<Q", data, file_offset(nested[0x60][2], 8))[0] == 0x38,
            "unexpected app-only block 5")

    relro = header(PT_GNU_RELRO)
    local_end = block0 + 0x248
    require(relro[3] <= block0 and local_end <= relro[3] + relro[6], "local blocks exceed RELRO")
    blocks_file = file_offset(block0, 0x248)
    data[blocks_file:blocks_file + 0x248] = bytes(0x248)
    local_blocks = ((0x000, 0xA8, 0x0E),
                    (0x0A8, 0x38, 0),
                    (0x0E0, 0x20, 0),
                    (0x100, 0x78, 2),
                    (0x178, 0xC0, 3))
    for offset, size, count in local_blocks:
        struct.pack_into("<Q", data, blocks_file + offset, size)
        if count:
            struct.pack_into("<Q", data, blocks_file + offset + 8, count)
    struct.pack_into("<Q", data, blocks_file + 0x238, 0xFFFFFFFFFFFFFFFF)
    struct.pack_into("<I", data, blocks_file + 0x240, 1)

    def patch_relative(old_offset, new_offset, addend):
        index, info, old_addend = relative(old_offset)
        del old_addend
        struct.pack_into("<QQq", data, rela_file + index * 24, new_offset, info, addend)

    patch_relative(block0 + 0x10, block0 + 0x10, block0 + 0x238)
    patch_relative(block0 + 0x20, block0 + 0x20, block0 + 0x240)
    patch_relative(block0 + 0x30, block0 + 0x30, block0 + 0x100)
    patch_relative(block0 + 0x38, block0 + 0x38, block0 + 0x178)
    # Firmware local helpers leave block0+0x60 null. Preserve RELACOUNT by
    # turning the app-only relocation into a harmless duplicate of block 3.
    patch_relative(block0 + 0x60, block0 + 0x30, block0 + 0x100)
    need_relocation_file = rela_file + need_relocation[0] * 24
    struct.pack_into("<QQq", data, need_relocation_file, block0 + 0x48,
                     need_relocation[2], 0)
    patched_targets = [
        struct.unpack_from("<Q", data, rela_file + index * 24)[0]
        for index in range(rela_count)
    ]
    expected_targets = [block0 + 0x10, block0 + 0x20, block0 + 0x30,
                        block0 + 0x30, block0 + 0x38, block0 + 0x48]
    actual_targets = sorted(target for target in patched_targets if block0 <= target < local_end)
    require(actual_targets == sorted(expected_targets), "unexpected relocation in local blocks")


    companion, module = ((LOCAL_COMPANION_SDK, LOCAL_MODULE_SDK) if args.sdk == "local"
                         else (TITLE_COMPANION_SDK, TITLE_MODULE_SDK))
    struct.pack_into("<I", data, proc_file + 0x10, companion)
    struct.pack_into("<I", data, proc_file + 0x14, module)
    args.output.write_bytes(data)
    print(f"local-helper: {args.output} sha256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
