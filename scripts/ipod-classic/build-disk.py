#!/usr/bin/env python3
"""Assemble a Classic disk from locally supplied, decrypted 2.0.4 images.

No firmware is downloaded or bundled. See docs/system/arm/ipod-classic.rst
for the two directory offset conventions and the boot ROM prerequisites.
"""
import argparse
import hashlib
import pathlib
import shutil
import struct
import subprocess
import tempfile

BLOCK = 4096
SECTOR = 512
FW_START = 0x8000
DIRECTORY = 0x5000
OSOS_SHA256 = 'f4368251a58b2fdc7b46acf3178dae1d24bc1e029736240741015851256c65c4'


def align(value, unit=BLOCK):
    return (value + unit - 1) // unit * unit


def copy_sparse(source, dest, limit):
    """Copy a bounded extent without materializing an entire disk in memory."""
    remaining = limit
    while remaining:
        data = source.read(min(1024 * 1024, remaining))
        if not data:
            raise ValueError('short input image')
        if data.count(0) == len(data):
            dest.seek(len(data), 1)
        else:
            dest.write(data)
        remaining -= len(data)


def build(args):
    osos = args.firmware_dir / 'osos.fw.decrypted'
    digest = hashlib.sha256()
    with osos.open('rb') as source:
        for data in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(data)
    with osos.open('rb') as source:
        # EFI authenticates the 0x40-byte header. With fused-key identity
        # transfers, its stored tag is the first 16 bytes of this SHA-1.
        osos_mac = hashlib.sha1(source.read(0x40)).digest()[:16]
    if digest.hexdigest() != OSOS_SHA256:
        raise ValueError('this layout and header MAC require the verified '
                         'decrypted 2.0.4 osos image; see the documentation')
    if args.preferences:
        preferences = args.preferences.read_bytes()
        if (len(preferences) != 0xb8c or
                struct.unpack_from('<II', preferences) != (0x3e, 0)):
            raise ValueError('expected a 2.0.4 Preferences file (2956 bytes, '
                             'version 0x3e)')
    names = [('osos', 'osos.fw.decrypted'), ('rsrc', 'rsrc.fw'),
             ('aupd', 'aupd.fw.decrypted'), ('hash', 'hash.fw')]
    entries = []
    offset = 0x6000
    for name, filename in names:
        path = args.firmware_dir / filename
        length = path.stat().st_size
        skip, entry_offset, placement = 0, 0, offset
        if name == 'rsrc':
            # Runtime reads devOffset + one device block. The update image
            # has an 0x800-byte wrapper and a FAT VBR at file offset 0x1e00.
            skip = 0x800
            length -= skip
            placement += BLOCK
            entry_offset = 0x1600
            with path.open('rb') as source:
                source.seek(0x1e00)
                boot = source.read(512)
            if length % BLOCK or boot[510:512] != b'\x55\xaa':
                raise ValueError('rsrc layout does not match firmware 2.0.4')
        entries.append((name, path, offset, placement, length, skip,
                        entry_offset))
        offset = align(placement + length)
    fw_size = offset
    data_start = align(FW_START + fw_size, 32768)
    # retailOS locates the data view at the end of the firmware partition.
    # Include alignment padding in its extent; a gap hides the FAT VBR.
    fw_size = data_start - FW_START

    with tempfile.TemporaryDirectory(prefix='ipod-disk-') as temp:
        data_path = pathlib.Path(temp) / 'data.img'
        data_size = args.data_size_mib * 1024 * 1024
        with data_path.open('wb') as data:
            data.truncate(data_size)
        subprocess.run(['mkfs.vfat', '-F', '32', '-S', str(BLOCK), '-s', '1',
                        '-R', '32', '-n', 'IPOD', str(data_path)], check=True)
        subprocess.run(['mmd', '-i', str(data_path), '::/iPod_Control',
                        '::/iPod_Control/Device', '::/iPod_Control/iTunes',
                        '::/iPod_Control/Music'], check=True)
        if args.preferences:
            subprocess.run(['mcopy', '-i', str(data_path),
                            str(args.preferences),
                            '::/iPod_Control/Device/Preferences'], check=True)
        # retailOS's volume classifier probes this reserved BPB field before
        # invoking the FAT32 driver. Both primary and backup VBR need it.
        with data_path.open('r+b') as data:
            boot = data.read(BLOCK)
            backup = struct.unpack_from('<H', boot, 50)[0]
            for block in (0, backup):
                data.seek(block * BLOCK + 0x34)
                data.write(b'FAT')
        total_size = data_start + data_size + 32768
        header = bytearray(SECTOR)
        for index, kind, start, size in ((0, 0, FW_START, fw_size),
                                         (1, 0x0b, data_start, data_size)):
            off = 0x1be + index * 16
            header[off + 4] = kind
            struct.pack_into('<II', header, off + 8,
                             start // BLOCK, size // BLOCK)
        header[510:512] = b'\x55\xaa'
        # Exclusive creation keeps a populated disk safe from an accidental
        # second invocation. Choose a new output name to build another disk.
        with args.output.open('xb') as disk:
            disk.truncate(total_size)
            disk.write(header)
            disk.seek(FW_START)
            disk.write(args.stop.read_bytes()[:256])
            disk.seek(FW_START + 0x100)
            disk.write(struct.pack('<IIHH', 0x5b68695d, 0x4000, 0x10c, 3))
            for index, (name, path, dev_offset, placement, length,
                        skip, entry_offset) in enumerate(entries):
                disk.seek(FW_START + DIRECTORY + index * 40)
                disk.write(struct.pack('<4s4s8I', b'!ATA',
                           name[::-1].encode('ascii'), int(name == 'aupd'),
                           dev_offset, length, 0x08000000, entry_offset,
                           0, 0x15000, 0xffffffff))
                disk.seek(FW_START + placement)
                with path.open('rb') as source:
                    source.seek(skip)
                    copy_sparse(source, disk, length)
                if name == 'osos':
                    disk.seek(FW_START + placement + 0x40)
                    disk.write(osos_mac)
            disk.seek(data_start)
            with data_path.open('rb') as source:
                copy_sparse(source, disk, data_size)
    print(f'Created {args.output}; data partition offset {data_start} bytes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-dir', type=pathlib.Path, required=True)
    parser.add_argument('--stop', type=pathlib.Path, required=True,
                        help='locally supplied firmware partition STOP header')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--data-size-mib', type=int, default=512)
    parser.add_argument('--preferences', type=pathlib.Path,
                        help='existing 2.0.4 Preferences file to restore')
    args = parser.parse_args()
    if args.data_size_mib < 512:
        parser.error('data size must be >= 512 MiB')
    if not all(shutil.which(tool) for tool in ('mkfs.vfat', 'mmd', 'mcopy')):
        parser.error('dosfstools and mtools are required')
    build(args)


if __name__ == '__main__':
    main()
