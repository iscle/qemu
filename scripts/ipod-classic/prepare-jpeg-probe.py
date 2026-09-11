#!/usr/bin/env python3
"""Prepare generated JPEG inputs and a GDB probe for the original retailOS.

The generated probe calls the supplied 2.0.4 firmware's allocator, memory-input
JPEG decoder and deallocator from a normal task in a disposable restored VM.
It changes guest RAM and device state. It does not change firmware files or
replace the decoder. Run it explicitly in an attached GDB session; generation
alone does not connect to QEMU. Pillow is required to generate the inputs.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from PIL import Image, ImageDraw

OSOS_SHA256 = 'f4368251a58b2fdc7b46acf3178dae1d24bc1e029736240741015851256c65c4'
DRAM_FILE_OFFSET = 0xb6d8
DRAM_BASE = 0x08000000
# Align output dimensions to complete MCUs for this direct driver diagnostic.
# Higher image wrappers and partial-MCU destination contracts are not tested.
CASES = (
    ('square-420', 64, 64, 2),
    ('wide-420', 96, 64, 2),
    ('square-422', 64, 64, 1),
    ('square-444', 64, 64, 0),
)
PLANES = (('y', 0x20000, 1), ('cb', 0x80000, 4), ('cr', 0xa0000, 4))


def entry_elf(instructions):
    """Give GDB an entry point for inferior calls; this ELF is not loaded."""
    names = b'\0.text\0.shstrtab\0'
    shoff = (0x1040 + len(names) + 3) & ~3
    data = bytearray(shoff + 120)
    ident = b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8)
    data[:52] = ident + struct.pack(
        '<HHIIIIIHHHHHH', 2, 40, 1, DRAM_BASE, 52, shoff, 0x05000000,
        52, 32, 1, 40, 3, 2)
    data[52:84] = struct.pack('<IIIIIIII', 1, 0x1000, DRAM_BASE, DRAM_BASE,
                              64, 64, 5, 0x1000)
    data[0x1000:0x1040] = instructions[:64]
    data[0x1040:0x1040 + len(names)] = names
    data[shoff + 40:shoff + 80] = struct.pack(
        '<IIIIIIIIII', 1, 1, 6, DRAM_BASE, 0x1000, 64, 0, 0, 4, 0)
    data[shoff + 80:shoff + 120] = struct.pack(
        '<IIIIIIIIII', 7, 3, 0, 0, 0x1040, len(names), 0, 0, 1, 0)
    return data


def pattern(width, height):
    image = Image.new('RGB', (width, height))
    draw = ImageDraw.Draw(image)
    colors = ('red', 'green', 'blue', 'yellow', 'cyan', 'magenta',
              'white', 'black')
    for i, color in enumerate(colors):
        draw.rectangle((i * width // 8, 0,
                        (i + 1) * width // 8 - 1, height - 1), fill=color)
    draw.rectangle((width // 8, height // 4, 7 * width // 8, 3 * height // 4),
                   outline='white', width=2)
    draw.ellipse((width // 4, height // 4, 3 * width // 4, 3 * height // 4),
                 fill=(80, 90, 130))
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', required=True, type=Path,
                        help='original osos.fw.decrypted for retailOS 2.0.4')
    parser.add_argument('--output', required=True, type=Path,
                        help='new directory for local diagnostic artifacts')
    args = parser.parse_args()
    firmware = args.firmware.read_bytes()
    if hashlib.sha256(firmware).hexdigest() != OSOS_SHA256:
        parser.error('firmware hash does not match the verified original')
    output = args.output.resolve()
    if any(c in str(output) for c in '\n\r\0'):
        parser.error('output path contains a control character')
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        parser.error('output directory already exists; choose a new directory')
    dram = firmware[DRAM_FILE_OFFSET:]
    (output / 'original-entry.elf').write_bytes(entry_elf(dram))

    checks = []
    for address, length in ((DRAM_BASE, 64), (0x0807bff8, 0x4a4),
                            (0x080e07a4, 64), (0x080dca98, 64)):
        offset = address - DRAM_BASE
        digest = hashlib.sha256(dram[offset:offset + length]).hexdigest()
        checks.append((address, length, digest))
    cases = []
    for name, width, height, sampling in CASES:
        path = output / (name + '.jpg')
        pattern(width, height).save(path, quality=90, subsampling=sampling,
                                    progressive=False)
        cases.append({'name': name, 'width': width, 'height': height,
                      'subsampling': sampling, 'file': str(path),
                      'size': path.stat().st_size,
                      'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})

    # GDB's Python API avoids quoting generated input paths as GDB commands.
    script = [
        'set pagination off',
        'set architecture arm',
        'symbol-file ' + json.dumps(str(output / 'original-entry.elf')),
        'python',
        'import gdb, hashlib, json',
        'from pathlib import Path',
        'inferior = gdb.selected_inferior()',
        'pc = int(gdb.parse_and_eval("$pc"))',
        'cpsr = int(gdb.parse_and_eval("$cpsr"))',
        'sp = int(gdb.parse_and_eval("$sp"))',
        'if (pc != 0x080e07a4 or cpsr & 0x80 or',
        '        not 0x08000000 <= sp < 0x0c000000):',
        '    raise gdb.GdbError("Stop a normal task at allocator entry '
        'with IRQs enabled")',
        f'checks = {checks!r}',
        'for address, length, expected in checks:',
        '    actual = bytes(inferior.read_memory(address, length))',
        '    if hashlib.sha256(actual).hexdigest() != expected:',
        '        raise gdb.GdbError(',
        '            "Original firmware mismatch at %#x" % address)',
        'end',
        'set $ipod_jpeg_buffer = '
        '((unsigned int (*)(unsigned int, unsigned int))'
        '0x080e07a4)(0x100000, 0)',
        'python',
        'buffer = int(gdb.parse_and_eval("$ipod_jpeg_buffer"))',
        'if not 0x08000000 <= buffer <= 0x0bf00000:',
        '    raise gdb.GdbError("Unexpected allocator result: %#x" % buffer)',
        'results = []',
        f'cases = {cases!r}',
        f'planes = {PLANES!r}',
        f'output = Path({str(output)!r})',
        'end',
    ]
    # Keep inferior calls as ordinary GDB commands. Embedded Python only
    # prepares and inspects buffers while the target is stopped.
    for index, case in enumerate(cases):
        width, height = case['width'], case['height']
        signature = ','.join(['unsigned int'] * 9)
        script.extend([
            'python',
            f'case = cases[{index}]',
            'inferior.write_memory(buffer, Path(case["file"]).read_bytes())',
            'for name, offset, divisor in planes:',
            '    size = case["width"] * case["height"] // divisor',
            '    inferior.write_memory(buffer + offset - 32, b"\\xcc" * 32)',
            '    inferior.write_memory(buffer + offset, b"\\xa5" * size)',
            '    inferior.write_memory(buffer + offset + size, b"\\xcc" * 32)',
            'end',
            f'set $ipod_jpeg_result = ((unsigned int (*)({signature}))'
            f'0x0807bff8)(1,0,$ipod_jpeg_buffer,{case["size"]},'
            '$ipod_jpeg_buffer+0x20000,$ipod_jpeg_buffer+0x80000,'
            f'$ipod_jpeg_buffer+0xa0000,{width},{height})',
            'python',
            'result = int(gdb.parse_and_eval("$ipod_jpeg_result"))',
            'if result != 0:',
            '    raise gdb.GdbError("%s: decoder returned %#x" %',
            '                       (case["name"], result))',
            'record = dict(case, result=result, planes={})',
            'for name, offset, divisor in planes:',
            '    size = case["width"] * case["height"] // divisor',
            '    data = bytes(inferior.read_memory(buffer + offset, size))',
            '    for guard in (offset - 32, offset + size):',
            '        actual = bytes(inferior.read_memory(buffer + guard, 32))',
            '        if actual != b"\\xcc" * 32:',
            '            raise gdb.GdbError(',
            '                case["name"] + ": output guard changed")',
            '    path = output / (case["name"] + "-" + name + ".bin")',
            '    path.write_bytes(data)',
            '    record["planes"][name] = {"size": size,',
            '        "sha256": hashlib.sha256(data).hexdigest()}',
            'results.append(record)',
            '(output / "results.json").write_text(',
            '    json.dumps(results, indent=2) + "\\n")',
            'print(case["name"] + ',
            '      ": decoder returned zero; output guards intact")',
            'end',
        ])
    script.extend([
        'call ((void (*)(unsigned int, unsigned int))0x080dca98)'
        '($ipod_jpeg_buffer,0)',
        'echo JPEG probe finished; the VM remains stopped.\\n',
    ])
    (output / 'probe.gdb').write_text('\n'.join(script) + '\n')
    (output / 'inputs.json').write_text(json.dumps(
        {'firmware_sha256': OSOS_SHA256, 'cases': cases}, indent=2) + '\n')
    print(f'Prepared {output / "probe.gdb"}')
    print('In GDB attached to a disposable VM at the restored UI:')
    print('hbreak *0x080e07a4')
    print('continue')
    print('After a normal task hits allocator entry, disable that breakpoint.')
    print('source ' + json.dumps(str(output / 'probe.gdb')))
    print('This invokes original firmware functions and changes guest RAM.')


if __name__ == '__main__':
    main()
