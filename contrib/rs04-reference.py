#!/usr/bin/env python3
"""
rs04-reference.py - slow, direct reference encoder for the RS04 format.

Implements the RS04 specification (documentation/codec-specs/rs04.tex)
literally: the GF(2^16) code is computed from its parity-check definition
by matrix algebra instead of the additive FFT, so it shares no code with
src/rs04-ff16.c. Running it

  1. checks the small codec test vectors (seeds 1-3 of the spec table),
  2. prints the RS04 format test vectors (md5 of the added zone and a
     hex dump of the first tag) quoted in the specification.

Usage: python3 rs04-reference.py      (Python 3 only, runs in a few seconds)
"""
import hashlib
import struct
import zlib

POLY = 0x1002D
CANTOR = [0x0001, 0xACCA, 0x3C0E, 0x163E, 0xC582, 0xED2E, 0x914C, 0x4012,
          0x6C98, 0x10D8, 0x6A72, 0xB900, 0xFDB8, 0xFB34, 0xFF38, 0x991E]

# Field elements are handled in the polynomial basis modulo POLY;
# an RS04 symbol value x denotes to_std(x) (sum of the Cantor basis
# elements selected by the bits of x).
EXP = [0] * 65535
LOG = [0] * 65536
_s = 1
for _i in range(65535):
    EXP[_i] = _s
    LOG[_s] = _i
    _s <<= 1
    if _s & 0x10000:
        _s ^= POLY


def mul(a, b):
    return 0 if a == 0 or b == 0 else EXP[(LOG[a] + LOG[b]) % 65535]


def inv(a):
    return EXP[(65535 - LOG[a]) % 65535]


def pw(a, e):
    return 1 if e == 0 else (0 if a == 0 else EXP[LOG[a] * e % 65535])


TO_STD = [0] * 65536
for _x in range(1, 65536):
    _low = _x & -_x
    TO_STD[_x] = TO_STD[_x ^ _low] ^ CANTOR[_low.bit_length() - 1]
FROM_STD = [0] * 65536
for _x in range(65536):
    FROM_STD[TO_STD[_x]] = _x


def next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def generator(k, m):
    """m x k matrix G (std basis): parity_j = sum_i G[j][i] * d_i."""
    M = next_pow2(m)
    w = [TO_STD[j] for j in range(M + k)]
    # sum_{j<M} c_j w_j^t = sum_{i<k} d_i w_{M+i}^t  (t < M); solve for c
    A = [[pw(w[j], t) for j in range(M)] + [pw(w[M + i], t) for i in range(k)]
         for t in range(M)]
    for col in range(M):
        piv = next(r for r in range(col, M) if A[r][col])
        A[col], A[piv] = A[piv], A[col]
        f = inv(A[col][col])
        A[col] = [mul(v, f) for v in A[col]]
        for r in range(M):
            if r != col and A[r][col]:
                f = A[r][col]
                A[r] = [v ^ mul(f, u) for v, u in zip(A[r], A[col])]
    return [row[M:] for row in A[:m]]


def symbols(shard):
    """Leopard buffer layout: per 64-byte block, 32 low bytes then 32 high bytes."""
    out = []
    for b in range(0, len(shard), 64):
        out += [shard[b + i] | shard[b + 32 + i] << 8 for i in range(32)]
    return out


def unsymbols(syms):
    out = bytearray(len(syms) * 2)
    for s, v in enumerate(syms):
        b, i = divmod(s, 32)
        out[64 * b + i] = v & 255
        out[64 * b + 32 + i] = v >> 8
    return bytes(out)


def encode_shards(originals, m):
    """originals: k equally sized shards (multiple of 64 bytes) -> m parity shards."""
    k = len(originals)
    G = generator(k, m)
    cols = [[TO_STD[v] for v in symbols(o)] for o in originals]
    par = []
    for j in range(m):
        acc = [0] * len(cols[0])
        for i in range(k):
            g = G[j][i]
            if g:
                lg = LOG[g]
                acc = [a ^ (EXP[(lg + LOG[v]) % 65535] if v else 0) for a, v in zip(acc, cols[i])]
        par.append(unsymbols([FROM_STD[v] for v in acc]))
    return par


# --- sector <-> shard mapping (spec: "Sector to shard mapping") ---

def shard_bytes(B):
    return (B + 63) // 64 * 64


def to_shard(sector):
    B = len(sector)
    F, r = B // 64 * 64, B % 64
    sh = bytearray(shard_bytes(B))
    sh[:F] = sector[:F]
    sh[F:F + r // 2] = sector[F:F + r // 2]
    sh[F + 32:F + 32 + r // 2] = sector[F + r // 2:]
    return bytes(sh)


def from_shard(sh, B):
    F, r = B // 64 * 64, B % 64
    used = bytearray(len(sh))
    used[:F] = sh[:F]
    used[F:F + r // 2] = sh[F:F + r // 2]
    used[F + 32:F + 32 + r // 2] = sh[F + 32:F + 32 + r // 2]
    assert used == sh, "non-zero shard padding"
    return sh[:F] + sh[F:F + r // 2] + sh[F + 32:F + 32 + r // 2]


# --- checksums ---

def crc32(b):
    """dvdisaster Crc32(): CRC-32/zlib without the final inversion."""
    return zlib.crc32(b) ^ 0xFFFFFFFF


def splitmix(seed):
    while True:
        seed = (seed + 0x9E3779B97F4A7C15) & (2**64 - 1)
        z = seed
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & (2**64 - 1)
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & (2**64 - 1)
        yield z ^ (z >> 31)


def stream(seed, nbytes):
    g = splitmix(seed)
    return b''.join(struct.pack('<Q', next(g)) for _ in range((nbytes + 7) // 8))[:nbytes]


# --- RS04 set ---

def make_tag(B, T, set_id, data_bytes, L, k, m, own_pos, md5, crcs, creator=0, flags=0):
    head = struct.pack('<12s4sHBBHHII8sQQIIQ16s', b'*dvdisaster*', b'RS04', 1, 0, flags,
                       B, T, creator, 0, set_id, data_bytes, L, k, m, own_pos, md5)
    assert len(head) == 88
    tag = bytearray(head + struct.pack('<255I', *(crcs + [0] * (255 - len(crcs)))) + bytes(940))
    struct.pack_into('<I', tag, 28, crc32(bytes(tag)))
    return bytes(tag) + bytes(B - 2048)


def added_zone(data, B, L, k, m, T, set_id):
    """Returns the added zone (= .ecc file) of an RS04 set as a list of B-byte sectors."""
    S = (len(data) + B - 1) // B
    c = (S + 511) // 512
    assert 1 <= m <= k and next_pow2(m) + k <= 65536 and k * L >= S + c
    assert T in (2, 4, 8, 16, 32, 64, 128, 256)
    sectors = [data[i * B:(i + 1) * B].ljust(B, b'\0') for i in range(S)]
    dcrc = [crc32(s) for s in sectors]
    crc_sectors = [struct.pack('<512I', *(dcrc[512 * j:512 * j + 512] + [0] * max(0, 512 * j + 512 - S)))
                   .ljust(B, b'\0') for j in range(c)]
    X = sectors + crc_sectors + [bytes(B)] * (k * L - S - c)        # originals, layer-major
    parity = [None] * (m * L)
    for i in range(L):                                              # codeword i
        par = encode_shards([to_shard(X[j * L + i]) for j in range(k)], m)
        for j in range(m):
            parity[j * L + i] = from_shard(par[j], B)
    payload = crc_sectors + parity
    Q = len(payload)
    A = Q + (Q + T - 2) // (T - 1)
    md5 = hashlib.md5(data).digest()
    zone = []
    for a in range(A):
        if a % T == 0:
            g = a // T
            crcs = [crc32(p) for p in payload[g * (T - 1):(g + 1) * (T - 1)]]
            zone.append(make_tag(B, T, set_id, len(data), L, k, m, S + a, md5, crcs))
        else:
            zone.append(payload[a - a // T - 1])
    return zone


# --- recommended parameter choice (spec: "Choosing the encoding parameters") ---

def pow2floor(x):
    return 1 << (max(1, x).bit_length() - 1)


def tag_interval(Q):
    return 256 if Q >= 4080 else max(2, pow2floor(Q // 16))


def choose(S, B=2048, ratio=None, capacity=None, ram=4 << 30):
    """ratio: parity/protected for .ecc files; capacity: sectors available
    for the whole set (fill the medium). Returns (L, k, m, T, total sectors)."""
    c = (S + 511) // 512
    D = S + c
    T0 = 256
    if capacity is not None:
        Z = capacity - S
        T0 = tag_interval(Z)
        P = min(Z - -(-Z // T0) - c, 2 * D)
        C = capacity
    else:
        P = min(round(ratio * D), 2 * D)
        C = D + P
    if P < 1 or P * 247 < 8 * D:
        return None

    def ok(L, N):
        m = P // L
        k = max(-(-D // L), m)
        return m >= 1 and next_pow2(m) + k <= N

    N = min(65536, pow2floor(C // 32), pow2floor(ram // (shard_bytes(B) * 128)))
    if not ok(P, N):
        N = 65536                        # tiny sets: drop the size/RAM cap
        if not ok(P, N):
            return None
    lo, hi = min(32, D, P), P
    while lo < hi:                       # smallest L with ok(L); ok() is monotone
        mid = (lo + hi) // 2
        lo, hi = (lo, mid) if ok(mid, N) else (mid + 1, hi)
    L = lo
    m = P // L
    k = max(-(-D // L), m)
    Q = c + m * L
    T = tag_interval(Q)
    size = S + Q + -(-Q // (T - 1))
    if capacity is not None and size > capacity:
        T = T0
        size = S + Q + -(-Q // (T - 1))
    return L, k, m, T, size


def main():
    # 1. codec test vectors (spec table), small cases only
    for seed, k, m, sb, want in [(1, 2, 1, 64, '9b1fe76ef28bf3823a5e3cf97adf0901'),
                                 (2, 5, 3, 64, '74130cbd984417137639d2a582b9b4a1'),
                                 (3, 100, 31, 128, '8c5370f6adebf2d28b46125fdd201dc8')]:
        raw = stream(seed, k * sb)
        par = encode_shards([raw[i * sb:(i + 1) * sb] for i in range(k)], m)
        assert hashlib.md5(b''.join(par)).hexdigest() == want, seed
    print('codec test vectors 1-3: ok')
    assert crc32(b'123456789') == 0x340BC6D9

    # 2. format test vectors
    for name, seed, nbytes, B, L, k, m, T in [('F1', 7, 40 * 2048 - 100, 2048, 7, 6, 3, 8),
                                             ('F2', 8, 10 * 2352, 2352, 4, 3, 2, 4),
                                             ('F3', 9, 5 * 2324, 2324, 6, 1, 1, 2)]:
        data = stream(seed, nbytes)
        zone = added_zone(data, B, L, k, m, T, bytes(range(1, 9)))
        blob = b''.join(zone)
        print(f'{name}: B={B} dataBytes={nbytes} L={L} k={k} m={m} T={T} '
              f'added zone {len(zone)} sectors, md5(data)={hashlib.md5(data).hexdigest()} '
              f'md5(added zone)={hashlib.md5(blob).hexdigest()}')
        if name == 'F1':
            for o in range(0, 96, 16):
                print('  %04x: %s' % (o, zone[0][o:o + 16].hex(' ')))

    # 3. parameter choice examples
    for what, S, kw in [('CD fill', 300000, dict(capacity=359424)),
                        ('DVD fill', 2000000, dict(capacity=2295104)),
                        ('DVD-9 fill', 3500000, dict(capacity=4171712)),
                        ('BD-25 fill', 10000000, dict(capacity=11826176)),
                        ('CD fill, tiny', 100, dict(capacity=359424)),
                        ('ecc 32/223', 2000000, dict(ratio=32 / 223)),
                        ('CD-DA ecc 20%', 330000, dict(B=2352, ratio=0.2)),
                        ('ecc 10 sectors 50%', 10, dict(ratio=0.5))]:
        r = choose(S, **kw)
        L, k, m, T, total = r
        print(f'{what}: S={S} -> L={L} k={k} m={m} n={k + m} T={T} '
              f'FFT={next_pow2(next_pow2(m) + k)} set size={total} parity/protected={m * L / (S + (S + 511) // 512):.3f}')


if __name__ == '__main__':
    main()
