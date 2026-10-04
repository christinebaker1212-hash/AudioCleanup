#!/usr/bin/env python3
"""Build a blind, loudness-matched listening set.

For every (file, preset) pair the CLI renders the processed result plus the
latency-aligned original matched to the processed loudness (--reference). The
two are given random labels A/B so the listener does not know which is which;
the key is written separately. Open index.html in a browser to listen and
record preferences. Standard library only.

usage: make_listening_set.py --cli path/to/af_cli --out listening/ \
           voice.studio:dialog.wav music.warm:mix.wav sfx.punchy:hit.wav ...
"""
import argparse
import html
import json
import os
import random
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("items", nargs="+", help="preset_id:path")
    a = ap.parse_args()
    rng = random.Random(a.seed)
    os.makedirs(a.out, exist_ok=True)
    key, rows = [], []
    for n, item in enumerate(a.items, 1):
        preset, path = item.split(":", 1)
        tag = f"{n:02d}"
        proc = os.path.join(a.out, f"{tag}_processed.wav")
        ref = os.path.join(a.out, f"{tag}_original_matched.wav")
        rem = os.path.join(a.out, f"{tag}_removed.wav")
        r = subprocess.run([a.cli, "process", path, proc, "--preset", preset, "--bits", "24",
                            "--reference", ref, "--removed", rem, "--plan"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            continue
        with open(os.path.join(a.out, f"{tag}_plan.txt"), "w") as f:
            f.write(r.stdout)
        flip = rng.random() < 0.5
        A, B = (proc, ref) if flip else (ref, proc)
        la, lb = os.path.join(a.out, f"{tag}_A.wav"), os.path.join(a.out, f"{tag}_B.wav")
        os.replace(A, la)
        os.replace(B, lb)
        key.append({"item": tag, "source": path, "preset": preset, "A": "processed" if flip else "original", "B": "original" if flip else "processed"})
        rows.append((tag, os.path.basename(path), preset))
        print(f"{tag}: {path} [{preset}] done")
    with open(os.path.join(a.out, "key.json"), "w") as f:
        json.dump(key, f, indent=2)
    with open(os.path.join(a.out, "index.html"), "w") as f:
        f.write("<!doctype html><meta charset=utf-8><title>Blind A/B</title><style>body{font:14px system-ui;margin:24px;background:#15171c;color:#dfe3ea}"
                "td,th{padding:6px 10px;border-bottom:1px solid #343a46}audio{width:260px}</style>"
                "<h2>Blind loudness-matched A/B</h2><p>A and B are the original (latency-aligned, matched to the processed loudness) and the "
                "processed render, in random order. Record which you prefer and why; the key is in key.json.</p><table><tr><th>#</th><th>Source</th><th>Preset</th><th>A</th><th>B</th><th>Prefer</th><th>Notes</th></tr>")
        for tag, src, preset in rows:
            f.write(f"<tr><td>{tag}</td><td>{html.escape(src)}</td><td>{html.escape(preset)}</td>"
                    f"<td><audio controls src='{tag}_A.wav'></audio></td><td><audio controls src='{tag}_B.wav'></audio></td>"
                    f"<td><select><option>-</option><option>A</option><option>B</option><option>no difference</option></select></td>"
                    f"<td><input size=40></td></tr>")
        f.write("</table>")
    print(f"wrote {a.out}/index.html and key.json")


if __name__ == "__main__":
    main()
