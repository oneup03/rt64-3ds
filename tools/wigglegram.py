#!/usr/bin/env python3
"""Turn 3DS left/right screenshots into a wigglegram (an animated GIF that
flips between the two eyes, so the depth shows without a 3D display).

The 3DS (and Azahar) save a stereo capture as two files:
    <stamp>_top.bmp        left eye
    <stamp>_top_right.bmp  right eye

Usage:
    wigglegram.py                      # every *_top.bmp pair in the current dir
    wigglegram.py shots/ other.bmp     # pairs in a dir, or a given left image
    wigglegram.py LEFT RIGHT -o out.gif
Needs Pillow.
"""
import argparse
import sys
from pathlib import Path

from PIL import Image


def right_for(left: Path) -> Path:
    return left.with_name(left.stem + "_right" + left.suffix)


def find_pairs(paths):
    lefts = []
    for p in map(Path, paths or ["."]):
        if p.is_dir():
            lefts += sorted(p.glob("*_top.bmp"))
        else:
            lefts.append(p)
    return [(l, right_for(l)) for l in lefts if right_for(l).exists()]


def wigglegram(left: Path, right: Path, out: Path, scale: int, ms: int):
    frames = [Image.open(p).convert("RGB") for p in (left, right)]
    if frames[0].size != frames[1].size:
        sys.exit(f"{left} and {right} differ in size")
    if scale != 1:
        w, h = frames[0].size
        frames = [f.resize((w * scale, h * scale), Image.NEAREST) for f in frames]
    frames[0].save(out, save_all=True, append_images=frames[1:],
                   duration=ms, loop=0, optimize=False)
    print(f"{out}  ({left.name} + {right.name})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", help="dirs or left-eye images, or LEFT RIGHT")
    ap.add_argument("-o", "--output", help="output GIF (single pair only)")
    ap.add_argument("-s", "--scale", type=int, default=2,
                    help="integer upscale, nearest neighbour (default 2)")
    ap.add_argument("-d", "--delay", type=int, default=150,
                    help="milliseconds per frame (default 150)")
    args = ap.parse_args()

    ins = [Path(p) for p in args.inputs]
    if len(ins) == 2 and all(p.is_file() for p in ins):
        pairs = [tuple(ins)]  # explicit LEFT RIGHT
    else:
        pairs = find_pairs(args.inputs)
    if not pairs:
        sys.exit("no left/right pairs found (expected *_top.bmp + *_top_right.bmp)")
    if args.output and len(pairs) > 1:
        sys.exit("-o only works with a single pair")

    for left, right in pairs:
        stem = left.stem.removesuffix("_top")
        out = Path(args.output) if args.output else left.with_name(stem + "_wiggle.gif")
        wigglegram(left, right, out, args.scale, args.delay)


if __name__ == "__main__":
    main()
