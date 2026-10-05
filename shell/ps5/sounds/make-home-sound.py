#!/usr/bin/env python3
"""
PSFlyCast - the sound the console plays while the title is selected on its
home screen (sce_sys/snd0.at9).

Copyright 2026 the PSFlyCast contributors
SPDX-License-Identifier: GPL-3.0-or-later

Writes a 24-second loop as a WAV file: 48 kHz, stereo, 16-bit. It is made of
the start-up sound's material (make-startup-sound.html): its scale, D major
pentatonic, a soft chord that breathes in and out twice, and a few bell notes
over it. Everything in it wraps around its end, so the last sample runs into
the first: the loop has no seam.

    python3 make-home-sound.py home.wav

The console wants ATRAC9. The WAV is encoded with ps5-at9-converter
(BlackBearReloaded, GPL-3.0-or-later), which also sets its loudness to the
-28 LUFS that sce_sys/param.json declares:

    python3 ps5_at9.py --input home.wav --output ../sce_sys/snd0.at9 --duration 24 \
        --fade-in 0 --fade-out 0

(no fades at its ends: the loop is whole as it is).

The result of both commands is in the repository; this file is how to make it
again. Nothing here is random: the same file comes out every time.
"""
import math
import struct
import sys

import numpy as np

RATE = 48000
SECONDS = 24
LENGTH = RATE * SECONDS

# D major pentatonic, as the start-up sound has it.
D3, A3, D4, E4, FS4, A4, B4 = 146.83, 220.0, 293.66, 329.63, 369.99, 440.0, 493.88
D5, E5, FS5, A5, B5, D6 = 587.33, 659.25, 739.99, 880.0, 987.77, 1174.66
A2, E3 = 110.0, 164.81


def whole(frequency):
    """The nearest pitch that fits the loop a whole number of times."""
    return round(frequency * SECONDS) / SECONDS


def add(track, start, samples, left, right):
    """Mixes samples in from a moment, running round the end of the loop."""
    at = (int(start * RATE) + np.arange(len(samples))) % LENGTH
    np.add.at(track[0], at, samples * left)
    np.add.at(track[1], at, samples * right)


def pad(track, start, seconds, notes, gain):
    """A chord that swells and falls away: two slightly detuned voices a note."""
    t = np.arange(int(seconds * RATE)) / RATE
    swell = np.sin(math.pi * t / seconds) ** 2
    for index, note in enumerate(notes):
        for detune, side in ((-0.7, 0.3), (0.7, 0.7)):
            frequency = note * 2 ** (detune / 1200)
            voice = np.sin(2 * math.pi * frequency * t) + 0.25 * np.sin(4 * math.pi * frequency * t + index)
            level = gain / (1 + index * 0.35)
            add(track, start, voice * swell * level, 1 - side, side)


def bell(track, start, frequency, gain, side):
    """A small bell: a sine, a quiet octave and a quiet fifth above, dying away."""
    seconds = 3.2
    t = np.arange(int(seconds * RATE)) / RATE
    attack = np.minimum(t / 0.006, 1)
    tone = (np.sin(2 * math.pi * frequency * t) * np.exp(-t * 1.9)
            + 0.3 * np.sin(4 * math.pi * frequency * t) * np.exp(-t * 3.4)
            + 0.12 * np.sin(6 * math.pi * frequency * t) * np.exp(-t * 5.0))
    add(track, start, tone * attack * gain, 1 - side, side)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "home.wav"
    track = np.zeros((2, LENGTH))

    # Two breaths of twelve seconds, overlapping a little: D, then A with the
    # fourth open over it.
    pad(track, 0.0, 14.0, (D3, A3, D4, FS4, A4), 0.085)
    pad(track, 12.0, 14.0, (A2, E3, A3, E4, B4), 0.085)

    # A low note under all of it, at a pitch that fits the loop exactly.
    t = np.arange(LENGTH) / RATE
    low = np.sin(2 * math.pi * whole(D3 / 2) * t) * (0.6 + 0.4 * np.cos(2 * math.pi * t / SECONDS))
    track += low * 0.035

    # The bells: the start-up sound's letters' notes, a few of them, far apart.
    for start, note, gain, side in (
            (1.5, A5, 0.11, 0.35), (3.0, FS5, 0.09, 0.62), (4.75, D6, 0.07, 0.5),
            (7.5, E5, 0.09, 0.3), (9.0, A5, 0.08, 0.68),
            (13.5, B5, 0.11, 0.6), (15.0, E5, 0.09, 0.38), (16.75, A5, 0.07, 0.5),
            (19.5, FS5, 0.09, 0.7), (21.0, D5, 0.10, 0.32)):
        bell(track, start, note, gain, side)

    # A room: three echoes, each going round the loop's end as the notes do.
    wet = np.zeros_like(track)
    for delay, level, swap in ((0.137, 0.30, False), (0.293, 0.20, True), (0.467, 0.12, False)):
        echo = np.roll(track, int(delay * RATE), axis=1) * level
        wet += echo[::-1] if swap else echo
    track += wet

    peak = float(np.max(np.abs(track)))
    track *= 0.7 / peak
    samples = np.round(track.T * 32767).astype("<i2")
    data = samples.tobytes()
    with open(out, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)
    print(f"{out}: {SECONDS} s, peak {peak:.3f} before scaling")


if __name__ == "__main__":
    main()
