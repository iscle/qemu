#!/usr/bin/env python3
"""Reproduce wInd3x's short-ciphertext failure with synthetic AES keys.

Reads the local originals, without modifying them or claiming to recover their
plaintext. The synthetic device oracle uses standard AES-CBC with a zero IV,
matching the decryptor's previous-ciphertext-block request construction.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def aes_cbc(key, data, encrypt=False):
    cipher = Cipher(algorithms.AES(key), modes.CBC(bytes(16)))
    context = cipher.encryptor() if encrypt else cipher.decryptor()
    return context.update(data) + context.finalize()


def wind3x_decrypt(key, ciphertext):
    """Transcribe request formation/output slicing in decrypt.Decrypt()."""
    output = bytearray()
    for offset in range(0, len(ciphertext), 0x30):
        chunk = ciphertext[offset:offset + 0x30].ljust(0x30, b'\0')
        if offset == 0:
            request = chunk + bytes(16)
            output.extend(aes_cbc(key, request)[:0x30])
        else:
            request = ciphertext[offset - 16:offset] + chunk
            output.extend(aes_cbc(key, request)[16:0x40])
    return bytes(output)


def synthetic_checks():
    checked = 0
    for key_bytes in (16, 24, 32):
        key = bytes(range(key_bytes))
        for declared in range(1, 193):
            padded = (declared + 15) & ~15
            plaintext = bytes((index * 37 + 11) & 255
                              for index in range(padded))
            ciphertext = aes_cbc(key, plaintext, encrypt=True)
            legacy = wind3x_decrypt(key, ciphertext[:declared])
            rounded = wind3x_decrypt(key, ciphertext)
            assert rounded[:padded] == plaintext
            full_blocks = declared & ~15
            assert legacy[:full_blocks] == plaintext[:full_blocks]
            assert len(legacy) == (declared + 47) // 48 * 48
            if declared % 16:
                # Padding is harmless only if every omitted ciphertext byte
                # already happens to be zero (possible for a short suffix).
                damaged = ciphertext[declared:] != bytes(padded - declared)
                assert (legacy[full_blocks:padded] !=
                        plaintext[full_blocks:]) == damaged
            checked += 1
    return checked


def read_original(path, expected_sha256):
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == expected_sha256, path
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('encrypted', type=Path,
                        help='original wrapped encrypted 2.0.4 osos image')
    parser.add_argument('decrypted', type=Path,
                        help='supplied wrapped decrypted 2.0.4 osos image')
    args = parser.parse_args()
    encrypted_hash = ('bd45026c0cd207ffdf8a78906e4cb2f24'
                      'a33659c85ca6a70de6483dbac631579')
    decrypted_hash = ('f4368251a58b2fdc7b46acf3178dae1d2'
                      '4bc1e029736240741015851256c65c4')
    encrypted = read_original(args.encrypted,
                              encrypted_hash)
    decrypted = read_original(
        args.decrypted,
        decrypted_hash)
    declared = struct.unpack_from('<I', encrypted, 12)[0]
    padded = (declared + 15) & ~15
    output_length = struct.unpack_from('<I', decrypted, 12)[0]
    assert declared == 0xa1b5e8 and padded == 0xa1b5f0
    assert output_length == (declared + 47) // 48 * 48 == padded
    assert len(decrypted) == 0x800 + output_length
    body = encrypted[0x800:0x800 + padded]
    legacy_final_chunk = body[(declared // 48) * 48:declared]
    legacy_final_chunk = legacy_final_chunk.ljust(48, b'\0')
    assert len(body) == padded
    marker_offset = 0x800 + padded - 16
    assert marker_offset == 0xa1bde0
    assert struct.unpack_from('<I', decrypted, marker_offset)[0] == 0x5a7c7c3c

    # An illustrative known-key sample with exactly the same length remainders.
    example_plaintext = bytearray((index * 37 + 11) & 255
                                 for index in range(144))
    struct.pack_into('<I', example_plaintext, 128, 0x13061973)
    example_ciphertext = aes_cbc(bytes(range(16)), example_plaintext, True)
    example_legacy = wind3x_decrypt(bytes(range(16)), example_ciphertext[:136])
    example_rounded = wind3x_decrypt(bytes(range(16)), example_ciphertext)
    assert example_legacy[:128] == example_plaintext[:128]
    assert example_rounded == example_plaintext

    print(json.dumps({
        'synthetic_cases_passed': synthetic_checks(),
        'synthetic_key_sizes_bits': [128, 192, 256],
        'encrypted_sha256': encrypted_hash,
        'decrypted_sha256': decrypted_hash,
        'declared_ciphertext_length': hex(declared),
        'rounded_ciphertext_length': hex(padded),
        'declared_mod_48': declared % 48,
        'declared_mod_16': declared % 16,
        'legacy_output_length': hex(output_length),
        'omitted_ciphertext': body[declared:].hex(),
        'previous_ciphertext_block': body[-32:-16].hex(),
        'original_final_ciphertext_block': body[-16:].hex(),
        'legacy_final_ciphertext_block': legacy_final_chunk[-16:].hex(),
        'affected_plaintext_file_offset': hex(marker_offset),
        'supplied_bad_plaintext_block': decrypted[marker_offset:].hex(),
        'example_expected_final_plaintext': example_plaintext[-16:].hex(),
        'example_legacy_final_plaintext': example_legacy[-16:].hex(),
        'example_rounded_final_plaintext': example_rounded[-16:].hex(),
        'conclusion': ('The source bug reproduces final-block-only corruption '
                       'at exactly the supplied image boundary. The real '
                       'hardware AES key and original final plaintext remain '
                       'unknown; these synthetic checks do not recover them.'),
    }, indent=2))


if __name__ == '__main__':
    main()
