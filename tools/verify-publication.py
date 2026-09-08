#!/usr/bin/env python3
"""Inspect the actual local Maven publication, including the native Android payload."""
import argparse
import hashlib
import io
import json
import pathlib
import xml.etree.ElementTree as ET
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--version', default='0.1.0-SNAPSHOT')
parser.add_argument('--group', default='io.github.moriya-taichi')
parser.add_argument('--release', action='store_true', help='Also require license metadata and signatures')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
repo = root / 'vulkano/build/repository'
folder = repo / args.group.replace('.', '/') / 'vulkano' / args.version
# Snapshot repositories use timestamped names; read the POM to select the published build.
poms = sorted(folder.glob('*.pom'), key=lambda p: p.stat().st_mtime)
assert poms, f'No publication in {folder}'
pom = poms[-1]
stem = pom.stem
ns = {'m': 'http://maven.apache.org/POM/4.0.0'}
xml = ET.parse(pom).getroot()
for field, expected in [('groupId', args.group), ('artifactId', 'vulkano'), ('version', args.version), ('packaging', 'aar')]:
    assert xml.findtext('m:' + field, namespaces=ns) == expected, field
for field in ['name', 'description', 'url', 'scm/connection', 'developers/developer/id']:
    assert xml.findtext('/'.join('m:' + part for part in field.split('/')), namespaces=ns), field
assert any(d.findtext('m:artifactId', namespaces=ns) == 'kotlin-stdlib'
           for d in xml.findall('m:dependencies/m:dependency', ns)), 'Kotlin transitive dependency missing'
artifacts = [pom, folder / (stem + '.aar'), folder / (stem + '-sources.jar'),
             folder / (stem + '-javadoc.jar'), folder / (stem + '.module')]
for file in artifacts:
    assert file.is_file() and file.stat().st_size > 0, file
    for algorithm in ['md5', 'sha1']:
        checksum = pathlib.Path(str(file) + '.' + algorithm)
        assert checksum.read_text().strip() == hashlib.new(algorithm, file.read_bytes()).hexdigest(), checksum
    if args.release:
        assert pathlib.Path(str(file) + '.asc').is_file(), f'Missing signature: {file}'
with zipfile.ZipFile(artifacts[1]) as aar:
    for abi in ['arm64-v8a', 'x86_64']:
        assert f'jni/{abi}/libvulkano.so' in aar.namelist(), abi
    with zipfile.ZipFile(io.BytesIO(aar.read('classes.jar'))) as classes:
        assert 'dev/vulkano/Device.class' in classes.namelist()
        assert 'META-INF/licenses/vulkano/VMA-LICENSE.txt' in classes.namelist()
        assert 'META-INF/licenses/vulkano/spirv-reflect/LICENSE' in classes.namelist()
        assert classes.read('META-INF/licenses/vulkano/Vulkano-LICENSE') == (root / 'LICENSE').read_bytes()
        assert 'META-INF/licenses/vulkano/SPIRV-Headers-LICENSE.txt' in classes.namelist()
        assert 'META-INF/licenses/vulkano/vulkan-headers/LICENSES/Apache-2.0.txt' in classes.namelist()
        assert 'META-INF/licenses/vulkano/vulkan-headers/LICENSES/MIT.txt' in classes.namelist()
with zipfile.ZipFile(artifacts[2]) as sources:
    assert any(n.endswith('/Device.kt') for n in sources.namelist())
    assert 'engine.cpp' in sources.namelist(), 'Native sources missing'
    assert sources.read('META-INF/licenses/vulkano/LICENSE') == (root / 'LICENSE').read_bytes()
with zipfile.ZipFile(artifacts[3]) as docs:
    assert 'index.html' in docs.namelist(), 'Dokka documentation missing'
    assert any('/-device/' in n for n in docs.namelist()), 'Device API documentation missing'
metadata = json.loads(artifacts[4].read_text())
assert metadata['component']['group'] == args.group
assert metadata['component']['version'] == args.version
if args.release:
    assert not args.version.endswith('-SNAPSHOT')
assert xml.findtext('m:licenses/m:license/m:name', namespaces=ns) == 'Apache License, Version 2.0'
assert xml.findtext('m:licenses/m:license/m:url', namespaces=ns) == 'https://www.apache.org/licenses/LICENSE-2.0.txt'
bundle = root / f'vulkano/build/distributions/vulkano-{args.version}-maven.zip'
with zipfile.ZipFile(bundle) as archive:
    for file in artifacts:
        name = 'vulkano-repository/' + file.relative_to(repo).as_posix()
        assert archive.read(name) == file.read_bytes(), name
    assert all(n.startswith('vulkano-repository/' + args.group.replace('.', '/') + '/vulkano/' + args.version + '/')
               or n.endswith('/') for n in archive.namelist()), 'Unrelated versions in archive'
print(f'Publication verified: {args.group}:vulkano:{args.version} (AAR, POM, module, sources, Dokka, checksums, ZIP)')
