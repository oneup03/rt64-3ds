#!/usr/bin/env python3
"""Make a game mode's ROM for rt64-3ds on a PC.

A game mode (a ROM hack played through a recomp's built-in code) needs the
hack's ROM, which the mod's .nrm makes from the game's ROM with its
patch.bps. The 3DS doesn't make it: the patched ROM is 64 MB and the patch
alone more than an Old 3DS has room for. This script does it once.

Run with no arguments (or double-clicked), it works in the folder it sits
in, laid out like the SD card's root as a 3DS release zip extracts: the
game's ROM in 3ds/<game>/ (Banjo: 3ds/BanjoRecomp/BK.z64) and the modes'
.nrm files next to the script. Or name them:

    python3 make_game_mod_rom.py BK.z64 bk_jiggies_of_time.nrm

Either way it writes mods/<mod id>.z64 and mods/<mod id>.z64.ok (the mode
and version it was made from) next to the ROM (-o names another folder),
which is where the game looks for them on the SD card; the .nrm itself
isn't needed there. Several .nrm files may be given at once. A ROM in .v64
or .n64 byte order is read as .z64. Standard library only.
"""
import argparse
import json
import os
import sys
import zipfile
import zlib


class BpsError(Exception):
    pass


def to_z64(rom):
    """The ROM in big-endian (.z64) byte order, whatever order it came in."""
    head = rom[:4]
    if head == b"\x80\x37\x12\x40":
        return rom
    if head == b"\x37\x80\x40\x12":      # .v64: 16-bit words byte-swapped
        out = bytearray(rom)
        out[0::2], out[1::2] = rom[1::2], rom[0::2]
        return bytes(out)
    if head == b"\x40\x12\x37\x80":      # .n64: 32-bit words little-endian
        out = bytearray(len(rom))
        out[0::4], out[1::4], out[2::4], out[3::4] = rom[3::4], rom[2::4], rom[1::4], rom[0::4]
        return bytes(out)
    return rom


def apply_bps(patch, source):
    """The target the BPS patch makes from source (the spec:
    https://github.com/blakesmith/rombp/blob/master/docs/bps_spec.md)."""
    if patch[:4] != b"BPS1" or len(patch) < 16:
        raise BpsError("not a BPS patch")
    source_crc, target_crc, patch_crc = (int.from_bytes(patch[i:i + 4], "little") for i in (-12, -8, -4))
    # Mod tools leave the patch's own CRC at 0 (the runtime skips it too).
    if patch_crc != 0 and zlib.crc32(patch[:-4]) != patch_crc:
        raise BpsError("the patch is damaged (its checksum doesn't match)")
    if zlib.crc32(source) != source_crc:
        raise BpsError("this ROM isn't the one the patch is for (check it's the right game and version)")
    pos = 4
    end = len(patch) - 12

    def number():
        nonlocal pos
        value, shift = 0, 1
        while True:
            byte = patch[pos]
            pos += 1
            value += (byte & 0x7F) * shift
            if byte & 0x80:
                return value
            shift <<= 7
            value += shift

    source_size = number()
    target_size = number()
    metadata = number()                   # (pos += number() would add to the old pos)
    pos += metadata
    if source_size != len(source):
        raise BpsError("the ROM's size isn't the one the patch is for")
    target = bytearray(target_size)
    out = 0
    source_rel = target_rel = 0
    while pos < end:
        data = number()
        action, length = data & 3, (data >> 2) + 1
        if action == 0:                   # SourceRead
            target[out:out + length] = source[out:out + length]
        elif action == 1:                 # TargetRead
            target[out:out + length] = patch[pos:pos + length]
            pos += length
        elif action == 2:                 # SourceCopy
            offset = number()
            source_rel += -(offset >> 1) if offset & 1 else offset >> 1
            target[out:out + length] = source[source_rel:source_rel + length]
            source_rel += length
        else:                             # TargetCopy: may overlap what it writes
            offset = number()
            target_rel += -(offset >> 1) if offset & 1 else offset >> 1
            if target_rel + length <= out:
                target[out:out + length] = target[target_rel:target_rel + length]
            else:                         # a run repeating what it copies
                for i in range(length):
                    target[out + i] = target[target_rel + i]
            target_rel += length
        out += length
    if out != target_size or zlib.crc32(target) != target_crc:
        raise BpsError("the patched ROM came out wrong (its checksum doesn't match)")
    return bytes(target)


def make(rom, nrm_path, out_dir):
    with zipfile.ZipFile(nrm_path) as nrm:
        manifest = json.loads(nrm.read("mod.json"))
        try:
            patch = nrm.read("patch.bps")
        except KeyError:
            raise BpsError("isn't a game mode (it has no patch.bps)")
    mod_id = manifest["id"]
    print(f"{os.path.basename(nrm_path)}: {manifest.get('display_name', mod_id)} {manifest.get('version', '')}")
    target = apply_bps(patch, rom)
    mods = os.path.join(out_dir, "mods")
    os.makedirs(mods, exist_ok=True)
    rom_out = os.path.join(mods, mod_id + ".z64")
    with open(rom_out + ".part", "wb") as f:
        f.write(target)
    os.replace(rom_out + ".part", rom_out)
    # What the 3DS checks (rt64-3ds game_modes.cpp): the mode and its
    # version against the one built in, and the ROM's size.
    with open(rom_out + ".ok", "w") as f:
        f.write(f"{mod_id} {manifest['version']} {len(target)}\n")
    print(f"  wrote {rom_out} ({len(target) >> 20} MB) and {os.path.basename(rom_out)}.ok")


ROM_EXTS = (".z64", ".v64", ".n64")


def find_inputs(base):
    """No arguments: the .nrm files next to the script, and the one ROM in a
    3ds/<game>/ folder under it (the SD card's layout)."""
    nrms = sorted(os.path.join(base, n) for n in os.listdir(base) if n.lower().endswith(".nrm"))
    roms = []
    top = os.path.join(base, "3ds")
    if os.path.isdir(top):
        for game in sorted(os.listdir(top)):
            folder = os.path.join(top, game)
            if os.path.isdir(folder):
                roms += [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if n.lower().endswith(ROM_EXTS)]
    if not nrms:
        raise SystemExit(f"No .nrm files next to the script ({base}): put the game modes' .nrm files there.")
    if not roms:
        raise SystemExit(f"No ROM in {os.path.join(top, '<game>')}: put the game's ROM in its folder there "
                         "(Banjo: 3ds/BanjoRecomp/BK.z64).")
    if len(roms) > 1:
        raise SystemExit("More than one ROM under 3ds/; name the one to use: make_game_mod_rom.py <ROM> <.nrm files>\n  "
                         + "\n  ".join(roms))
    return roms[0], nrms


def run(args):
    if args.rom is None:
        rom_path, nrms = find_inputs(os.path.dirname(os.path.abspath(__file__)))
    else:
        if not args.nrm:
            raise SystemExit("Name the game modes' .nrm files after the ROM.")
        rom_path, nrms = args.rom, args.nrm
    out = args.out if args.out is not None else os.path.dirname(os.path.abspath(rom_path))
    print(f"ROM: {rom_path}")
    with open(rom_path, "rb") as f:
        rom = to_z64(f.read())
    failed = 0
    for nrm in nrms:
        try:
            make(rom, nrm, out)
        except (BpsError, KeyError, ValueError, zipfile.BadZipFile, OSError) as e:
            print(f"{nrm}: {e}", file=sys.stderr)
            failed += 1
    if failed == 0 and args.out is None:
        print(f"Done: the game modes' ROMs are in {os.path.join(out, 'mods')}, where the game looks for them "
              "(copy the 3ds folder to the SD card's root if this isn't the SD card).")
    elif failed == 0:
        print(f"Done: copy the files in {os.path.join(out, 'mods')} to the mods folder next to the game on the SD card.")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description="Make game modes' ROMs from the game's ROM and the modes' .nrm files, for rt64-3ds. "
                                 "With no arguments: the .nrm files next to this script and the ROM in its 3ds/<game>/ folder.")
    ap.add_argument("rom", nargs="?", help="the game's ROM (the one the 3DS port uses, e.g. BK.z64)")
    ap.add_argument("nrm", nargs="*", help="game mode .nrm files")
    ap.add_argument("-o", "--out", default=None, help="where to write the mods folder (default: next to the ROM)")
    args = ap.parse_args()
    try:
        status = run(args)
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            status = 1
        else:
            status = e.code or 0
    # Double-clicked (no arguments, a console of its own): keep the window
    # open until the result is read.
    if args.rom is None and sys.stdin is not None and sys.stdin.isatty():
        try:
            input("Press Enter to close.")
        except EOFError:
            pass
    return status


if __name__ == "__main__":
    sys.exit(main())
