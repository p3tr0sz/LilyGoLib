#!/usr/bin/env python3
"""Build the web desktop (examples/factory/web) into an SD-card bundle.

The bundle is the source files plus a .gz twin for each text file (the watch sends the .gz with
Content-Encoding: gzip) and a manifest.json listing every file with its size and SHA-256.

    python3 web_build.py --out <dir>          build into <dir> (replaced)
    python3 web_build.py --install <sd-root>  build into <sd-root>/web.new, then swap it in as <sd-root>/web
                                              (the old desktop stays until the new one is complete)
    python3 web_build.py --zip <dir>          the starter SD-card ZIP, <dir>/twatch-sd-<version>.zip: web/ (the desktop),
                                              the empty data/ folders and a README. Unpack it to the card root, or
                                              choose it on the watch's /rescue page
    python3 web_build.py --rescue             regenerate the two headers built into the firmware, and commit them:
                                              ../web_rescue.h   the rescue page (from ../web-rescue/index.html)
                                              ../web_desktop_pin.h  the desktop version this firmware wants, and the
                                              size and SHA-256 of its ZIP: the watch installs only that exact ZIP
                                              when it downloads one
    --version X  (with --zip or --out) build as version X instead of web/manifest.json's, for tests

<sd-root> is a mounted SD card, or the simulator's sim_sd folder.
"""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, '..', 'web'))
RESCUE_SRC = os.path.normpath(os.path.join(HERE, '..', 'web-rescue', 'index.html'))
RESCUE_HDR = os.path.normpath(os.path.join(HERE, '..', 'web_rescue.h'))
RESCUE_BUDGET = 60 * 1024
PIN_HDR = os.path.normpath(os.path.join(HERE, '..', 'web_desktop_pin.h'))
VERSION_OVERRIDE = None
GZIP_EXT = {'.html', '.css', '.js', '.mjs', '.json', '.svg', '.txt', '.webmanifest', '.md'}
GZIP_MIN = 256


def build(out):
    if os.path.exists(out):
        shutil.rmtree(out)
    with open(os.path.join(SRC, 'manifest.json'), encoding='utf-8') as f:
        manifest = json.load(f)
    if VERSION_OVERRIDE:
        manifest['version'] = VERSION_OVERRIDE
    files = {}
    total = gz_total = 0
    for root, dirs, names in os.walk(SRC):
        dirs[:] = sorted(d for d in dirs if not d.startswith('.'))
        for name in sorted(names):
            if name.startswith('.') or name == 'manifest.json':
                continue
            src = os.path.join(root, name)
            rel = os.path.relpath(src, SRC).replace(os.sep, '/')
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(src, 'rb') as f:
                data = f.read()
            with open(dst, 'wb') as f:
                f.write(data)
            files[rel] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            total += len(data)
            if os.path.splitext(name)[1].lower() in GZIP_EXT and len(data) >= GZIP_MIN:
                packed = gzip.compress(data, compresslevel=9, mtime=0)
                with open(dst + '.gz', 'wb') as f:
                    f.write(packed)
                gz_total += len(packed)
            else:
                gz_total += len(data)
    manifest['files'] = files
    with open(os.path.join(out, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    return manifest, total, gz_total


DATA_DIRS = ['data', 'data/ir', 'data/gnss', 'data/nfc', 'data/recordings', 'data/lora']
README = """T-Watch Ultra - SD card starter, desktop {version} (web API {api})

Two ways to use this ZIP:

1. With a card reader: unpack it to the root of a FAT32 card (up to 32 GB), so that the
   card has web/ and data/ at the top. Put the card in the watch.
2. Without one: on the watch, open Settings > Web desktop and turn it on. In a browser,
   open the address shown there, followed by /rescue (for example http://watch.local/rescue).
   Pair with the PIN, then choose this ZIP under "Web desktop".

web/   the web desktop the watch serves to your browser
data/  your files and what the watch's tools save (IR codes, GNSS tracks, NFC dumps,
       recordings, LoRa logs)
"""
# A fixed date inside the ZIP, so the same sources make the same ZIP.
ZIP_DATE = (2026, 1, 1, 0, 0, 0)


def make_zip(out_dir, quiet=False):
    with tempfile.TemporaryDirectory() as tmp:
        web = os.path.join(tmp, 'web')
        manifest, total, gz_total = build(web)
        name = f"twatch-sd-{manifest.get('version', '0')}.zip"
        os.makedirs(out_dir, exist_ok=True)
        path = os.path.join(out_dir, name)

        def add(zf, arcname, data):
            info = zipfile.ZipInfo(arcname, ZIP_DATE)
            # Stored, not deflated: the same bytes on every machine (zlib versions differ), so the
            # firmware can pin the ZIP's SHA-256. The files worth compressing ship as .gz twins anyway.
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = (0o40755 << 16) | 0x10 if arcname.endswith('/') else 0o644 << 16
            zf.writestr(info, data)

        with zipfile.ZipFile(path, 'w') as zf:
            add(zf, 'README.txt', README.format(version=manifest.get('version'), api=manifest.get('api')))
            for d in DATA_DIRS:
                add(zf, d + '/', b'')
            add(zf, 'web/', b'')
            for root, dirs, names in os.walk(web):
                dirs.sort()
                rel_root = os.path.relpath(root, tmp).replace(os.sep, '/')
                for d in dirs:
                    add(zf, f'{rel_root}/{d}/', b'')
                for n in sorted(names):
                    with open(os.path.join(root, n), 'rb') as f:
                        add(zf, f'{rel_root}/{n}', f.read())
    size = os.path.getsize(path)
    if not quiet:
        print(f"SD starter {manifest.get('version')} (API {manifest.get('api')}): {size / 1024:.0f} KB -> {path}")
    return path


def pin_header():
    with tempfile.TemporaryDirectory() as tmp:
        path = make_zip(tmp, quiet=True)
        with open(path, 'rb') as f:
            data = f.read()
    with open(os.path.join(SRC, 'manifest.json'), encoding='utf-8') as f:
        version = json.load(f)['version']
    name = os.path.basename(path)
    out = f'''/* Generated by tools/web_build.py --rescue. Do not edit.
 * The web desktop this firmware wants, and the exact ZIP it accepts when it downloads one. */
#pragma once

#define WEB_DESKTOP_VERSION     "{version}"
#define WEB_DESKTOP_ZIP_NAME    "{name}"
#define WEB_DESKTOP_ZIP_SIZE    {len(data)}
#define WEB_DESKTOP_ZIP_SHA256  "{hashlib.sha256(data).hexdigest()}"
'''
    with open(PIN_HDR, 'w', encoding='utf-8') as f:
        f.write(out)
    print(f'desktop pin: {name}, {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]}… -> {PIN_HDR}')


def rescue_header():
    with open(RESCUE_SRC, 'rb') as f:
        data = f.read()
    packed = gzip.compress(data, compresslevel=9, mtime=0)
    if len(packed) > RESCUE_BUDGET:
        sys.exit(f'rescue page is {len(packed)} bytes gzipped, over the {RESCUE_BUDGET} byte budget')
    lines = []
    for i in range(0, len(packed), 16):
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in packed[i:i + 16]) + ',')
    body = '\n'.join(lines)
    out = f'''/* Generated by tools/web_build.py --rescue from web-rescue/index.html. Do not edit. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* The rescue page, gzipped ({len(data)} bytes as HTML). sha256 {hashlib.sha256(data).hexdigest()[:16]} */
static const uint8_t WEB_RESCUE_HTML_GZ[] = {{
{body}
}};
static const size_t WEB_RESCUE_HTML_GZ_LEN = sizeof(WEB_RESCUE_HTML_GZ);
'''
    with open(RESCUE_HDR, 'w', encoding='utf-8') as f:
        f.write(out)
    print(f'rescue page: {len(data)} bytes, {len(packed)} gzipped -> {RESCUE_HDR}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = p.add_mutually_exclusive_group()
    g.add_argument('--out', help='build into this folder')
    g.add_argument('--install', metavar='SD_ROOT', help='build and install as <SD_ROOT>/web')
    g.add_argument('--zip', metavar='DIR', help='write the starter SD-card ZIP into DIR')
    p.add_argument('--rescue', action='store_true', help='regenerate web_rescue.h and web_desktop_pin.h')
    p.add_argument('--version', help='build as this version (tests)')
    a = p.parse_args()
    global VERSION_OVERRIDE
    VERSION_OVERRIDE = a.version
    if not (a.out or a.install or a.zip or a.rescue):
        p.error('give --out, --install, --zip or --rescue')
    if a.rescue:
        if a.version:
            p.error('--rescue pins the real version; drop --version')
        rescue_header()
        pin_header()
    if a.zip:
        make_zip(a.zip)
        return
    if not (a.out or a.install):
        return

    if a.out:
        manifest, total, gz_total = build(a.out)
        where = a.out
    else:
        root = a.install
        if not os.path.isdir(root):
            sys.exit(f'{root} is not a folder')
        new, cur, old = (os.path.join(root, n) for n in ('web.new', 'web', 'web.old'))
        manifest, total, gz_total = build(new)
        if os.path.exists(old):
            shutil.rmtree(old)
        if os.path.exists(cur):
            os.rename(cur, old)
        os.rename(new, cur)
        if os.path.exists(old):
            shutil.rmtree(old)
        where = cur
    print(f"desktop {manifest.get('version')} (API {manifest.get('api')}): {len(manifest['files'])} files, "
          f"{total / 1024:.0f} KB, {gz_total / 1024:.0f} KB as served -> {where}")


if __name__ == '__main__':
    main()
