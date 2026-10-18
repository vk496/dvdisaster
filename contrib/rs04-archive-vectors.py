#!/usr/bin/env python3
"""Reproduce deterministic RS04 v2 complete-container vectors.

SPDX-License-Identifier: GPL-3.0-or-later
Run with --scalar to reproduce the bytes without the native FFT codec.
"""
import argparse
import json
import struct
import tempfile
from pathlib import Path

import rs04_archive as archive
from rs04_archive_codec import Codec


def stream(seed, size):
    output = bytearray()
    mask = (1 << 64)-1
    while len(output) < size:
        seed = (seed+0x9E3779B97F4A7C15) & mask
        z = seed
        z = ((z ^ (z >> 30))*0xBF58476D1CE4E5B9) & mask
        z = ((z ^ (z >> 27))*0x94D049BB133111EB) & mask
        output.extend(struct.pack('<Q',z ^ (z >> 31)))
    return bytes(output[:size])


def vectors(scalar=False):
    result = {'version': 2, 'source_generator': 'splitmix64, little-endian u64 output', 'cases': []}
    with tempfile.TemporaryDirectory(prefix='rs04-vectors-') as temporary:
        root=Path(temporary)
        for seed,B,count,tail,k,M,rows in ((7,2048,40,-100,6,4,[0,2]),
                                          (8,2352,10,0,3,4,[1,3]),
                                          (9,2324,5,0,1,2,[0])):
            data=stream(seed,count*B+tail)
            source=root/'source'
            source.write_bytes(data)
            output=root/f'{B}.ecc'
            family=archive.make_family((len(data)+B-1)//B,k,M,rows)
            created=archive.create_archive([source],output,Codec(scalar=scalar),[family],B)
            result['cases'].append({'seed':seed,'sector':B,'data_bytes':len(data),
                'k':family['k'],'reserve':M,'layer':family['layer'],'rows':rows,
                'source_sha256':archive.digest(data).hex(),
                'source_identity':created['source_hash'],
                'catalogue_sha256':created['catalogue_hash'],
                'container_sha256':archive.digest(output.read_bytes()).hex()})
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scalar',action='store_true')
    args=parser.parse_args()
    print(json.dumps(vectors(args.scalar),indent=2,sort_keys=True))
