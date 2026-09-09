#!/usr/bin/env python3
"""Compile Vulkan 1.1 fixtures; ray/mesh stages require Vulkan 1.2 / SPIR-V 1.4+."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--glslc", default=os.environ.get("GLSLC", shutil.which("glslc")))
parser.add_argument("--spirv-as", default=os.environ.get("SPIRV_AS", shutil.which("spirv-as")))
args = parser.parse_args()
if not args.glslc:
    parser.error("Pass --glslc <NDK>/shader-tools/<host>/glslc, or install glslc")
root = Path(__file__).resolve().parents[1]
output = root / "tests/shaders"
if not args.spirv_as:
    parser.error("Pass --spirv-as <path> or install SPIRV-Tools")
for source in sorted(output.glob("*.spvasm")):
    subprocess.run([args.spirv_as, "--target-env", "vulkan1.2", str(source), "-o",
                    str(source.with_suffix(".spv"))], check=True)
for folder in [root / "sample/src/main/shaders", output]:
    for source in sorted(folder.iterdir()):
        if source.suffix in {".comp", ".vert", ".frag", ".tesc", ".tese", ".mesh", ".task", ".rgen", ".rmiss", ".rchit"}:
            subprocess.run([args.glslc, "--target-env=vulkan1.2" if source.name in {"query.comp", "cooperative.comp"} or source.suffix in {".mesh", ".task", ".rgen", ".rmiss", ".rchit"} else "--target-env=vulkan1.1", str(source), "-o", str(output / (source.name + ".spv"))], check=True)
