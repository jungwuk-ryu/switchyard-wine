"""Deterministic sustained CPU workload for the pinned Windows x64 CPython.

This is a private Switchyard performance gate, not a timing-by-deadline loop.
It combines a pure-interpreter 64-bit integer kernel with CPython's standard
hashing and compression modules, then prints only deterministic result data.
"""

from __future__ import annotations

import bz2
import hashlib
import lzma
import struct
import sys
import zlib


MASK64 = (1 << 64) - 1
INTEGER_ITERATIONS = 1_000_000
COMPRESSION_ROUNDS = 4
BLOB_BLOCKS = 4_096


def integer_kernel(iterations: int) -> int:
    state = 0x243F6A8885A308D3
    accumulator = 0x13198A2E03707344
    for index in range(iterations):
        state = (state + 0x9E3779B97F4A7C15 + index) & MASK64
        mixed = state
        mixed = ((mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        mixed = ((mixed ^ (mixed >> 27)) * 0x94D049BB133111EB) & MASK64
        mixed ^= mixed >> 31
        rotation = index & 63
        rotated = ((mixed << rotation) | (mixed >> ((64 - rotation) & 63))) & MASK64
        accumulator = (accumulator + rotated) & MASK64
        accumulator ^= ((accumulator << 17) | (accumulator >> 47)) & MASK64
    return accumulator


def deterministic_blob(blocks: int) -> bytes:
    seed = b"Switchyard Windows x64 CPython sustained CPU gate v1"
    output = bytearray()
    for index in range(blocks):
        seed = hashlib.sha256(seed + struct.pack("<I", index)).digest()
        output.extend(seed)
    return bytes(output)


def compression_kernel(blob: bytes, rounds: int) -> tuple[int, int, int, str]:
    transcript = hashlib.sha256()
    zlib_total = 0
    bz2_total = 0
    lzma_total = 0

    for round_index in range(rounds):
        zlib_output = zlib.compress(blob, level=9)
        bz2_output = bz2.compress(blob, compresslevel=9)
        lzma_output = lzma.compress(blob, preset=6)
        zlib_total += len(zlib_output)
        bz2_total += len(bz2_output)
        lzma_total += len(lzma_output)
        transcript.update(struct.pack("<IQQQ", round_index, len(zlib_output),
                                      len(bz2_output), len(lzma_output)))
        transcript.update(hashlib.sha256(zlib_output).digest())
        transcript.update(hashlib.sha256(bz2_output).digest())
        transcript.update(hashlib.sha256(lzma_output).digest())
        replacement = hashlib.sha256(transcript.digest() + blob[:64]).digest()
        blob = replacement + blob[len(replacement):]

    return zlib_total, bz2_total, lzma_total, transcript.hexdigest()


def main() -> None:
    blob = deterministic_blob(BLOB_BLOCKS)
    integer_result = integer_kernel(INTEGER_ITERATIONS)
    zlib_total, bz2_total, lzma_total, compression_digest = compression_kernel(
        blob, COMPRESSION_ROUNDS
    )
    final = hashlib.sha256()
    final.update(struct.pack("<Q", integer_result))
    final.update(struct.pack("<QQQ", zlib_total, bz2_total, lzma_total))
    final.update(bytes.fromhex(compression_digest))

    print("SWITCHYARD_WINDOWS_PYTHON_CPU_V1")
    print(f"python={sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")
    print(f"integer_iterations={INTEGER_ITERATIONS}")
    print(f"compression_rounds={COMPRESSION_ROUNDS}")
    print(f"blob_bytes={len(blob)}")
    print(f"integer_result={integer_result:016x}")
    print(f"zlib_total={zlib_total}")
    print(f"bz2_total={bz2_total}")
    print(f"lzma_total={lzma_total}")
    print(f"compression_digest={compression_digest}")
    print(f"final_digest={final.hexdigest()}")


if __name__ == "__main__":
    main()
