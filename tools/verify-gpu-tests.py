#!/usr/bin/env python3
"""Summarize JNI/GPU JUnit results and reject Vulkan validation errors."""
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

reports = list(Path("vulkano/build/test-results/testDebugUnitTest").glob("TEST-*.xml"))
if not reports:
    raise SystemExit("No Kotlin/JNI test reports were produced")
total = skipped = failed = 0
validation = []
for report in sorted(reports):
    root = ET.parse(report).getroot()
    for test in root.findall("testcase"):
        total += 1
        name = f"{test.attrib['classname']}.{test.attrib['name']}"
        if test.find("skipped") is not None:
            skipped += 1
            print("SKIPPED:", name)
        elif test.find("failure") is not None or test.find("error") is not None:
            failed += 1
            print("FAILED:", name)
        else:
            print("PASSED:", name)
    for node in list(root.iter("system-out")) + list(root.iter("system-err")):
        for line in (node.text or "").splitlines():
            if "Validation Error" in line or "SYNC-HAZARD" in line:
                validation.append(line)
print(f"Tests: {total}, passed: {total - skipped - failed}, skipped: {skipped}, failures: {failed}")
# JVM native stdout bypasses Gradle's per-test capture, so XML alone misses VUIDs.
log = Path("build/gradle-gpu.log")
if not log.is_file():
    raise SystemExit("The complete Gradle/JNI log is required for Vulkan validation")
for line in log.read_text(errors="replace").splitlines():
    if "Validation Error" in line or "SYNC-HAZARD" in line:
        validation.append(line)
for line in validation:
    print(line)
sys.exit(1 if failed or validation or total == 0 else 0)
