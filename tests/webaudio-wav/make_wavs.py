#!/usr/bin/env python3
"""WAV files for the 0048 reader test: each with a known ramp, and a .txt with the expected floats."""
import math
import os
import struct
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
FRAMES = 37


def values(channels):
    # per channel a different ramp in [-1, 1)
    return [[math.sin(0.3 * f + c) * 0.9 for f in range(FRAMES)] for c in range(channels)]


def pack(v, fmt, bits):
    if fmt == 3:
        return struct.pack("<f" if bits == 32 else "<d", v)
    if bits == 8:
        return bytes([max(0, min(255, int(round(v * 128)) + 128))])
    scale = 1 << (bits - 1)
    i = max(-scale, min(scale - 1, int(round(v * scale))))
    return (i & ((1 << bits) - 1)).to_bytes(bits // 8, "little")


def expected(v, fmt, bits):
    if fmt == 3:
        return struct.unpack("<f", struct.pack("<f", v))[0] if bits == 32 else v
    if bits == 8:
        return (max(0, min(255, int(round(v * 128)) + 128)) - 128) / 128
    scale = 1 << (bits - 1)
    return max(-scale, min(scale - 1, int(round(v * scale)))) / scale


def write(name, fmt, bits, channels, rate, extensible=False, extra_chunk=False, odd_list=False, data_first=False, truncated=False):
    vals = values(channels)
    block = channels * bits // 8
    data = b"".join(pack(vals[c][f], fmt, bits) for f in range(FRAMES) for c in range(channels))
    if extensible:
        fmt_body = struct.pack("<HHIIHH", 0xFFFE, channels, rate, rate * block, block, bits)
        fmt_body += struct.pack("<HHI", 22, bits, 0) + struct.pack("<H", fmt) + bytes(14)
    else:
        fmt_body = struct.pack("<HHIIHH", fmt, channels, rate, rate * block, block, bits)
    chunks = []
    fmt_chunk = b"fmt " + struct.pack("<I", len(fmt_body)) + fmt_body
    data_size = len(data) + (1000 if truncated else 0)
    data_chunk = b"data" + struct.pack("<I", data_size) + data
    if odd_list:
        chunks.append(b"LIST" + struct.pack("<I", 5) + b"abcde" + b"\0")   # odd size: padded
    if extra_chunk:
        chunks.append(b"fact" + struct.pack("<I", 4) + struct.pack("<I", FRAMES))
    chunks += [data_chunk, fmt_chunk] if data_first else [fmt_chunk, data_chunk]
    body = b"WAVE" + b"".join(chunks)
    with open(os.path.join(out, name + ".wav"), "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    with open(os.path.join(out, name + ".txt"), "w") as f:
        f.write(f"{channels} {rate} {FRAMES}\n")
        for c in range(channels):
            f.write(" ".join(repr(expected(vals[c][fr], fmt, bits)) for fr in range(FRAMES)) + "\n")


write("pcm8_mono", 1, 8, 1, 48000)
write("pcm16_stereo", 1, 16, 2, 48000, extra_chunk=True)
write("pcm24_stereo", 1, 24, 2, 48000, odd_list=True)
write("pcm32_mono", 1, 32, 1, 48000)
write("float32_stereo", 3, 32, 2, 48000)
write("float64_mono", 3, 64, 1, 48000)
write("ext16_6ch", 1, 16, 6, 48000, extensible=True)
write("ext_float_stereo", 3, 32, 2, 48000, extensible=True)
write("data_first", 1, 16, 2, 48000, data_first=True)
write("truncated", 1, 16, 1, 48000, truncated=True)
write("rate22050", 1, 16, 1, 22050)
# not read here (GStreamer's): A-law, 12-bit, a bad block size
with open(os.path.join(out, "reject_alaw.wav"), "wb") as f:
    body = b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 6, 1, 8000, 8000, 1, 8) + b"data" + struct.pack("<I", 4) + b"\x01\x02\x03\x04"
    f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
with open(os.path.join(out, "reject_notwav.wav"), "wb") as f:
    f.write(b"OggS" + bytes(100))
with open(os.path.join(out, "reject_short.wav"), "wb") as f:
    f.write(b"RIFF\x04\x00\x00\x00WAV")
