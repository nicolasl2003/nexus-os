#!/usr/bin/env python3
"""Génère le son d'ouverture de session de Nexus OS (carillon de 3 notes).
Sortie : packages/nexus-desktop/files/share/sounds/nexus/connexion.wav"""
import math, os, struct, wave

RATE = 22050
notes = [(659.25, 0.00), (830.61, 0.16), (987.77, 0.32), (1318.5, 0.48)]   # mi, sol#, si, mi aigu
duration = 1.8
samples = [0.0] * int(RATE * duration)
for freq, start in notes:
    s0 = int(start * RATE)
    for i in range(int(1.3 * RATE)):
        t = i / RATE
        if s0 + i >= len(samples):
            break
        env = min(1.0, t / 0.008) * math.exp(-t * 3.2)          # attaque douce, extinction
        tone = math.sin(2 * math.pi * freq * t) + 0.25 * math.sin(2 * math.pi * freq * 2 * t)
        samples[s0 + i] += 0.22 * env * tone
peak = max(abs(x) for x in samples)
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "packages", "nexus-desktop",
                   "files", "share", "sounds", "nexus", "connexion.wav")
with wave.open(out, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(b"".join(struct.pack("<h", int(x / peak * 0.8 * 32767)) for x in samples))
print("son généré :", os.path.normpath(out))
