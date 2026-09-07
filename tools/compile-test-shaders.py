#!/usr/bin/env python3
"""Regenerate test fixtures with the NDK's glslc (SPIR-V 1.3 / Vulkan 1.1)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--glslc", default=os.environ.get("GLSLC", shutil.which("glslc")))
args = parser.parse_args()
if not args.glslc:
    parser.error("Pass --glslc <NDK>/shader-tools/<host>/glslc, or install glslc")
root = Path(__file__).resolve().parents[1]
output = root / "tests/shaders"
for folder in [root / "sample/src/main/shaders", output]:
    for source in sorted(folder.iterdir()):
        if source.suffix in {".comp", ".vert", ".frag"}:
            subprocess.run([args.glslc, "--target-env=vulkan1.1", str(source), "-o", str(output / (source.name + ".spv"))], check=True)
