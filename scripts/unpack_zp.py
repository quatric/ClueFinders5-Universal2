#!/usr/bin/env python3
"""Unpack the app from an InstallerMaker (Mill) Archive.zp.

usage: unpack_zp.py <manifest.plist> <Archive.zp> <output dir>

The manifest is Archive.zplist (zlib-compressed NSArchiver typedstream)
converted to an XML plist. Each "pack" task lists its files; each file's
data fork is a run of zlib streams in Archive.zp (or raw bytes plus a
9-byte trailer for segments that compression didn't shrink).
"""
import os
import plistlib
import sys
import zlib


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    manifest = plistlib.load(open(sys.argv[1], 'rb'))
    archive = open(sys.argv[2], 'rb')
    out = sys.argv[3]
    objects = manifest['YAAllObjectsPropertiesKey']
    count = 0
    for task in manifest['YAAllTasksRepresentationsKey']:
        if task.get('PluginClassName') != 'YAPackTask':
            continue
        key = task['PluginClassProperties']['YAPluginClassUniquieKey']
        base = os.path.join(out, task['kInstallSubpathKey'])
        for entry in objects[key]['FilesList']:
            attrs = entry['Attributes']
            path = os.path.join(base, entry['EntryPath']) if entry['EntryPath'] else base
            if os.path.relpath(path, out).startswith('..'):
                raise SystemExit('unsafe path in archive: %r' % entry['EntryPath'])
            if attrs['NSFileType'] == 'NSFileTypeDirectory':
                os.makedirs(path, exist_ok=True)
                continue
            if attrs['NSFileType'] != 'NSFileTypeRegular':
                print('skipping %s (%s)' % (path, attrs['NSFileType']))
                continue
            data = bytearray()
            for fork in entry.get('Forks', []):
                if fork.get('ForkName'):
                    print('skipping %s fork of %s' % (fork['ForkName'], path))
                    continue
                for seg in fork['Segments']:
                    archive.seek(seg['OffsLO'] + (seg.get('OffsHI', 0) << 32))
                    chunk = archive.read(seg['Length'])
                    try:
                        data += zlib.decompress(chunk)
                    except zlib.error:
                        # segments that don't shrink are stored as is, with a
                        # 9-byte trailer
                        data += chunk[:attrs['NSFileSize'] - len(data)]
            if len(data) != attrs['NSFileSize']:
                raise SystemExit('%s: got %d bytes, expected %d' % (path, len(data), attrs['NSFileSize']))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as f:
                f.write(data)
            os.chmod(path, attrs.get('NSFilePosixPermissions', 0o644) & 0o755)
            count += 1
    print('unpacked %d files' % count)


if __name__ == '__main__':
    main()
