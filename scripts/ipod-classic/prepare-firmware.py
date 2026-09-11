#!/usr/bin/env python3
"""Map the unmodified decrypted 2.0.4 image for static analysis.

Import dram.bin at 0x08000000 and iram.bin at 0x22000000 as ARM little-endian
ARMv5T code. The latter contains the low-vector table and RTOS interrupt code.
"""
import argparse
import pathlib
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('osos', type=pathlib.Path)
parser.add_argument('output', type=pathlib.Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
with args.osos.open('rb') as source:
    if source.read(4) != b'8702':
        parser.error('expected the 0x800-byte wrapped decrypted osos image')
    source.seek(0x800)
    with (args.output / 'iram.bin').open('xb') as output:
        iram = source.read(0xaed8)
        if len(iram) != 0xaed8:
            parser.error('short osos image')
        output.write(iram)
    with (args.output / 'dram.bin').open('xb') as output:
        shutil.copyfileobj(source, output, 1024 * 1024)
