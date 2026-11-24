"""Test helper: compile real Cortex-M3 soft-float -fshort-enums C snippets into pebble-app.bin."""

from __future__ import annotations

import pathlib
import struct
import subprocess
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
LIBPEBBLE = REPO_ROOT / "third_party" / "pebble_sdk" / "libpebble.a"

SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2


def _parse_elf32(data: bytes) -> tuple[list[dict[str, object]], dict[str, int]]:
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("expected 32-bit little-endian ELF")

    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)

    sections: list[dict[str, object]] = []
    for i in range(shnum):
        o = shoff + i * shentsize
        name_off, sh_type, flags, addr, off, size, link = struct.unpack_from("<7I", data, o)
        sections.append(
            {
                "name_off": name_off,
                "type": sh_type,
                "flags": flags,
                "addr": addr,
                "off": off,
                "size": size,
                "link": link,
            }
        )

    def read_str(strtab: dict[str, object], offset: int) -> str:
        start = int(strtab["off"]) + offset
        end = data.index(b"\x00", start)
        return data[start:end].decode("ascii")

    shstr = sections[shstrndx]
    for sec in sections:
        sec["name"] = read_str(shstr, int(sec["name_off"]))

    symbols: dict[str, int] = {}
    for sec in sections:
        if sec["type"] != SHT_SYMTAB:
            continue
        strtab = sections[int(sec["link"])]
        off = int(sec["off"])
        end = off + int(sec["size"])
        for pos in range(off, end, 16):
            st_name, st_value, _st_size, st_info, _st_other, st_shndx = struct.unpack_from(
                "<IIIBBH", data, pos
            )
            if st_shndx == 0:
                continue
            name = read_str(strtab, st_name)
            is_func = (st_info & 0x0F) == 2
            symbols[name] = (st_value & ~1) if is_func else st_value

    return sections, symbols


def build_pebble_app_bin(
    c_source: str,
    *,
    struct_version_major: int = 0x10,
    app_name: str = "TestFixture",
    company_name: str = "HaloAtlas",
) -> bytes:
    """Compile C source as Cortex-M3 soft-float -fshort-enums linked with libpebble.a."""
    if struct_version_major == 0x10:
        header_size = 0x82
    elif struct_version_major == 0x09:
        header_size = 0x80
    elif struct_version_major == 0x08:
        header_size = 0x7C
    else:
        raise ValueError(f"unsupported test struct_version_major={struct_version_major:#x}")

    linker_script = f"""
ENTRY(main)
SECTIONS {{
    . = 0x0;
    .pbl_header : {{
        . = . + {header_size};
    }}
    .text : ALIGN(4) {{
        KEEP(*(.text.jump_to_pbl_function))
        *(.text .text.*)
        *(.rodata .rodata.*)
    }}
    .data : ALIGN(4) {{
        *(.data .data.*)
    }}
    .got : ALIGN(4) {{
        *(.got .got.*)
    }}
    .bss : ALIGN(4) {{
        *(.bss .bss.* COMMON)
    }}
}}
"""
    clang_res = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip()
    freestanding_inc = pathlib.Path(clang_res) / "include"

    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        src_path = d / "app.c"
        obj_path = d / "app.o"
        ld_path = d / "app.ld"
        elf_path = d / "app.elf"
        src_path.write_text(c_source, encoding="utf-8")
        ld_path.write_text(linker_script, encoding="utf-8")

        subprocess.run(
            [
                "clang",
                "--target=arm-none-eabi",
                "-mcpu=cortex-m3",
                "-mthumb",
                "-mfloat-abi=soft",
                "-fshort-enums",
                "-fPIE",
                "-ffunction-sections",
                "-fdata-sections",
                "-ffreestanding",
                "-fno-stack-protector",
                "-fno-builtin",
                "-nostdinc",
                "-isystem",
                str(freestanding_inc),
                "-O2",
                "-c",
                str(src_path),
                "-o",
                str(obj_path),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

        subprocess.run(
            [
                "ld.lld",
                "-T",
                str(ld_path),
                "--gc-sections",
                "--emit-relocs",
                str(obj_path),
                str(LIBPEBBLE),
                "-o",
                str(elf_path),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

        elf = elf_path.read_bytes()

    sections, symbols = _parse_elf32(elf)
    if "main" not in symbols:
        raise RuntimeError("compiled test ELF missing 'main' symbol")
    if "pbl_table_addr" not in symbols:
        raise RuntimeError("compiled test ELF missing 'pbl_table_addr' symbol (must call a PebbleOS syscall)")

    # Build loadable image (all SHF_ALLOC non-NOBITS sections, starting at 0x0)
    load_end = header_size
    virt_end = header_size
    for sec in sections:
        flags = int(sec["flags"])
        size = int(sec["size"])
        addr = int(sec["addr"])
        if not (flags & SHF_ALLOC) or size == 0:
            continue
        virt_end = max(virt_end, addr + size)
        if sec["type"] != SHT_NOBITS:
            load_end = max(load_end, addr + size)

    # Align load_size to 4 bytes so appended relocation table is word-aligned
    load_size = (load_end + 3) & ~3
    virt_size = max(virt_end, load_size)

    image = bytearray(load_size)
    for sec in sections:
        flags = int(sec["flags"])
        size = int(sec["size"])
        addr = int(sec["addr"])
        off = int(sec["off"])
        if (flags & SHF_ALLOC) and sec["type"] != SHT_NOBITS and size > 0:
            image[addr : addr + size] = elf[off : off + size]

    # Collect 32-bit pointer relocations in .data* and .got
    relocs: list[int] = []
    for sec in sections:
        name = str(sec["name"])
        if sec["type"] == SHT_REL and name.startswith(".rel.data"):
            off = int(sec["off"])
            end = off + int(sec["size"])
            for pos in range(off, end, 8):
                r_offset, r_info = struct.unpack_from("<II", elf, pos)
                r_type = r_info & 0xFF
                if r_type == 2:  # R_ARM_ABS32
                    relocs.append(r_offset)
        elif name == ".got":
            addr = int(sec["addr"])
            size = int(sec["size"])
            for a in range(addr, addr + size, 4):
                relocs.append(a)

    # Populate PebbleProcessInfo header
    image[0:8] = b"PBLAPP\x00\x00"
    image[8] = struct_version_major
    image[9] = 0
    image[10] = 5  # sdk major
    image[11] = 72  # sdk minor
    image[12] = 1
    image[13] = 0
    struct.pack_into("<H", image, 0x0E, load_size)
    struct.pack_into("<I", image, 0x10, symbols["main"])
    name_bytes = app_name.encode("ascii")[:31]
    company_bytes = company_name.encode("ascii")[:31]
    image[0x18 : 0x18 + len(name_bytes)] = name_bytes
    image[0x38 : 0x38 + len(company_bytes)] = company_bytes
    struct.pack_into("<I", image, 0x5C, symbols["pbl_table_addr"])
    struct.pack_into("<I", image, 0x60, 0)  # flags

    if struct_version_major == 0x10:
        struct.pack_into("<I", image, 0x64, len(relocs))
        struct.pack_into("<H", image, 0x80, virt_size)
    elif struct_version_major == 0x09:
        struct.pack_into("<I", image, 0x64, len(relocs))
    elif struct_version_major == 0x08:
        struct.pack_into("<I", image, 0x64, load_size)  # reloc_list_start
        struct.pack_into("<I", image, 0x68, len(relocs))

    for r in relocs:
        image.extend(struct.pack("<I", r))

    return bytes(image)
