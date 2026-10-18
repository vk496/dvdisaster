"""Offline GF(2^16) reference and optional native FFT engine for RS04 v2.

SPDX-License-Identifier: GPL-3.0-or-later
No third party Python packages are required. Native and reference engines
use the field, Cantor basis and shard packing of the RS04 specification.
"""
try:
    import ctypes
except ImportError:
    ctypes = None
import os
from functools import lru_cache
from pathlib import Path

POLY = 0x1002D
BASIS = (1, 0xACCA, 0x3C0E, 0x163E, 0xC582, 0xED2E, 0x914C, 0x4012,
         0x6C98, 0x10D8, 0x6A72, 0xB900, 0xFDB8, 0xFB34, 0xFF38, 0x991E)
EXP = []
LOG = [0] * 65536
x = 1
for i in range(65535):
    EXP.append(x)
    LOG[x] = i
    x <<= 1
    if x & 65536:
        x ^= POLY
TO = [0] * 65536
for i in range(1, 65536):
    low = i & -i
    TO[i] = TO[i ^ low] ^ BASIS[low.bit_length() - 1]
FROM = [0] * 65536
for i, x in enumerate(TO):
    FROM[x] = i


def mul(a, b):
    return EXP[(LOG[a] + LOG[b]) % 65535] if a and b else 0


def inv(a):
    if not a:
        raise ValueError('singular equation')
    return EXP[-LOG[a] % 65535]


def pow2(n):
    return 1 << (max(1, n) - 1).bit_length()


def geometry(k, reserve):
    if not 1 <= k <= 65535 or reserve != pow2(reserve) or not 1 <= reserve <= 32768 or k + reserve > 65536:
        raise ValueError('invalid immutable coding geometry')


@lru_cache(maxsize=65536)
def numerator(reserve, source):
    value = 1
    for j in range(reserve):
        value = mul(value, TO[reserve + source] ^ TO[j])
    derivative = 1
    for j in range(1, reserve):
        derivative = mul(derivative, TO[j])
    return mul(value, inv(derivative))


def coefficient(reserve, row, source):
    """Cauchy expression for the draft's parity-check-defined code.

    P(x)=product(x+omega_j,j<M); P' is constant on the binary subspace.
    G[row,source]=P(omega_(M+source))/(P'* (omega_row+omega_(M+source))).
    Coefficients use the polynomial representation, payload uses Cantor.
    """
    return mul(numerator(reserve, source), inv(TO[row] ^ TO[reserve + source]))


def pack(sector):
    full, tail = divmod(len(sector), 64)
    if not tail:
        return sector
    offset = full * 64
    return (sector[:offset] + sector[offset:offset + tail // 2]
            + bytes(32 - tail // 2) + sector[offset + tail // 2:] + bytes(32 - tail // 2))


def unpack(shard, length):
    offset, tail = length // 64 * 64, length % 64
    if not tail:
        return shard
    if any(shard[offset + tail // 2:offset + 32]) or any(shard[offset + 32 + tail // 2:]):
        raise ValueError('nonzero shard padding')
    return shard[:offset + tail // 2] + shard[offset + 32:offset + 32 + tail // 2]


@lru_cache(maxsize=16)
def multiplication_table(factor):
    return [FROM[mul(TO[i], factor)] for i in range(65536)]


def add_scaled(target, source, factor):
    if not factor:
        return
    if factor == 1:
        for i, value in enumerate(source):
            target[i] ^= value
        return
    table = multiplication_table(factor)
    for base in range(0, len(source), 64):
        for i in range(32):
            value = table[source[base + i] | source[base + 32 + i] << 8]
            target[base + i] ^= value & 255
            target[base + 32 + i] ^= value >> 8


def eliminate(equations, variables, add=add_scaled):
    """Solve vector-valued equations. None means insufficient rank."""
    pivots = {}
    for coefficients, payload in equations:
        row, value = list(coefficients), bytearray(payload)
        for col, (previous, rhs) in sorted(pivots.items()):
            if row[col]:
                scale = row[col]
                row = [a ^ mul(scale, b) for a, b in zip(row, previous)]
                add(value, rhs, scale)
        col = next((i for i, v in enumerate(row) if v), None)
        if col is None:
            if any(value):
                raise ValueError('inconsistent recovery equations')
            continue
        scale = inv(row[col])
        row = [mul(v, scale) for v in row]
        normalized = bytearray(len(value))
        add(normalized, value, scale)
        pivots[col] = row, normalized
    if len(pivots) != variables:
        return None
    result = [None] * variables
    for col in sorted(pivots, reverse=True):
        row, value = pivots[col]
        for i in range(col + 1, variables):
            if row[i]:
                add(value, result[i], row[i])
        result[col] = bytes(value)
    return result


class Codec:
    def __init__(self, library=None, scalar=False):
        self.native = None
        self.region = None
        if library and ctypes is None:
            raise ValueError('this Python interpreter has no ctypes module for native acceleration')
        if not scalar and ctypes is not None:
            candidates = [library] if library else [os.environ.get('RS04_CODEC_LIBRARY'),
                str(Path(__file__).resolve().parent.parent / 'build' / 'librs04-codec.so'),
                str(Path(__file__).resolve().parent / 'librs04-codec.so')]
            for name in filter(None, candidates):
                try:
                    self.native = ctypes.CDLL(name)
                    break
                except OSError:
                    if library:
                        raise
            if self.native:
                for name in ('RS04EncodeFamily', 'RS04DecodeFamily'):
                    try:
                        fn = getattr(self.native, name)
                    except AttributeError as error:
                        raise ValueError('codec library lacks the immutable-family API') from error
                    fn.restype = ctypes.c_int
                    fn.argtypes = [ctypes.c_uint64, ctypes.c_uint, ctypes.c_uint,
                                   ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
                    if name == 'RS04DecodeFamily':
                        fn.argtypes += [ctypes.POINTER(ctypes.c_void_p)]
                    fn.argtypes += [ctypes.POINTER(ctypes.c_void_p)]
                self.region = getattr(self.native, 'RS04MulAddFamily', None)
                if self.region:
                    self.region.restype = ctypes.c_int
                    self.region.argtypes = [ctypes.c_uint64, ctypes.c_uint16, ctypes.c_void_p, ctypes.c_void_p]

    def add_scaled(self, target, source, factor):
        if not self.region:
            return add_scaled(target, source, factor)
        if not factor:
            return
        source_buffer = ctypes.create_string_buffer(bytes(source), len(source))
        target_buffer = (ctypes.c_ubyte * len(target)).from_buffer(target)
        result = self.region(len(source), FROM[factor], source_buffer, target_buffer)
        if result:
            raise ValueError(f'native field operation failed ({result})')

    def encode(self, originals, reserve, rows=None):
        k = len(originals)
        geometry(k, reserve)
        rows = list(range(reserve)) if rows is None else list(rows)
        if len(set(rows)) != len(rows) or any(not 0 <= r < reserve for r in rows):
            raise ValueError('invalid or duplicate parity row ID')
        length = len(originals[0])
        if any(len(s) != length for s in originals) or length % 2:
            raise ValueError('unequal or odd-sized shards')
        source = [pack(s) for s in originals]
        if self.native:
            buffers = [ctypes.create_string_buffer(s, len(s)) for s in source]
            pointers = (ctypes.c_void_p * k)(*[ctypes.addressof(s) for s in buffers])
            workspace = ctypes.create_string_buffer(2 * reserve * len(source[0]))
            work = (ctypes.c_void_p * (2 * reserve))(*[
                ctypes.addressof(workspace) + i * len(source[0]) for i in range(2 * reserve)])
            code = self.native.RS04EncodeFamily(len(source[0]), k, reserve, 2 * reserve, pointers, work)
            if code:
                raise ValueError(f'native encode failed ({code})')
            return {r: unpack(ctypes.string_at(work[r], len(source[0])), length) for r in rows}
        active = [i for i, shard in enumerate(source) if any(shard)]
        result = {}
        for r in rows:
            value = bytearray(len(source[0]))
            for i in active:
                add_scaled(value, source[i], coefficient(reserve, r, i))
            result[r] = unpack(bytes(value), length)
        return result

    def decode(self, originals, reserve, recovery):
        k = len(originals)
        geometry(k, reserve)
        missing = [i for i, s in enumerate(originals) if s is None]
        if not missing:
            return {}
        if len(recovery) < len(missing):
            return None
        if any(not 0 <= r < reserve for r in recovery):
            raise ValueError('invalid parity row ID')
        length = len(next(iter(recovery.values())))
        if any(len(s) != length for s in list(recovery.values()) + [s for s in originals if s is not None]):
            raise ValueError('unequal shard sizes')
        source = [pack(s) if s is not None else None for s in originals]
        parity = {r: pack(s) for r, s in recovery.items()}
        if self.native:
            sb = len(next(iter(parity.values())))
            buffers = [ctypes.create_string_buffer(s, sb) if s is not None else None for s in source]
            rbuffers = {r: ctypes.create_string_buffer(s, sb) for r, s in parity.items()}
            op = (ctypes.c_void_p * k)(*[ctypes.addressof(s) if s is not None else 0 for s in buffers])
            rp = (ctypes.c_void_p * reserve)(*[ctypes.addressof(rbuffers[r]) if r in rbuffers else 0
                                                         for r in range(reserve)])
            count = pow2(k + reserve)
            workspace = ctypes.create_string_buffer(count * sb)
            work = (ctypes.c_void_p * count)(*[ctypes.addressof(workspace) + i * sb for i in range(count)])
            code = self.native.RS04DecodeFamily(sb, k, reserve, count, op, rp, work)
            if code:
                raise ValueError(f'native decode failed ({code})')
            return {i: unpack(ctypes.string_at(work[i], sb), length) for i in missing}
        active = [i for i, shard in enumerate(source) if shard is not None and any(shard)]
        equations = []
        for r, payload in list(parity.items())[:len(missing)]:
            value = bytearray(payload)
            for i in active:
                add_scaled(value, source[i], coefficient(reserve, r, i))
            equations.append(([coefficient(reserve, r, i) for i in missing], value))
        result = eliminate(equations, len(missing))
        return None if result is None else {i: unpack(s, length) for i, s in zip(missing, result)}
