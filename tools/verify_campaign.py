"""Play the recorded single-player QA campaign and check the route through the credits."""
import argparse
import ctypes as C
import itertools
import os
from pathlib import Path
import zlib


# Background, middle and foreground map IDs in the USA release. The return to
# the forest after the last fight is part of the rescue scene.
ROUTE = (
    (5, 1, 2), (3, 6, 4), (12, 11, 10), (14, 15, 13),
    (19, 18, 17), (21, 20, 0), (0, 97, 98), (0, 24, 23),
    (82, 80, 81), (85, 83, 84), (88, 86, 87), (91, 89, 90),
    (29, 27, 28), (33, 32, 37), (0, 59, 60), (0, 39, 0),
    (45, 42, 41), (38, 43, 44), (99, 100, 101), (26, 31, 30),
    (0, 47, 46), (45, 42, 41), (94, 93, 0), (102, 103, 104),
    (92, 56, 0), (0, 0, 0),
)


def read_buttons(path):
    with path.open() as source:
        for line in source:
            if line.strip() and not line.startswith("#"):
                count, key = line.split()
                yield from itertools.repeat(int(key, 16), int(count))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", type=Path)
    parser.add_argument("chips", type=Path)
    parser.add_argument("--inputs", type=Path, default=Path(__file__).with_name("campaign.inputs"))
    args = parser.parse_args()
    # Free play and four lives in this test process only. The recording was
    # made with a health refill so its duration is independent of continues.
    os.environ.update(ZEROTEAM_DIP_A="ff", ZEROTEAM_DIP_B="fb", ZEROTEAM_DIP_C="f9")
    board = C.CDLL(str(args.board.resolve()))
    board.zt_main_ram_ptr.restype = C.POINTER(C.c_uint8)
    if board.zt_init(os.fsencode(args.chips)) != 0:
        raise RuntimeError("The board could not initialize with these chips")
    try:
        ram = board.zt_main_ram_ptr()
        video = (C.c_uint32 * (428 * 256))()
        audio = (C.c_int16 * 4096)()
        channels = memoryview(audio).cast("B")
        def word(addr):
            return ram[addr] | ram[addr + 1] << 8

        reached = audio_crc = video_crc = 0
        credits_at = None
        for frame, key in enumerate(read_buttons(args.inputs)):
            if frame > 400 and ram[0x8c0] and 0 < word(0x8dc) <= 768:
                ram[0x8dc], ram[0x8dd] = 0, 3
            samples = board.zt_frame(video, audio, key, 0)
            if not 0 <= samples <= 2048:
                raise RuntimeError(f"Invalid audio length at frame {frame}")
            audio_crc = zlib.crc32(channels[:4 * samples], audio_crc)
            maps = (word(0x9be2), word(0x9c24), word(0x9c66))
            milestone = reached < len(ROUTE) and maps == ROUTE[reached]
            if milestone:
                reached += 1
                print(f"{frame:7d}  map {maps}", flush=True)
                if reached == len(ROUTE):
                    credits_at = frame
            if milestone or frame % 60 == 0:
                video_crc = zlib.crc32(video, video_crc)
            if credits_at is not None and frame >= credits_at + 350:
                print(f"credits frame={frame} video={video_crc:08x} audio={audio_crc:08x} "
                      f"fallback={board.zt_aot_fallback()}")
                return
        raise RuntimeError(f"Replay ended after {reached}/{len(ROUTE)} milestones")
    finally:
        board.zt_shutdown()


if __name__ == "__main__":
    main()
