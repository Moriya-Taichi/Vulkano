#!/usr/bin/env python3
"""Fail on a Vulkan validation error as well as a failing native assertion."""
import argparse
import os
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("executable")
parser.add_argument("--no-validation", action="store_true")
args = parser.parse_args()
env = os.environ.copy()
if not args.no_validation:
    env["VULKANO_VALIDATION"] = "1"
result = subprocess.run([args.executable], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
print(result.stdout, end="")
if result.returncode or "Validation Error" in result.stdout or "SYNC-HAZARD" in result.stdout:
    sys.exit(1)
