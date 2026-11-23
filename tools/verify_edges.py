"""Walk to both ends of the first fight with the actual board and controller inputs."""
import argparse
import ctypes as C
import os
from pathlib import Path
import subprocess
import sys


def check(board, chips, direction, cabinet):
    lib = C.CDLL(str(board.resolve()))
    lib.zt_set_widescreen(int(not cabinet))
    lib.zt_main_ram_ptr.restype = C.POINTER(C.c_uint8)
    if lib.zt_init(os.fsencode(chips)) != 0:
        raise RuntimeError("Board initialization failed")
    try:
        ram = lib.zt_main_ram_ptr()
        pixels = (C.c_uint32 * (428 * 256))()
        audio = (C.c_int16 * 4096)()
        def word(address):
            return ram[address] | ram[address + 1] << 8

        expected = (16 if cabinet else -38) if direction == "left" else (304 if cabinet else 358)
        consecutive = 0
        for frame in range(2600):
            button = 0
            if 30 <= frame < 45:
                button = 128  # coin
            elif 70 <= frame < 78:
                button = 64  # start
            elif 145 <= frame < 149:
                button = 16  # select character
            elif frame >= 760:
                button = 4 if direction == "left" else 8
            if lib.zt_frame(pixels, audio, button, 0) < 0:
                raise RuntimeError(f"Board stopped at frame {frame}")
            # P1 actor world X minus camera X, before the screen offset.
            x = C.c_int16(word(0x088a) - word(0x918e)).value
            at_edge = frame >= 760 and ram[0x08c0] and x == expected
            consecutive = consecutive + 1 if at_edge else 0
            if consecutive >= 3:
                print(f"{'4:3' if cabinet else '16:9'} {direction}: x={x} frame={frame}")
                return
        raise RuntimeError(f"{direction} did not reach {expected}")
    finally:
        lib.zt_shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", type=Path)
    parser.add_argument("chips", type=Path)
    parser.add_argument("--direction", choices=["left", "right"])
    parser.add_argument("--cabinet", action="store_true")
    args = parser.parse_args()
    if args.direction:
        check(args.board, args.chips, args.direction, args.cabinet)
    else:
        for mode in ([], ["--cabinet"]):
            for direction in ("left", "right"):
                subprocess.run([sys.executable, __file__, args.board, args.chips,
                                "--direction", direction, *mode], check=True)
