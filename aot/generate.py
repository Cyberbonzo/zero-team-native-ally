"""Translate the Zero Team V30 program to position-independent x86-64.

Every byte address of the program ROM gets a 8-byte native entry that branches
to the precompiled implementation of the opcode stored at that address. The
decoding of "address -> opcode -> handler" happens here, at build time; the
generated code is derived from the owner's ROM and stays private to the build.

The handlers themselves are FBNeo's NEC core instruction bodies, compiled by the
same build with the same flags as the reference interpreter, so a translated
run and an interpreted run can be compared frame by frame.
"""
import argparse
import hashlib
import json
import re
import struct
import zipfile
import zlib
from pathlib import Path

ROM_BASE = 0x20000
ROM_END = 0x100000
SITE_SIZE = 8
VENEER_SIZE = 16

# zeroteamRomDesc load order in ZeroteamInit: four 256 KiB program chips are
# interleaved 4-way, in this byte-lane order.
CPU_CHIPS = (
    (0, 0x25AA5BA4, 0),  # seibu__1 -> lanes 0
    (1, 0xEC79A12B, 2),  # seibu__3 -> lanes 2
    (2, 0x54F3D359, 1),  # seibu__2 -> lanes 1
    (3, 0xA017B8D0, 3),  # seibu__4 -> lanes 3
)


def read_chips(path):
    """Return {crc32: bytes} for a chip directory or an arcade ZIP."""
    chips = {}
    if path.is_dir():
        for item in sorted(path.iterdir()):
            if item.is_file() and item.stat().st_size:
                data = item.read_bytes()
                chips[zlib.crc32(data)] = data
        return chips
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            data = archive.read(info)
            chips[zlib.crc32(data)] = data
    return chips


def build_program(chips):
    image = bytearray(0x100000)
    for _, crc, lane in CPU_CHIPS:
        if crc not in chips:
            raise ValueError(f"missing program chip {crc:08x}")
        data = chips[crc]
        if len(data) != 0x40000:
            raise ValueError(f"chip {crc:08x} has {len(data)} bytes, expected 262144")
        image[lane::4] = data
    return bytes(image)


def parse_handlers(necinstr_h):
    """Map opcode byte -> FBNeo handler name, from the 256-entry table."""
    source = necinstr_h.read_text()
    start = source.index("static void (*const nec_instruction[256]")
    block = source[start:source.index("};", start)]
    names = re.findall(r"(i[_a-z0-9]+)\s*,?\s*/\*\s*0x([0-9a-f]{2})\s*\*/", block)
    table = {}
    for name, opcode in names:
        table[int(opcode, 16)] = name
    if len(table) != 256:
        raise ValueError(f"decoded {len(table)} of 256 opcode handlers")
    return table


def write_sites(program, table, out):
    """Emit the site blob and the assembly that holds it plus its veneers."""
    count = ROM_END - ROM_BASE
    site_bytes = bytearray(count * SITE_SIZE)
    veneer_base = count * SITE_SIZE
    used = sorted({program[addr] for addr in range(ROM_BASE, ROM_END)})
    # Every distinct opcode present in the program gets one indirect veneer;
    # a site is a direct relative jump to it, so no per-site relocation is
    # needed and the blob stays a plain binary that assembles in one pass.
    slot = {opcode: index for index, opcode in enumerate(used)}
    for addr in range(ROM_BASE, ROM_END):
        opcode = program[addr]
        at = (addr - ROM_BASE) * SITE_SIZE
        target = veneer_base + slot[opcode] * VENEER_SIZE
        displacement = target - (at + 5)
        struct.pack_into("<BiB", site_bytes, at, 0xE9, displacement, 0x90)
        site_bytes[at + 6:at + 8] = b"\x90\x90"
    (out / "sites.bin").write_bytes(site_bytes)

    assembly = [
        # The translated entries are executed, so they live in .text.
        '.section .text,"ax",@progbits',
        ".p2align 4",
        ".globl zt_sites",
        ".hidden zt_sites",
        ".type zt_sites,@object",
        "zt_sites:",
        f'.incbin "{out.as_posix()}/sites.bin"',
        ".p2align 4",
        ".globl zt_veneers",
        ".hidden zt_veneers",
        "zt_veneers:",
    ]
    for opcode in used:
        # jmp *zt_handlers+8*N(%rip)  ->  ff 25 <disp32>, padded to 16 bytes
        assembly += [".byte 0xff,0x25",
                     f".long zt_handlers + {opcode * 8} - . - 4",
                     ".fill 10,1,0x90"]
    assembly.append('.section .note.GNU-stack,"",@progbits')
    (out / "zeroteam_sites.S").write_text("\n".join(assembly) + "\n")
    return used, count


def write_dispatch(table, used, out):
    entries = []
    for opcode in range(256):
        name = table[opcode]
        entries.append(f"    (const void *)(void (*)(void)){name},")
    body = f"""// Generated: V30 opcode handlers and the translated dispatch.
// Included by nec_aot.cpp, where the handler bodies are in scope.
extern "C" {{
extern const unsigned char zt_sites[];
}}

const void *zt_handlers[256] = {{
{chr(10).join(entries)}
}};

static unsigned long zt_fallback_hits;

extern "C" __attribute__((visibility("default"))) unsigned zt_aot_sites(void) {{ return {ROM_END - ROM_BASE}u; }}
extern "C" __attribute__((visibility("default"))) unsigned zt_aot_fallback(void) {{ return (unsigned)zt_fallback_hits; }}

// Replaces "NEC_OPTABLE[fetchop(nec_state)](nec_state)": the opcode at the
// current CS:IP has already been decoded at build time, so the fetch becomes a
// direct entry into the translated code that implements it.
static void zt_aot_dispatch(nec_state_t *nec_state)
{{
    prefetch(nec_state);
    unsigned linear = ((Sreg(PS) << 4) + nec_state->ip) & 0xfffff;
    nec_state->ip++;
    const unsigned offset = linear - {ROM_BASE}u;
    if (offset < {ROM_END - ROM_BASE}u) {{
        ((void (*)(nec_state_t *))(zt_sites + (size_t)offset * {SITE_SIZE}))(nec_state);
    }} else {{
        // Program RAM and the reset window are not part of the translation.
        zt_fallback_hits++;
        nec_instruction[cpu_readop(linear ^ nec_state->fetch_xor)](nec_state);
    }}
}}
"""
    (out / "zt_dispatch.inc").write_text(body)
    return used


def patch_core(nec_cpp, out):
    source = nec_cpp.read_text()
    include_anchor = '#include "necmodrm.h"\n'
    if source.count(include_anchor) != 1:
        raise ValueError("unexpected NEC core include block")
    source = source.replace(include_anchor, include_anchor + '\n#include "zt_dispatch.inc"\n')
    call = "NEC_OPTABLE[fetchop(nec_state)](nec_state);"
    if source.count(call) != 2:
        raise ValueError("unexpected NEC opcode dispatch sites")
    source = source.replace(call, "zt_aot_dispatch(nec_state);")
    (out / "nec_aot.cpp").write_text(source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fbneo", type=Path, required=True)
    parser.add_argument("--chips", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--expect", help="expected SHA-256 of the program image")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    chips = read_chips(args.chips)
    program = build_program(chips)
    digest = hashlib.sha256(program).hexdigest()
    if args.expect and digest != args.expect:
        raise SystemExit(f"program image {digest} != expected {args.expect}")

    table = parse_handlers(args.fbneo / "src/cpu/nec/necinstr.h")
    used, count = write_sites(program, table, args.out)
    write_dispatch(table, used, args.out)
    patch_core(args.fbneo / "src/cpu/nec/nec.cpp", args.out)

    reset_ip, reset_cs = struct.unpack_from("<HH", program, 0xFFFF0)
    stats = {
        "program_sha256": digest,
        "program_bytes": len(program),
        "translated_addresses": count,
        "address_range": [hex(ROM_BASE), hex(ROM_END)],
        "distinct_opcodes": len(used),
        "opcodes": [f"{op:02x}:{table[op]}" for op in used],
        "reset_vector": {"cs": f"{reset_cs:04x}", "ip": f"{reset_ip:04x}"},
        "site_bytes": count * SITE_SIZE,
    }
    (args.out / "translation.json").write_text(json.dumps(stats, indent=2) + "\n")
    print(json.dumps({k: stats[k] for k in
                      ("program_sha256", "translated_addresses", "distinct_opcodes",
                       "reset_vector", "site_bytes")}))


if __name__ == "__main__":
    main()
