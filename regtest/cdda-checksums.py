#!/usr/bin/env python3
"""Independent reference for the CD-DA checksums dvdisaster prints after
reading an audio CD (AccurateRip v1/v2, CRC32, AccurateRip/FreeDB disc id,
CTDB TOC id and CRC). Used to cross-check the values in the regtest database.

  cdda-checksums.py IMAGE.bin a0-1279,a1280-2559[,d3000-3999] [offset]
      tracks as in --sim-toc (sector ranges, a = audio, d = data)
  cdda-checksums.py --selftest [sample.wav]
      disc ids against real AccurateRip ids, and with sample.wav from
      https://github.com/arcctgx/ARver (tests/data/samples) the AccurateRip
      CRCs against ARver's published values
"""
import base64, hashlib, struct, sys, wave, zlib


def ar_crcs(data, first, last):
    """AccurateRip v1/v2 of a track; skips 5 sectors at the disc edges."""
    n = len(data) // 4
    lo = 2939 if first else 0
    hi = n - 2940 if last else n
    v1 = v2 = 0
    for i, s in enumerate(struct.unpack('<%dI' % n, data)):
        if lo <= i < hi:
            p = s * (i + 1)
            v1 += p & 0xffffffff
            v2 += (p & 0xffffffff) + (p >> 32)
    return v1 & 0xffffffff, v2 & 0xffffffff


def disc_ids(tracks):
    """tracks: list of (start, end, audio) with end = first sector behind."""
    id1 = id2 = num = cddb = 0
    for s, e, a in tracks:
        if a:
            num += 1
            id1 += s
            id2 += max(s, 1) * num
        cddb += sum(int(c) for c in str(s // 75 + 2))
    leadout = tracks[-1][1]
    id1 += leadout
    id2 += leadout * (num + 1)
    cddb = (cddb % 255) << 24 | (leadout // 75 - tracks[0][0] // 75) << 8 | len(tracks)
    return '%03d-%08x-%08x-%08x' % (num, id1 & 0xffffffff, id2 & 0xffffffff, cddb)


def ctdb_tocid(audio):
    first = audio[0][0]
    s = ''.join('%08X' % (t[0] - first) for t in audio[1:])
    s += '%08X' % (audio[-1][1] - first) + '0' * 8 * (100 - len(audio))
    b = base64.b64encode(hashlib.sha1(s.encode()).digest()).decode()
    return b.replace('+', '.').replace('/', '_').replace('=', '-')


def main(image, toc, offset=0):
    tracks = [(int(t[1:].split('-')[0]), int(t[1:].split('-')[1]) + 1, t[0] == 'a')
              for t in toc.split(',')]
    audio = [t for t in tracks if t[2]]
    pad = 4 * abs(offset)    # offset corrected, silence outside of the image
    raw = bytes(pad) + open(image, 'rb').read() + bytes(pad)

    def sectors(s, e):
        return raw[pad + s * 2352 + 4 * offset:pad + e * 2352 + 4 * offset]

    print('AccurateRip disc id: %s' % disc_ids(tracks))
    for i, (s, e, a) in enumerate(tracks):
        if a:
            d = sectors(s, e)
            v1, v2 = ar_crcs(d, (s, e, a) == audio[0], (s, e, a) == audio[-1])
            print('Track %2d: AR v1 %08x, AR v2 %08x, CRC32 %08x' % (i + 1, v1, v2, zlib.crc32(d)))
    d = sectors(audio[0][0], audio[-1][1])
    n = len(d) // 4
    print('CTDB TOC id: %s, CRC32 %08x' % (ctdb_tocid(audio),
          zlib.crc32(d[5880 * 4:(n - 5880 - n % 5880) * 4])))


def selftest(wav=None):
    # (track lengths, pregap, data track length, id) from ARver's discinfo_test.py;
    # these are ids of real discs in the AccurateRip database.
    for lengths, pregap, data, ref in [
            ([75258, 54815, 205880], 0, 0, '003-00084264-001cc184-19117f03'),
            ([107450, 71470, 105737, 71600], 33, 0, '004-000e26d9-00380804-3e128e04'),
            ([12617, 27720, 22738, 30185, 24705, 33750, 32475, 30920, 32195, 22880], 0, 52066,
             '010-00164419-00b9f6e2-9e11600b'),
            ([90778], 0, 164721, '001-00041293-00082527-100de602')]:
        tracks, s = [], pregap
        for n in lengths:
            tracks.append((s, s + n, True))
            s += n
        if data:   # CD Extra: data session 11400 sectors behind the audio session
            tracks.append((s + 11400, s + 11400 + data, False))
        assert disc_ids(tracks) == ref, (disc_ids(tracks), ref)
    print('disc ids: ok')
    if wav:
        w = wave.open(wav)
        d = w.readframes(w.getnframes())
        assert ar_crcs(d, True, True) == (0xf43f9174, 0x0ae7c6f9)
        assert ar_crcs(d, True, False) == (0x9d6c90ec, 0xb775893e)
        assert ar_crcs(d, False, False) == (0x3c8dd1d2, 0x56bba272)
        assert ar_crcs(d, False, True) == (0x9360d25a, 0xaa2de02d)
        assert zlib.crc32(d) == 0x8ce80129
        print('AccurateRip CRCs: ok')


if __name__ == '__main__':
    if sys.argv[1] == '--selftest':
        selftest(*sys.argv[2:])
    else:
        main(sys.argv[1], sys.argv[2], *map(int, sys.argv[3:]))
