#!/usr/bin/env python3
"""Build the single-title Zero Team board from the pinned FBNeo sources.

Only the files this PCB needs are compiled. The driver keeps its other game
selections in dead code; Zero Team's Init pins `game_select = 2`.

Two CPU builds are possible and must stay interchangeable, because the build
compares them frame by frame:

  * reference   - FBNeo's NEC core driven by its opcode dispatch table.
  * --aot       - the generated translation: one native entry per program byte
                  address, built by aot/generate.py from the player's chips.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get("ZEROTEAM_WORK", HERE.parent.parent)).resolve()
FB = ROOT / "vendor/FBNeo"
SRC = FB / "src"
BUILD = ROOT / "build"
GEN = BUILD / "generated"
OBJ = BUILD / "board-obj"

SOURCE_FILES = """
burn/burn.cpp burn/burn_memory.cpp burn/tiles_generic.cpp burn/tilemap_generic.cpp
burn/burn_bitmap.cpp burn/debug_track.cpp burn/burn_sound.cpp burn/burn_sound_c.cpp
burn/load.cpp burn/timer.cpp burn/devices/eeprom.cpp
burn/snd/burn_ym2151.cpp burn/snd/ym2151.c burn/snd/msm6295.cpp
burn/snd/burn_ym2203.cpp burn/snd/burn_ym3812.cpp burn/snd/fm.c burn/snd/fmopl.c
burn/snd/ay8910.c burn/snd/ymdeltat.c
cpu/z80_intf.cpp cpu/z80/z80.cpp cpu/z80/z80daisy.cpp cpu/z80/z80ctc.cpp
cpu/z80/z80pio.cpp cpu/nec_intf.cpp
burn/devices/seibucop.cpp burn/devices/seibusnd.cpp
""".split()

# The V25 core is only reachable through the shared Vez interface, which the
# driver's type switch keeps, so its symbols are needed even on a V30 board.
CPU_FILES = ["cpu/nec/nec.cpp", "cpu/nec/v25.cpp"]

INCLUDES = [
    SRC / "burn", SRC / "burn/devices", SRC / "burn/snd", SRC / "cpu",
    SRC / "cpu/nec", SRC / "cpu/z80", SRC / "burner/headless", SRC / "burner",
    SRC / "intf", SRC / "burn/drv/pst90s", GEN, HERE.parent / "frontend",
]

FLAGS = [
    "-O2", "-fPIC", "-fvisibility=hidden", "-ffunction-sections",
    "-fdata-sections", "-fno-strict-aliasing", "-Wno-write-strings",
    "-Wno-narrowing", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
    "-Wno-stringop-overflow", "-Wno-array-bounds",
    "-DLSB_FIRST", "-DBUILD_HEADLESS", "-DBUILD_X64_EXE", "-DNO_VIZ",
    "-DHAVE_UNISTD_H",
    # FBNeo's CPU interfaces define this for GCC when the FASTCALL build option
    # is off; the Z80 core includes z80.h without going through the interface.
    "-D__fastcall=",
]


def update(path, data):
    if not path.exists() or path.read_text() != data:
        path.write_text(data)


def compile_object(source, extra=()):
    cpp = source.suffix in (".cpp", ".cc")
    compiler = os.environ.get("CXX", "g++") if cpp else os.environ.get("CC", "gcc")
    std = "-std=gnu++17" if cpp else "-std=gnu17"
    out = OBJ / (source.name + (".aot" if "-DZEROTEAM_AOT" in extra else "") + ".o")
    cmd = [compiler, std, *FLAGS] + [f"-I{x}" for x in INCLUDES]
    cmd += [*extra, "-c", str(source), "-o", str(out)]
    subprocess.run(cmd, check=True)
    return out


def assemble(source):
    out = OBJ / (source.name + ".o")
    subprocess.run(["gcc", "-fPIC", "-c", str(source), "-o", str(out)], check=True)
    return out


WIDESCREEN = True


def patch_widescreen(code, margin=54):
    """Widen the Zero Team renderer from 320 to 320 + 2*margin columns.

    The driver already walks the full 512px tile maps and the 512px sprite
    space, and clips to nScreenWidth. So a 16:9 view needs a wider surface, the
    extra columns on both sides (320 + 2*54 = 428), and the fixed text layer
    kept to its original 40 columns so the HUD stays centred.

    The shift is constant rather than clamped at the level edge: the game also
    composes fixed 320-wide screens (title, character select) out of sprites, and
    a centred shift keeps those centred. Clamping would leave them flush left.
    """
    replacements = [
        # Surface allocation for the 32bpp composition buffer.
        ("Next += 320 * 256 * sizeof(UINT32);",
         "Next += ZT_SCREEN_WIDTH * ZT_SCREEN_HEIGHT * sizeof(UINT32);"),
        # Tile layers: wrap the 512px world at the new left edge, then shift.
        ("if (sx < -15) sx += 512;",
         f"if (sx < -15 - {margin}) sx += 512;\n\t\tsx += {margin};"),
        # Fixed text layer: keep the original 40 columns, centred in the view.
        ("INT32 sx = (offs & 0x3f) * 8;",
         f"if ((offs & 0x3f) >= 40) continue;\n"
         f"\t\tINT32 sx = ((offs & 0x3f) * 8) + {margin};"),
        # Sprites live in the same space as the tile maps and the fixed screens.
        ("if (sx < -15 || sy < -15 || sy >= nScreenHeight || sx >= nScreenWidth) return;",
         f"sx += {margin};\n\tif (sx < -15 || sy < -15 || sy >= nScreenHeight ||"
         f" sx >= nScreenWidth) return;"),
    ]
    for before, after in replacements:
        if code.count(before) != 1:
            raise SystemExit(f"unexpected driver source for widescreen patch: {before!r}")
        code = code.replace(before, after)
    # The driver's visible size drives GenericTilesInit and the composition
    # loops, so the single supported title is declared at the wider geometry.
    marker = "struct BurnDriver BurnDrvZeroteam = {"
    start = code.index(marker)
    end = code.index("\n};", start)
    block = code[start:end]
    if block.count("\t320, 256, 4, 3") != 1:
        raise SystemExit("unexpected BurnDrvZeroteam geometry")
    block = block.replace("\t320, 256, 4, 3",
                          "\tZT_SCREEN_WIDTH, ZT_SCREEN_HEIGHT, 16, 9")
    code = code[:start] + block + code[end:]
    return code


def patch_scroll_trace(code):
    """Log every scroll-register write with the guest PC that issued it.

    Diagnostic only: it turns the camera routine into a handful of addresses
    that can be disassembled, instead of guessing from a linear sweep of a
    variable-length instruction set.
    """
    marker = "\t\tscroll[offset & 0x0f] = data;\n"
    if code.count(marker) != 1:
        raise SystemExit("unexpected scroll write site")
    code = code.replace(marker, marker +
                        "\t\tstatic int trace = -1;\n"
                        "\t\tif (trace < 0) trace = getenv(\"ZT_TRACE_SCR\") ? 1 : 0;\n"
                        "\t\tif (trace)\n"
                        "\t\t\tfprintf(stderr, \"SCR reg=%d val=%04x pc=%05x\\n\",\n"
                        "\t\t\t        offset & 0x0f, data, VezGetPC(-1) & 0xfffff);\n")
    return code


def patch_store_watch(code, address):
    """Route writes to one RAM word through the handler and log the store PC.

    Sampling after an instruction cannot tell which store changed memory, and
    the translated dispatch runs one guest instruction at a time, so the write
    handler is the only place where the guest PC is exact.
    """
    marker = ("static void __fastcall zeroteam_main_write(UINT32 address, UINT8 data)\n"
              "{\n")
    if code.count(marker) != 1:
        raise SystemExit("unexpected zeroteam_main_write definition")
    hook = (marker +
            f"\tif (address == {address:#x} || address == {address + 1:#x}) {{\n"
            "\t\tstatic int trace = -1;\n"
            "\t\tif (trace < 0) trace = getenv(\"ZT_WATCH_STORE\") ? 1 : 0;\n"
            "\t\tif (trace)\n"
            "\t\t\tfprintf(stderr, \"WS pc=%05x a=%03x v=%02x\\n\",\n"
            "\t\t\t        VezGetPC(-1) & 0xfffff, address, data);\n"
            "\t}\n")
    code = code.replace(marker, hook)
    map_marker = "\tVezSetWriteHandler(zeroteam_main_write);"
    if code.count(map_marker) != 1:
        raise SystemExit("unexpected write handler registration")
    page_start = address & ~0x1ff
    page_end = (address | 0x1ff)
    code = code.replace(map_marker, map_marker +
                        f"\n\tVezMemCallback({page_start:#x}, {page_end:#x}, 1);"
                        " // watch one word through the handler")
    return code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--aot", action="store_true",
                        help="link the generated V30 translation instead of the interpreter")
    parser.add_argument("--lib", default="libzeroteam_board.so")
    parser.add_argument("--trace-scroll", action="store_true",
                        help="diagnostic: log scroll writes with the guest PC")
    parser.add_argument("--watch-store", metavar="ADDR",
                        help="diagnostic: route writes to one RAM word through the handler")
    args = parser.parse_args()

    OBJ.mkdir(exist_ok=True, parents=True)
    GEN.mkdir(exist_ok=True, parents=True)

    update(GEN / "driverlist.h", """extern struct BurnDriver BurnDrvZeroteam;
static struct BurnDriver *const pDriver[] = { &BurnDrvZeroteam };
static const struct {const char *game_name; char *sourcefile;} sourcefile_table[] = {
    {"zeroteam", (char*)"d_raiden2.cpp"}, {"", (char*)""}};
""")

    # The driver keeps every Seibu title of the family in one file. The board
    # only needs the Zero Team paths; a private copy exposes the program image
    # so the build can prove that the generated translation matches the bytes
    # the driver assembles from the chips, and carries the 16:9 geometry.
    header = (HERE.parent / "frontend/zeroteam_board.h").read_text()
    width = int(re.search(r"ZT_WIDTH = (\d+)", header).group(1))
    height = int(re.search(r"ZT_HEIGHT = (\d+)", header).group(1))
    margin = int(re.search(r"ZT_WIDE_MARGIN = (\d+)", header).group(1))
    if width != 320 + 2 * margin:
        raise SystemExit(f"ZT_WIDTH {width} is not 320 plus two {margin}-column margins")
    driver = (SRC / "burn/drv/pst90s/d_raiden2.cpp").read_text()
    if WIDESCREEN:
        driver = patch_widescreen(driver, margin)
    if args.trace_scroll:
        driver = patch_scroll_trace(driver)
    if args.watch_store:
        driver = patch_store_watch(driver, int(args.watch_store, 16))
    geometry = (f"// Generated: geometry shared with frontend/zeroteam_board.h.\n"
                f"#define ZT_SCREEN_WIDTH {width}\n#define ZT_SCREEN_HEIGHT {height}\n\n")
    update(GEN / "d_raiden2.cpp", geometry + driver + """
extern "C" UINT8 *zt_main_rom_ptr(void) { return DrvMainROM; }

// Diagnostics used by the tools while investigating the camera and the sprite
// list; they are not part of the board interface the frontend uses. The board
// is built with hidden visibility, so these need an explicit export.
#define ZT_EXPORT extern "C" __attribute__((visibility("default")))
ZT_EXPORT UINT8 *zt_scroll_ptr(void) { return scroll; }
ZT_EXPORT UINT8 *zt_spr_ram_ptr(void) { return DrvSprRAM; }
ZT_EXPORT UINT16 zt_spr_cur_start(void) { return sprites_cur_start; }
ZT_EXPORT UINT8 *zt_main_ram_ptr(void) { return DrvMainRAM; }
""")

    sources = [SRC / s for s in SOURCE_FILES]
    sources += [GEN / "d_raiden2.cpp", HERE / "zeroteam_board.cpp", HERE / "standalone.cpp"]
    extra = []
    objects_extra = []
    if args.aot:
        if not (GEN / "nec_aot.cpp").exists() or not (GEN / "zeroteam_sites.S").exists():
            parser.error("--aot needs build/generated/nec_aot.cpp; run aot/generate.py first")
        extra = ["-DZEROTEAM_AOT"]
        sources.append(GEN / "nec_aot.cpp")
        # The V25 core is a separate translation unit with its own copy of the
        # shared 8086 opcodes; the Vez interface still needs it.
        sources.append(SRC / "cpu/nec/v25.cpp")
        objects_extra.append(assemble(GEN / "zeroteam_sites.S"))
    else:
        sources += [SRC / s for s in CPU_FILES]

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        objects = list(pool.map(lambda s: compile_object(s, extra), sources))

    subprocess.run([
        "g++", "-shared", "-Wl,--gc-sections,-z,defs",
        "-o", str(BUILD / args.lib), *map(str, objects + objects_extra),
        "-lz", "-lm", "-pthread",
    ], check=True)
    print(f"wrote {BUILD / args.lib}")


if __name__ == "__main__":
    main()
