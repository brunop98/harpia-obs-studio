#!/usr/bin/env python3
"""Build harpia/resources/emoji/noto-emoji.pack from Google's Noto Emoji images.

The editor draws emoji in text clips as these pictures (editor/timeline/Emoji.hpp),
the same artwork as Android, Gmail and Google Chat, instead of the one-colour
outline a font glyph becomes in a text path.

    git clone --depth 1 --filter=blob:none --sparse https://github.com/googlefonts/noto-emoji
    (cd noto-emoji && git sparse-checkout set 2D/png/128 third_party/region-flags/png)
    python3 harpia/scripts/make-emoji-pack.py noto-emoji/2D/png/128 harpia/resources/emoji/noto-emoji.pack \
        noto-emoji/third_party/region-flags/png

Each 128 px PNG becomes a WebP (quality 80, with its alpha), which FFmpeg -- already
in Harpia -- decodes; a third of the PNGs' size.

Flags are not among the 128 px pictures: Noto builds them from region-flags, one
picture per country ("BR.png") at its own proportions. Each is fitted into the
128 px square (at most 120 x 96, centred), with slightly rounded corners, and
filed under its emoji: the two regional indicator letters (BR -> 1f1e7_1f1f7),
or for England, Scotland and Wales the black flag and tag letters.

The pack (little endian):
    8 bytes  "HEMOJI01"
    u32      count
    count x  { u16 key length, key (ASCII, e.g. "1f468_200d_1f469"), u32 offset, u32 size }
    the WebP files, back to back (offsets from the start of the pack)
Keys are the Noto file names without "emoji_u" and ".png": code points in lower-case
hex, at least 4 digits, joined by "_", with no U+FE0F (Noto's own convention).
"""
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile


def to_webp(png, out):
    subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", png, "-c:v", "libwebp",
                    "-quality", "80", out], check=True)
    with open(out, "rb") as f:
        return f.read()


# Corner radius 8 px on the fitted flag, then centred on a transparent square.
FLAG_FILTER = ("scale=w=120:h=96:force_original_aspect_ratio=decrease:flags=lanczos,format=rgba,"
               "geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':"
               "a='alpha(X,Y)*clip(8.5-hypot(max(max(8-X,X-(W-9)),0),max(max(8-Y,Y-(H-9)),0)),0,1)',"
               "pad=128:128:(ow-iw)/2:(oh-ih)/2:color=black@0")


def flag_to_webp(png, out):
    subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", png, "-vf", FLAG_FILTER,
                    "-c:v", "libwebp", "-quality", "80", out], check=True)
    with open(out, "rb") as f:
        return f.read()


def flag_key(name):
    """'BR' -> regional indicators; 'GB-ENG' -> the tag sequence; else None."""
    if len(name) == 2 and name.isalpha():
        return "_".join("%x" % (0x1F1E6 + ord(c) - ord("A")) for c in name.upper())
    if name in ("GB-ENG", "GB-SCT", "GB-WLS"):
        tags = "".join(name.lower().split("-"))
        return "_".join(["1f3f4"] + ["%x" % (0xE0000 + ord(c)) for c in tags] + ["e007f"])
    return None


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    flags = sys.argv[3] if len(sys.argv) == 4 else None
    names = sorted(n for n in os.listdir(src) if n.startswith("emoji_u") and n.endswith(".png"))
    with tempfile.TemporaryDirectory() as tmp:
        jobs = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            for n in names:
                key = n[len("emoji_u"):-len(".png")]
                jobs[key] = pool.submit(to_webp, os.path.join(src, n), os.path.join(tmp, key + ".webp"))
            for n in sorted(os.listdir(flags)) if flags else []:
                key = flag_key(n[:-len(".png")]) if n.endswith(".png") else None
                if key and key not in jobs:
                    jobs[key] = pool.submit(flag_to_webp, os.path.join(flags, n), os.path.join(tmp, key + ".webp"))
        blobs = [(k.encode("ascii"), jobs[k].result()) for k in sorted(jobs)]
    index = sum(2 + len(k) + 8 for k, _ in blobs)
    offset = 8 + 4 + index
    head = bytearray(b"HEMOJI01" + struct.pack("<I", len(blobs)))
    for k, data in blobs:
        head += struct.pack("<H", len(k)) + k + struct.pack("<II", offset, len(data))
        offset += len(data)
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(head)
        for _, data in blobs:
            f.write(data)
    print(f"{len(blobs)} emoji, {offset / 1048576:.1f} MB -> {dst}")


if __name__ == "__main__":
    main()
