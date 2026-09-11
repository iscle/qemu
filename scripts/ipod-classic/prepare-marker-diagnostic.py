#!/usr/bin/env python3
"""Create a private disk with ONE structurally reconstructed firmware word.

This is an explicit diagnostic workaround, not recovered original plaintext.
No emulator device, source firmware, normal disk, or launcher is modified.
"""
import argparse
import errno
import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--firmware', type=Path, required=True,
                    help='original supplied decrypted 2.0.4 osos image')
parser.add_argument('--source-disk', type=Path, required=True,
                    help='stopped disk prepared with build-disk.py')
parser.add_argument('--source-nor', type=Path, required=True,
                    help='stopped writable NOR image')
parser.add_argument('--output-dir', type=Path, required=True,
                    help='new directory for the diagnostic copies and manifest')
args = parser.parse_args()
base = args.output_dir
original = args.firmware
firmware = original.read_bytes()
original_hash = hashlib.sha256(firmware).hexdigest()
assert original_hash == ('f4368251a58b2fdc7b46acf3178dae1d24'
                         'bc1e029736240741015851256c65c4')
disk_source = args.source_disk
disk_output = base / 'disk-marker-reconstructed.img'
nor_source = args.source_nor
nor_output = base / 'nor-diagnostic.bin'
disk_osos = 0xe000
marker = 0xa1bde0
with disk_source.open('rb') as source:
    source.seek(0xd000)
    entry = struct.unpack('<4s4s8I', source.read(40))
    assert entry[:2] == (b'!ATA', b'soso')
    assert entry[3] + 0x8000 == disk_osos
    assert entry[4] == len(firmware)
    source.seek(disk_osos)
    disk_firmware = source.read(len(firmware))
expected_disk_firmware = bytearray(firmware)
expected_disk_firmware[0x40:0x50] = hashlib.sha1(firmware[:0x40]).digest()[:16]
assert disk_firmware == expected_disk_firmware
assert firmware[marker:marker + 4] == bytes.fromhex('3c7c7c5a')


def exclusive_sparse_copy(source_path, output_path):
    with source_path.open('rb') as source, output_path.open('xb') as output:
        try:
            fcntl.ioctl(output.fileno(), 0x40049409, source.fileno())  # FICLONE
        except OSError as error:
            if error.errno not in (errno.EXDEV, errno.EINVAL, errno.ENOTTY,
                                   errno.EOPNOTSUPP):
                raise
            while chunk := source.read(1024 * 1024):
                if not any(chunk):
                    output.seek(len(chunk), 1)
                else:
                    output.write(chunk)
            output.truncate(source.tell())


base.mkdir()
exclusive_sparse_copy(disk_source, disk_output)
with nor_source.open('rb') as source, nor_output.open('xb') as output:
    shutil.copyfileobj(source, output, 1024 * 1024)
with disk_output.open('r+b') as disk:
    disk.seek(disk_osos + marker)
    disk.write(bytes.fromhex('73190613'))
    disk.seek(disk_osos)
    reconstructed = disk.read(len(firmware))
changed = [index for index, (before, after) in
           enumerate(zip(disk_firmware, reconstructed)) if before != after]
assert changed == list(range(marker, marker + 4))
assert hashlib.sha256(original.read_bytes()).hexdigest() == original_hash
manifest = {
    'status': 'STRUCTURAL RECONSTRUCTION: NOT VERIFIED ORIGINAL PLAINTEXT',
    'purpose': 'Private game diagnosis through normal boot without a debugger',
    'original_firmware': str(original),
    'original_firmware_sha256': original_hash,
    'source_disk': str(disk_source),
    'source_osos_extent_sha256': hashlib.sha256(disk_firmware).hexdigest(),
    'output_disk': str(disk_output),
    'initial_output_osos_extent_sha256':
        hashlib.sha256(reconstructed).hexdigest(),
    'disk_osos_offset': hex(disk_osos),
    'firmware_word_offset': hex(marker),
    'disk_word_offset': hex(disk_osos + marker),
    'guest_word_address': '0x08a10708',
    'before_le_hex': '3c7c7c5a',
    'after_le_hex': '73190613',
    'evidence': ('Original module loader 0x080e9140 checks 0x13061973; '
                 'all preceding export tables have that closing marker. '
                 'Bootstrap copies this word as the last four DRAM bytes.'),
    'limitations': ('The final 12 bytes remain unchanged and damaged. '
                    'This is not a complete AES-block recovery. The normal '
                    'disk, firmware inputs, launcher and QEMU code are '
                    'unchanged.'),
}
with (base / 'marker-reconstruction.json').open('x') as output:
    json.dump(manifest, output, indent=2)
    output.write('\n')
print(json.dumps(manifest, indent=2))
