#!/usr/bin/env python3
"""Small deterministic fixtures shared by CTest; requires FFmpeg CLI/libx264."""
import pathlib
import subprocess
import sys

ffmpeg, destination = sys.argv[1:]
folder = pathlib.Path(destination)
folder.mkdir(parents=True, exist_ok=True)


def generate(name, *arguments):
    output = folder / name
    if not output.is_file():
        subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-y',
                        *arguments, str(output)], check=True)


for color, frequency in [('red', 300), ('green', 700), ('blue', 1500)]:
    video = 'testsrc2=s=320x180:r=30:d=4' if color == 'red' else f'color=c={color}:s=320x180:r=30:d=4'
    generate(f'av_{color}.mp4', '-f', 'lavfi', '-i', video, '-f', 'lavfi', '-i',
             f'sine=frequency={frequency}:sample_rate=48000:duration=4',
             '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-shortest')
generate('tone.wav', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=2',
         '-c:a', 'pcm_s16le')
generate('still.png', '-f', 'lavfi', '-i', 'color=c=yellow:s=400x300:d=0.1',
         '-frames:v', '1', '-threads', '1')
