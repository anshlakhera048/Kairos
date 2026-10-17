#!/usr/bin/env python3
"""Generate docs/demo.svg: an animated terminal cast of real kairos output.

Regenerate with:
    KAIROS_TOURNAMENT=<path> python3 tools/demo_cast.py

Runs `kairos demo --quiet` and `kairos realism-gap` for real, captures
their actual output, and renders a self-contained SVG with a staggered
line-reveal animation (pure CSS, no JS). The cast shows real output —
never hand-written.
"""

import html
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "python"))

ANSI = re.compile(r"\x1b\[[0-9;]*m")

W, H = 920, 600
FS = 14
LH = 22
PAD_X, PAD_Y = 28, 44
MAX_CHARS = 104


def run(cmd):
    env = dict(os.environ)
    p = subprocess.run([sys.executable, "-m", "kairos.cli"] + cmd,
                       capture_output=True, text=True, cwd=REPO, env=env)
    if p.returncode != 0:
        sys.exit(f"command failed: kairos {' '.join(cmd)}\n{p.stderr[-2000:]}")
    return ANSI.sub("", p.stdout)


def esc(s):
    return html.escape(s[:MAX_CHARS])


def main():
    demo_lines = run(["demo", "--quiet"]).strip().split("\n")
    gap_lines = run(["realism-gap", "--seconds", "60", "--seed", "7"]).strip().split("\n")

    scenes = [
        ("$ kairos demo", demo_lines),
        ("$ kairos realism-gap", gap_lines),
    ]

    css = """
    <style>
    text { font-family: "DejaVu Sans Mono", "JetBrains Mono", Menlo, Consolas, monospace;
           font-size: 14px; fill: #c9d4e3; }
    .prompt { fill: #4ade80; font-weight: bold; }
    .headline { fill: #fbbf24; font-weight: bold; }
    .dim { fill: #64748b; }
    .line { opacity: 0; animation: reveal 0.01s forwards; }
    @keyframes reveal { to { opacity: 1; } }
    .cursor { animation: blink 1.1s steps(1) infinite; }
    @keyframes blink { 50% { opacity: 0; } }
    </style>"""

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}">',
             css,
             f'<rect width="{W}" height="{H}" rx="10" fill="#0b0f17"/>',
             f'<rect width="{W}" height="36" rx="10" fill="#111827"/>',
             f'<rect y="26" width="{W}" height="10" fill="#111827"/>',
             '<circle cx="24" cy="18" r="6" fill="#f87171"/>',
             '<circle cx="44" cy="18" r="6" fill="#fbbf24"/>',
             '<circle cx="64" cy="18" r="6" fill="#4ade80"/>',
             f'<text x="{W - PAD_X}" y="23" font-size="12" text-anchor="end" '
             f'class="dim">kairos — live terminal cast</text>']

    y = PAD_Y
    t = 0.6  # start delay
    step = 0.5
    for prompt, lines in scenes:
        parts.append(
            f'<text x="{PAD_X}" y="{y}" class="line prompt" '
            f'style="animation-delay:{t:.1f}s">{esc(prompt)}</text>')
        y += LH
        t += step
        for ln in lines:
            if not ln.strip():
                y += LH * 0.5
                continue
            cls = "headline" if ln.startswith("->") else ""
            cls_attr = f' class="line {cls}"' if cls else ' class="line"'
            parts.append(
                f'<text x="{PAD_X}" y="{y}"{cls_attr} '
                f'style="animation-delay:{t:.1f}s">{esc(ln)}</text>')
            y += LH
            t += step * 0.55
        y += LH * 1.2
        t += 0.8

    # Blinking cursor at the end.
    parts.append(
        f'<rect x="{PAD_X}" y="{y - 14}" width="9" height="17" '
        f'fill="#4ade80" class="cursor" style="animation-delay:{t:.1f}s"/>')
    parts.append("</svg>")
    out = "\n".join(parts) + "\n"

    dest = os.path.join(REPO, "docs", "demo.svg")
    with open(dest, "w") as f:
        f.write(out)
    print(f"wrote {dest} ({len(out)} bytes, {t:.0f}s cast)")


if __name__ == "__main__":
    main()
