"""Extract and CRC-verify the 13 chips of the Zero Team USA (Seibu 1993) set.

The arcade set distributed as Zero-Team-U-S-A_Arcade_EN.zip contains a single
member, zeroteam.zip. FBNeo's zeroteamRomDesc lists the size and CRC32 of every
chip, so the set identity is checked here before any code generation runs.

Chips are written as individual files named by CRC so that the build never has
to rely on the archive member names, which differ between redistributions.
"""
import argparse
import hashlib
import json
import sys
import zipfile
import zlib
from pathlib import Path

# (index, name, size, crc32) from FBNeo zeroteamRomDesc (revision 61cd3ede).
CHIPS = (
    (0, "seibu__1.u024.5k", 0x040000, 0x25AA5BA4),
    (1, "seibu__3.u023.6k", 0x040000, 0xEC79A12B),
    (2, "seibu__2.u025.6l", 0x040000, 0x54F3D359),
    (3, "seibu__4.u026.5l", 0x040000, 0xA017B8D0),
    (4, "copx-d2.u0313.6n", 0x040000, 0xA6732FF9),
    (5, "seibu__5.u1110.5b", 0x010000, 0x7EC1FBC3),
    (6, "seibu__7.u072.5s", 0x010000, 0x9F6AA0F0),
    (7, "seibu__8.u077.5r", 0x010000, 0x68F7DDDC),
    (8, "musha_back-1.u075.4s", 0x100000, 0x8B7F9219),
    (9, "musha_back-2.u0714.2s", 0x080000, 0xCE61C952),
    (10, "musha_obj-1.u0811.6f", 0x200000, 0x45BE8029),
    (11, "musha_obj-2.u082.5f", 0x200000, 0xCB61C19D),
    (12, "seibu__6.u105.4a", 0x040000, 0x48BE32B1),
)


def open_archive(path):
    """Return a ZipFile for the inner zeroteam.zip, given the outer or inner ZIP."""
    outer = zipfile.ZipFile(path)
    names = outer.namelist()
    inner = [n for n in names if n.lower() == "zeroteam.zip"]
    if inner:
        data = outer.read(inner[0])
        import io

        return zipfile.ZipFile(io.BytesIO(data))
    if any(n.lower().endswith("/zeroteam.zip") for n in names):
        member = next(n for n in names if n.lower().endswith("/zeroteam.zip"))
        data = outer.read(member)
        import io

        return zipfile.ZipFile(io.BytesIO(data))
    # Already the inner archive: verify by content instead of name.
    if any(info.CRC == CHIPS[0][3] for info in outer.infolist()):
        return outer
    raise SystemExit(f"{path}: no zeroteam.zip member and no known chip CRCs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="Zero-Team...zip or zeroteam.zip")
    parser.add_argument("out", type=Path, help="directory for the extracted chips")
    args = parser.parse_args()

    archive = open_archive(args.rom)
    by_crc = {}
    for info in archive.infolist():
        if info.is_dir():
            continue
        by_crc.setdefault(info.CRC, []).append(info)

    args.out.mkdir(parents=True, exist_ok=True)
    report = []
    missing = []
    for index, name, size, crc in CHIPS:
        entries = by_crc.get(crc, [])
        if not entries:
            missing.append(f"{name} crc {crc:08x}")
            continue
        info = entries[0]
        data = archive.read(info)
        if len(data) != size:
            raise SystemExit(f"{name}: size {len(data)} != {size}")
        path = args.out / f"{index:02d}-{crc:08x}.bin"
        path.write_bytes(data)
        report.append(
            {
                "index": index,
                "name": name,
                "size": size,
                "crc32": f"{crc:08x}",
                "sha1": hashlib.sha1(data).hexdigest(),
                "sha256": hashlib.sha256(data).hexdigest(),
                "archive_member": info.filename,
            }
        )

    if missing:
        raise SystemExit("missing chips:\n  " + "\n  ".join(missing))

    totals = sum(item["size"] for item in report)
    print(
        json.dumps(
            {
                "set": "zeroteam",
                "title": "Zero Team USA (US)",
                "chips": report,
                "total_bytes": totals,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    sys.exit(main())
