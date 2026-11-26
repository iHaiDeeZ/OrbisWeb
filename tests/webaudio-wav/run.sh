#!/usr/bin/env bash
# PC test of WebKit patch 0048's WAV reader (wispBusFromWav), taken out of the patched source as is and
# compiled against a stand-in AudioBus; WAV files of each format made by make_wavs.py.
set -e
cd "$(dirname "$0")"
SRC=~/wisp-build/src/WebKit/Source/WebCore/platform/audio/gstreamer/AudioFileReaderGStreamer.cpp
awk '/^static RefPtr<AudioBus> wispBusFromWav/{on=1} on{print} on && /^}$/{exit}' "$SRC" > reader.inc
test -s reader.inc
python3 make_wavs.py wavs
clang++-21 -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -o wavtest test.cpp
./wavtest wavs
