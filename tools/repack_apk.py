#!/usr/bin/env python3
"""Recompresses an unsigned APK with pigz at LEVEL: 11 is Zopfli, the strongest deflate, for
releases; 9 is fast, for development builds. Android reads only stored and deflate entries.
Deflated entries and the native libraries (extractNativeLibs is true, see
patches/tennessine/smaller-apk.patch) are recompressed; stored ones stay stored, as Chromium
reads them in place. zipalign and signing come after.

The entries compress in parallel: most are small, and one pigz at a time spent most of the
time starting processes. The output is the same as compressing them one after another.

Usage: repack_apk.py IN OUT [LEVEL]   (default 11)
"""
import os, subprocess, sys, zipfile
from concurrent.futures import ThreadPoolExecutor

CPUS = os.cpu_count()


def deflate(data, level):
    """The raw deflate stream of data. A big entry (libchrome.so) gets every CPU; pigz splits it
    into blocks."""
    threads = CPUS if len(data) > 8 << 20 else 1
    out = subprocess.run(['pigz', f'-{level}', '-z', '-p', str(threads), '-c'], input=data,
                         capture_output=True, check=True).stdout
    return out[2:-4]  # The raw deflate stream inside the zlib header and Adler-32.


class Precompressed:
    """A compressor for zipfile that returns a stream deflated beforehand."""

    def __init__(self, stream):
        self.stream = stream

    def compress(self, data):
        return b''

    def flush(self):
        return self.stream


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    level = sys.argv[3] if len(sys.argv) == 4 else '11'
    with zipfile.ZipFile(sys.argv[1]) as src:
        entries = []
        for info in src.infolist():
            deflate_it = info.compress_type == zipfile.ZIP_DEFLATED or (
                info.filename.startswith('lib/') and info.filename.endswith('.so'))
            entries.append((info, deflate_it, src.read(info)))
    with ThreadPoolExecutor(CPUS) as pool:
        streams = [pool.submit(deflate, data, level) if deflate_it else None
                   for _, deflate_it, data in entries]
        stock = zipfile._get_compressor
        with zipfile.ZipFile(sys.argv[2], 'w') as dst:
            for (info, deflate_it, data), stream in zip(entries, streams):
                out = zipfile.ZipInfo(info.filename, info.date_time)
                out.external_attr = info.external_attr
                out.compress_type = zipfile.ZIP_DEFLATED if deflate_it else zipfile.ZIP_STORED
                if deflate_it:
                    result = stream.result()
                    zipfile._get_compressor = lambda kind, level=None: Precompressed(result)
                dst.writestr(out, data)
                zipfile._get_compressor = stock


if __name__ == '__main__':
    main()
