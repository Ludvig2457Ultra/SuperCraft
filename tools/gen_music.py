#!/usr/bin/env python3
"""Генерирует короткий synthwave-луп для живого эфира SuperCraft.

Без внешних файлов и лицензий: всё синтезируется numpy и сохраняется в WAV.
"""

from pathlib import Path

import numpy as np
import wave

SR = 44100
BPM = 120
BEAT = 60 / BPM
BARS = 4
DUR = BARS * 4 * BEAT           # 8 секунд
FADE = 0.12                     # кроссфейд для бесшовного лупа
OUT = Path(__file__).resolve().parents[1] / "assets" / "supercraft-loop.wav"

NOTES = {
    "A2": 110.00, "C3": 130.81, "E3": 164.81,
    "F2": 87.31, "A3": 220.00, "C4": 261.63,
    "G2": 98.00, "B2": 123.47, "D3": 146.83,
    "E4": 329.63, "G3": 196.00, "F3": 174.61,
}
CHORDS = [
    ("A2", "C3", "E3"),
    ("F2", "A2", "C4"),
    ("C3", "E3", "G3"),
    ("G2", "B2", "D3"),
]
ARP = ["C4", "E4", "A3", "E4", "F3", "C4", "A3", "F3"]


def envelope(t: np.ndarray, attack: float, release: float, length: float) -> np.ndarray:
    a = np.clip(t / attack, 0, 1)
    r = np.clip((length - t) / release, 0, 1)
    return np.minimum(a, r)


def synth(total: float) -> np.ndarray:
    n = int(total * SR)
    t = np.arange(n) / SR
    out = np.zeros(n, dtype=np.float64)

    # Пэд: по аккорду на два такта (2 секунды).
    seg = 2 * BEAT
    for index, chord in enumerate(CHORDS):
        start = int(index * seg * SR)
        stop = min(n, int((index + 1) * seg * SR))
        local = np.arange(stop - start) / SR
        env = envelope(local, 0.10, 0.18, seg)
        for j, note in enumerate(chord):
            f = NOTES[note]
            detune = 1 + (j - 1) * 0.0015
            out[start:stop] += 0.055 * env * (
                np.sin(2 * np.pi * f * detune * local)
                + 0.45 * np.sin(2 * np.pi * f * 2 * local)
            )

    # Арпеджио: восьмые ноты, короткий затухающий звук.
    step = BEAT / 2
    for k in range(int(np.ceil(total / step))):
        start = int(k * step * SR)
        stop = min(n, int((k + 1) * step * SR))
        if start >= n:
            break
        local = np.arange(stop - start) / SR
        f = NOTES[ARP[k % len(ARP)]]
        env = np.exp(-local * 11)
        out[start:stop] += 0.075 * env * (
            np.sin(2 * np.pi * f * local)
            + 0.25 * np.sin(2 * np.pi * f * 2 * local)
        )

    # Кик на каждую долю.
    for k in range(int(np.ceil(total / BEAT))):
        start = int(k * BEAT * SR)
        stop = min(n, int((k + 0.32) * SR + start))
        if start >= n or stop <= start:
            continue
        local = np.arange(stop - start) / SR
        phase = 2 * np.pi * (45 * local + 80 * (1 - np.exp(-local * 22)) / 22)
        out[start:stop] += 0.28 * np.sin(phase) * np.exp(-local * 9)

    # Хэт на оффбитах.
    rng = np.random.default_rng(2457)
    noise = rng.normal(0, 1, n)
    for k in range(int(np.ceil(total / BEAT))):
        start = int((k + 0.5) * BEAT * SR)
        stop = min(n, start + int(0.06 * SR))
        if start >= n or stop <= start:
            continue
        local = np.arange(stop - start) / SR
        out[start:stop] += 0.055 * noise[start:stop] * np.exp(-local * 55)

    return out


def main() -> None:
    raw = synth(DUR + FADE)
    body = raw[: int(DUR * SR)].copy()
    tail = raw[int(DUR * SR): int((DUR + FADE) * SR)]
    fade_n = len(tail)
    if fade_n:
        win = np.linspace(0, 1, fade_n)
        body[:fade_n] = body[:fade_n] * win + tail * (1 - win)

    peak = np.max(np.abs(body)) or 1.0
    body = body / peak * 0.72

    # Лёгкое стерео: правый канал задержан на 9 мс.
    left = body
    delay = int(0.009 * SR)
    right = np.concatenate([np.zeros(delay), body[:-delay]])
    stereo = np.stack([left, right], axis=1)
    pcm = (np.clip(stereo, -1, 1) * 32767).astype("<i2")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(OUT), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(SR)
        wav.writeframes(pcm.tobytes())
    print(f"{OUT} · {DUR:.1f} с · {OUT.stat().st_size // 1024} КБ")


if __name__ == "__main__":
    main()
