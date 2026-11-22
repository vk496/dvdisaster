#!/usr/bin/env python3
"""RS04 archival development format and offline recovery CLI.

SPDX-License-Identifier: GPL-3.0-or-later
This module deliberately uses only the Python standard library. Optional
native FFT acceleration is provided by src/rs04-ff16.c, not a Python package.
"""
import argparse
import bisect
import hashlib
import json
import math
import os
import shutil
import struct
import sys
import tempfile
import zlib
from collections import defaultdict
from contextlib import ExitStack, contextmanager, nullcontext
from pathlib import Path, PurePosixPath

from rs04_archive_codec import Codec, add_scaled, coefficient, eliminate, geometry, pack, pow2, unpack

MAGIC = b'*dvdisaster*RS04'
VERSION = 2
META, PARITY = 1, 2
HEADER = struct.Struct('<16sHHHHII32sQIIQ32sII')
SHARE = 65536
META_K, META_M = 64, 64
BOOTSTRAP_BYTES = HEADER.size + 8
MAX_RECORD = 1024 * 1024
MAX_JSON = 4 * 1024 * 1024
MAX_RECORDS = 4 * 1024 * 1024
SECTORS = (2048, 2324, 2352)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True,
                      allow_nan=False).encode('ascii')


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('duplicate JSON key')
        value[key] = item
    return value


def parse_json(blob):
    try:
        result = json.loads(blob, object_pairs_hook=unique_object,
                            parse_constant=lambda v: (_ for _ in ()).throw(ValueError('nonfinite JSON')))
    except (RecursionError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError('invalid metadata JSON') from error
    return result


def load_json_file(path):
    with open(path,'rb') as source:
        value = source.read(MAX_JSON+1)
    if len(value)>MAX_JSON:
        raise ValueError('JSON input exceeds resource bound')
    return parse_json(value)


def digest(data):
    return hashlib.sha256(data).digest()


def fingerprint(data, valid, sector):
    return digest(b'RS04v2 content block\0' + struct.pack('<II', sector, valid) + data[:valid])


def file_digest(stream, length=None):
    stream.seek(0)
    h = hashlib.sha256()
    while length is None or length:
        block = stream.read(1024 * 1024 if length is None else min(length, 1024 * 1024))
        if not block:
            if length:
                raise ValueError('truncated file')
            break
        h.update(block)
        if length is not None:
            length -= len(block)
    return h.digest()


def integer(value, lo, hi):
    if type(value) is not int or not lo <= value <= hi:
        raise ValueError('integer outside format bounds')
    return value


def hex_digest(value):
    if not isinstance(value, str) or len(value) != 64:
        raise ValueError('invalid digest')
    try:
        result = bytes.fromhex(value)
        if result.hex() != value:
            raise ValueError('noncanonical digest')
        return result
    except ValueError as error:
        raise ValueError('invalid digest') from error


def safe_name(value):
    if not isinstance(value, str) or not value or '\\' in value or '\x00' in value:
        raise ValueError('unsafe catalogue path')
    path = PurePosixPath(value)
    if path.is_absolute() or any(p in ('', '.', '..') or ':' in p for p in value.split('/')):
        raise ValueError('unsafe catalogue path')
    return value


@contextmanager
def atomic_output(path):
    """Publish a completed file without replacing an existing destination."""
    path = Path(path)
    if path.exists() or path.is_symlink():
        raise ValueError(f'output already exists: {path}')
    fd, name = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w+b') as out:
            yield out
            out.flush()
            os.fsync(out.fileno())
        # link is exclusive even if another process creates path meanwhile.
        os.link(name, path)
    finally:
        os.unlink(name)


class View:
    """Sector-addressable immutable lengths, including virtual tail padding."""
    def __init__(self, streams, lengths, sector):
        self.streams, self.lengths, self.B = streams, lengths, sector
        self.counts = [(n + sector - 1) // sector for n in lengths]
        self.starts = [0]
        for count in self.counts:
            self.starts.append(self.starts[-1] + count)
        self.count = self.starts[-1]

    def locate(self, index):
        if not 0 <= index < self.count:
            raise ValueError('sector outside source image')
        volume = bisect.bisect_right(self.starts, index) - 1
        offset = (index - self.starts[volume]) * self.B
        return volume, offset, min(self.B, self.lengths[volume] - offset)

    def read(self, index):
        volume, offset, valid = self.locate(index)
        stream = self.streams[volume]
        if stream is None:
            return None
        stream.seek(offset)
        value = stream.read(valid)
        return value.ljust(self.B, b'\0') if len(value) == valid else None

    def write(self, index, value):
        volume, offset, valid = self.locate(index)
        stream = self.streams[volume]
        if stream is None:
            raise ValueError('missing output volume')
        stream.seek(offset)
        stream.write(value[:valid])

    def read_bytes(self, volume, offset, count):
        while count:
            index = self.starts[volume] + offset//self.B
            value = self.read(index)
            if value is None:
                raise ValueError('unreadable extraction extent')
            within = offset % self.B
            length = min(count, self.B-within)
            yield value[within:within+length]
            count -= length
            offset += length

    def write_bytes(self, volume, offset, value):
        at = 0
        while at < len(value):
            index = self.starts[volume] + offset//self.B
            sector = bytearray(self.read(index) or bytes(self.B))
            within = offset % self.B
            length = min(len(value)-at, self.B-within)
            sector[within:within+length] = value[at:at+length]
            self.write(index, bytes(sector))
            at += length
            offset += length


class OverlayView(View):
    """Read-only originals with sparse scratch writes for selected recovery."""
    def __init__(self, streams, lengths, sector, stack):
        super().__init__(streams, lengths, sector)
        self.overlays = [stack.enter_context(tempfile.TemporaryFile()) for _ in streams]
        self.changed = bytearray(self.count)

    def read(self, index):
        if not self.changed[index]:
            return super().read(index)
        volume, offset, valid = self.locate(index)
        self.overlays[volume].seek(offset)
        value = self.overlays[volume].read(valid)
        return value.ljust(self.B, b'\0') if len(value) == valid else None

    def write(self, index, value):
        volume, offset, valid = self.locate(index)
        self.overlays[volume].seek(offset)
        self.overlays[volume].write(value[:valid])
        self.changed[index] = 1


class Extents:
    def __init__(self, extents, total):
        self.extents = []
        self.starts = [0]
        previous_end = 0
        for start, count in extents:
            integer(start, previous_end, total - 1)
            integer(count, 1, total - start)
            self.extents.append((start, count))
            self.starts.append(self.starts[-1] + count)
            previous_end = start + count
        if not self.extents:
            raise ValueError('empty source map')
        self.count = self.starts[-1]
        self.global_starts = [start for start, _ in self.extents]

    def index(self, position):
        if position >= self.count:
            return None
        e = bisect.bisect_right(self.starts, position) - 1
        return self.extents[e][0] + position - self.starts[e]

    def position(self, index):
        e = bisect.bisect_right(self.global_starts, index)-1
        if e < 0 or index >= self.extents[e][0]+self.extents[e][1]:
            return None
        return self.starts[e]+index-self.extents[e][0]


def write_record(out, kind, B, root, group, row, count, meta_length, payload, family=0,
                 metadata_k=META_K, metadata_reserve=META_M):
    if len(payload) > MAX_RECORD:
        raise ValueError('record payload too large')
    header = bytearray(HEADER.pack(MAGIC, VERSION, kind, B, B, len(payload), 0,
                                  root, group, row, count, meta_length, digest(payload), family, 0))
    header.extend(struct.pack('<II',metadata_k,metadata_reserve))
    header.extend(bytes(B - len(header)))
    struct.pack_into('<I', header, 28, zlib.crc32(header))
    out.write(header)
    out.write(payload)
    out.write(bytes(-len(payload) % B))


def scan_records(path):
    """Resynchronize after missing headers without trusting corrupt lengths."""
    size = os.path.getsize(path)
    records = []
    with open(path, 'rb') as source:
        offset = 0
        while offset < size:
            source.seek(offset)
            chunk = source.read(1024 * 1024)
            found = chunk.find(MAGIC)
            if found < 0:
                if len(chunk) < len(MAGIC):
                    break
                offset += len(chunk) - len(MAGIC) + 1
                continue
            position = offset + found
            offset = position + 1
            source.seek(position)
            prefix = source.read(HEADER.size)
            if len(prefix) < HEADER.size:
                break
            values = HEADER.unpack(prefix)
            _, version, kind, B, header_size, length, crc, root, group, row, count, ml, sha, family, flags = values
            if B not in SECTORS or header_size != B or length > MAX_RECORD:
                continue
            source.seek(position)
            header = bytearray(source.read(B))
            if len(header) != B:
                continue
            header[28:32] = bytes(4)
            if zlib.crc32(header) != crc:
                continue
            if version != VERSION:
                # v1 has another CRC and layout; never reinterpret it.
                raise ValueError(f'unsupported RS04 archival version: {version}')
            if kind & 0x8000 and not flags and not any(header[BOOTSTRAP_BYTES:]):
                offset = position + B + ((length+B-1)//B)*B
                continue
            if flags or any(header[BOOTSTRAP_BYTES:]) or kind not in (META, PARITY):
                raise ValueError('unsupported required record semantics')
            metadata_k,metadata_reserve = struct.unpack_from('<II',header,HEADER.size)
            geometry(metadata_k,metadata_reserve)
            if not 4 <= ml <= (1 << 40):
                raise ValueError('invalid catalogue length')
            if kind == META and (length != SHARE or row >= metadata_k + metadata_reserve or count != 1 or family):
                raise ValueError('invalid metadata record')
            if kind == PARITY and (not 1 <= count <= 128 or length != count * B):
                raise ValueError('invalid parity record')
            records.append({'path': str(path), 'offset': position + B, 'kind': kind, 'B': B,
                            'length': length, 'root': root, 'group': group, 'row': row,
                            'count': count, 'meta_length': ml, 'sha': sha, 'family': family})
            records[-1]['metadata_k'] = metadata_k
            records[-1]['metadata_reserve'] = metadata_reserve
            if len(records) > MAX_RECORDS:
                raise ValueError('record inventory exceeds resource limit')
            offset = position + B + ((length + B - 1) // B) * B
    return records


def record_payload(record, verified=True):
    with open(record['path'], 'rb') as source:
        source.seek(record['offset'])
        payload = source.read(record['length'])
    if len(payload) != record['length'] or (verified and digest(payload) != record['sha']):
        return None
    return payload


class Archive:
    def __init__(self, records, codec, scratch, memory_mib=256):
        self.records = records
        first = records[0]
        self.B, self.root, self.length = first['B'], first['root'], first['meta_length']
        if any((r['B'], r['root'], r['meta_length']) != (self.B, self.root, self.length) for r in records):
            raise ValueError('conflicting archive bootstrap records')
        k,reserve = first['metadata_k'],first['metadata_reserve']
        if any((r['metadata_k'],r['metadata_reserve']) != (k,reserve) for r in records):
            raise ValueError('conflicting metadata coding parameters')
        if (4*k + 4*reserve + pow2(k+reserve))*SHARE > memory_mib*1024*1024:
            raise ValueError('metadata coding parameters exceed --memory-mib')
        groups = (self.length + k * SHARE - 1) // (k * SHARE)
        by_group = defaultdict(dict)
        for record in records:
            if record['kind'] == META:
                if record['group'] >= groups:
                    raise ValueError('metadata group outside catalogue')
                by_group[record['group']].setdefault(record['row'], []).append(record)
        if groups > len(by_group):
            raise ValueError('missing metadata groups')
        self.meta = scratch.enter_context(tempfile.TemporaryFile())
        for group in range(groups):
            shards = {}
            for row, copies in by_group[group].items():
                for record in copies:
                    payload = record_payload(record)
                    if payload is not None:
                        shards[row] = payload
                        break
            originals = [shards.get(i) if (group*k+i)*SHARE < self.length else bytes(SHARE) for i in range(k)]
            recovered = codec.decode(originals, reserve, {r-k:v for r,v in shards.items() if r>=k})
            if recovered is None:
                raise ValueError(f'unrecoverable catalogue group {group}')
            for row in range(k):
                value = originals[row] if originals[row] is not None else recovered[row]
                valid = min(len(value), max(0, self.length-self.meta.tell()))
                if any(value[valid:]):
                    raise ValueError('nonzero metadata padding')
                self.meta.write(value[:valid])
        if file_digest(self.meta) != self.root:
            raise ValueError('recovered catalogue fails SHA-256')
        self.meta.seek(0)
        length = struct.unpack('<I', self.meta.read(4))[0]
        if not 1 <= length <= min(MAX_JSON, self.length - 4):
            raise ValueError('invalid manifest length')
        raw = self.meta.read(length)
        self.manifest = parse_json(raw)
        if canonical(self.manifest) != raw:
            raise ValueError('noncanonical manifest')
        self.hash_start = 4 + length
        validate_manifest(self.manifest, self.B, self.length - self.hash_start)
        if source_identity(self.meta, self.B, self.manifest['lengths'], self.manifest['blocks'], self.hash_start) != self.manifest['source_hash']:
            raise ValueError('source identity does not commit the source fingerprint table')
        self.source_hash = self.manifest['source_hash']
        self.parity = defaultdict(list)
        self.add_parity_records(records)

    def add_parity_records(self, records):
        families = self.manifest['families']
        for record in records:
            if record['kind'] != PARITY:
                continue
            fi = record['family']
            if fi >= len(families):
                raise ValueError('unknown parity family')
            f = families[fi]
            if record['row'] not in f['rows'] or record['group'] + record['count'] > f['layer']:
                raise ValueError('parity record outside family inventory')
            self.parity[(f['id'], record['row'])].append(record)
        self.parity_starts = {}
        for key, packets in self.parity.items():
            packets.sort(key=lambda record: record['group'])
            self.parity_starts[key] = [record['group'] for record in packets]

    def hash_at(self, index):
        self.meta.seek(self.hash_start + index * 32)
        value = self.meta.read(32)
        if len(value) != 32:
            raise ValueError('hash index outside catalogue')
        return value

    def source_digest(self, index):
        return self.hash_at(index)

    def recovery(self, family, col):
        result = {}
        for slot, row in enumerate(family['rows']):
            expected = self.hash_at(family['hash_offset'] + slot * family['layer'] + col)
            key = (family['id'], row)
            packets = self.parity.get(key, [])
            at = bisect.bisect_right(self.parity_starts.get(key, []), col)-1
            while at >= 0 and packets[at]['group'] > col-128:
                record = packets[at]
                at -= 1
                relative = col-record['group']
                if relative >= record['count']:
                    continue
                with open(record['path'], 'rb') as source:
                    source.seek(record['offset'] + relative * self.B)
                    value = source.read(self.B)
                if len(value) == self.B and digest(value) == expected:
                    result[row] = value
                    break
        return result


def family_id(family, source_hash):
    descriptor = {key: family[key] for key in ('k', 'reserve', 'layer', 'extents', 'layout')}
    return digest(b'RS04v2 family\0' + hex_digest(source_hash) + canonical(descriptor)).hex()


def validate_manifest(m, B, hash_bytes):
    required = {'profile', 'sector', 'lengths', 'image_hashes', 'source_hash', 'blocks', 'families', 'files'}
    if not isinstance(m, dict) or set(m) != required or m['profile'] != VERSION or m['sector'] != B:
        raise ValueError('unsupported manifest profile')
    if not isinstance(m['lengths'], list) or not 1 <= len(m['lengths']) <= 64:
        raise ValueError('invalid source volume count')
    for length in m['lengths']:
        integer(length, 1, (1 << 63) - 1)
    total = sum((n + B - 1) // B for n in m['lengths'])
    integer(m['blocks'],1,1<<40)
    if m['blocks'] != total or total > (1 << 40):
        raise ValueError('invalid source block count')
    if not isinstance(m['image_hashes'], list) or len(m['image_hashes']) != len(m['lengths']):
        raise ValueError('invalid image hash list')
    for value in m['image_hashes'] + [m['source_hash']]:
        hex_digest(value)
    if not isinstance(m['families'], list) or not 1 <= len(m['families']) <= 64:
        raise ValueError('invalid family inventory')
    offset = total
    seen = set()
    for family in m['families']:
        if set(family) != {'id', 'k', 'reserve', 'layer', 'extents', 'layout', 'rows', 'hash_offset'}:
            raise ValueError('unsupported family descriptor')
        k = integer(family['k'], 1, 65535)
        reserve = integer(family['reserve'], 1, 32768)
        geometry(k, reserve)
        layer = integer(family['layer'], 1, total)
        extents = Extents(family['extents'], total)
        if family['layout'] not in ('layered', 'grouped', 'volumes'):
            raise ValueError('unsupported source layout')
        if family['layout'] == 'volumes':
            if k != len(m['lengths']) or family['extents'] != [[0, total]] or layer != max((n+B-1)//B for n in m['lengths']):
                raise ValueError('invalid volume stripe geometry')
        elif not (k - 1) * layer < extents.count <= k * layer:
            raise ValueError('invalid family source geometry')
        rows = family['rows']
        if not isinstance(rows, list) or not rows or len(set(rows)) != len(rows):
            raise ValueError('empty or duplicate recovery row IDs')
        for row in rows:
            integer(row, 0, reserve - 1)
        if family['id'] != family_id(family, m['source_hash']) or family['id'] in seen:
            raise ValueError('invalid or duplicate family identity')
        seen.add(family['id'])
        integer(family['hash_offset'],0,1<<40)
        if family['hash_offset'] != offset:
            raise ValueError('noncanonical fingerprint offsets')
        offset += layer * len(rows)
    if hash_bytes != offset * 32:
        raise ValueError('fingerprint inventory length mismatch')
    if not isinstance(m['files'], list) or len(m['files']) > 100000:
        raise ValueError('invalid file catalogue')
    names = set()
    for entry in m['files']:
        if set(entry) != {'path', 'volume', 'extents', 'sha256', 'length'}:
            raise ValueError('unsupported file descriptor')
        name = safe_name(entry['path'])
        if name in names:
            raise ValueError('duplicate catalogue path')
        names.add(name)
        volume = integer(entry['volume'], 0, len(m['lengths']) - 1)
        length = 0
        for start, count in entry['extents']:
            integer(start, 0, m['lengths'][volume] - 1)
            integer(count, 1, m['lengths'][volume] - start)
            length += count
        integer(entry['length'],0,(1<<63)-1)
        if entry['length'] != length:
            raise ValueError('file extent length mismatch')
        hex_digest(entry['sha256'])


def load_archives(paths, codec, stack, catalogue_hashes=None, memory_mib=256):
    grouped = defaultdict(list)
    selected = {hex_digest(value) for value in catalogue_hashes} if catalogue_hashes else None
    for path in paths:
        for record in scan_records(path):
            if selected is None or record['root'] in selected:
                grouped[(record['root'], record['B'])].append(record)
    if not grouped:
        raise ValueError('no RS04 v2 archival records found')
    if selected is not None and not selected.issubset({root for root,_ in grouped}):
        raise ValueError('a requested catalogue identity was not found')
    archives = []
    unavailable = []
    for records in grouped.values():
        encodings = defaultdict(list)
        for record in records:
            encodings[(record['metadata_k'],record['metadata_reserve'])].append(record)
        failures = []
        for shape, encoded in sorted(encodings.items(),key=lambda item:sum(item[0])):
            try:
                archive = Archive(encoded,codec,stack,memory_mib)
            except ValueError as error:
                failures.append(str(error))
                continue
            others = [r for r in records if (r['metadata_k'],r['metadata_reserve'])!=shape]
            archive.add_parity_records(others)
            archives.append(archive)
            break
        else:
            unavailable.append({'catalogue_hash': records[0]['root'].hex(), 'errors': failures})
    if not archives:
        raise ValueError('unrecoverable catalogue: '+json.dumps(unavailable))
    for archive in archives:
        archive.metadata_failures = unavailable
    roots = {a.source_hash for a in archives}
    if len(roots) != 1:
        raise ValueError('recovery volumes protect different source content; select --catalogue-hash when containers are nested')
    baseline = archives[0]
    # Hash equality of the catalogue itself is not a substitute for matching
    # the actual ordered source fingerprints across extension volumes.
    for archive in archives[1:]:
        if (archive.manifest['lengths'], archive.B) != (baseline.manifest['lengths'], baseline.B):
            raise ValueError('inconsistent source descriptors')
        if archive.manifest['image_hashes'] != baseline.manifest['image_hashes']:
            raise ValueError('conflicting whole-image digests')
        for i in range(baseline.manifest['blocks']):
            if archive.source_digest(i) != baseline.source_digest(i):
                raise ValueError('conflicting source fingerprints')
    return archives


def source_identity(meta, B, lengths, count, offset=0):
    meta.seek(offset)
    h = hashlib.sha256(b'RS04v2 source\0' + canonical({'sector': B, 'lengths': lengths}))
    remaining = count * 32
    while remaining:
        block = meta.read(min(remaining, 1024 * 1024))
        if not block:
            raise ValueError('truncated source fingerprints')
        h.update(block)
        remaining -= len(block)
    return h.hexdigest()


def positions(family, column, view):
    if family['layout'] == 'volumes':
        return [view.starts[j] + column if column < view.counts[j] else None
                for j in range(family['k'])]
    extents = Extents(family['extents'], view.count)
    return [extents.index(j * family['layer'] + column if family['layout'] == 'layered'
                          else column * family['k'] + j) for j in range(family['k'])]


def read_extents(stream, extents):
    for offset, length in extents:
        stream.seek(offset)
        while length:
            block = stream.read(min(length, 1024 * 1024))
            if not block:
                raise ValueError('truncated catalogue file extent')
            yield block
            length -= len(block)


def iso_catalogue(source, image_length):
    """Catalogue the primary ISO 9660 namespace, including multi extents.

    Extension namespaces are not inferred. Unsupported interleaved files,
    malformed records, cycles and out-of-image extents are explicit errors.
    """
    root = None
    for block in range(16, min(image_length // 2048, 256)):
        source.seek(block * 2048)
        descriptor = source.read(2048)
        if len(descriptor) != 2048 or descriptor[1:6] != b'CD001':
            break
        if descriptor[0] == 1:
            root = descriptor[156:190]
        if descriptor[0] == 255:
            break
    if root is None:
        return []
    def numbers(record):
        if len(record) < 34 or record[26] or record[27]:
            raise ValueError('unsupported ISO directory record')
        offset = int.from_bytes(record[2:6], 'little')
        size = int.from_bytes(record[10:14], 'little')
        if offset != int.from_bytes(record[6:10], 'big') or size != int.from_bytes(record[14:18], 'big'):
            raise ValueError('inconsistent ISO endian fields')
        offset = (offset + record[1]) * 2048
        if offset + size > image_length:
            raise ValueError('ISO extent outside image')
        return offset, size
    pending = [('', root)]
    visited = set()
    entries = {}
    continuing = set()
    while pending:
        parent, record = pending.pop()
        offset, length = numbers(record)
        if offset in visited:
            raise ValueError('cyclic or aliased ISO directory')
        visited.add(offset)
        if length > MAX_JSON * 16 or len(visited) > 100000:
            raise ValueError('ISO directory resource limit')
        source.seek(offset)
        data = source.read(length)
        at = 0
        while at < len(data):
            size = data[at]
            if size == 0:
                at = (at // 2048 + 1) * 2048
                continue
            rec = data[at:at+size]
            if len(rec) != size or size < 34 or rec[32] > size-33:
                raise ValueError('malformed ISO directory entry')
            at += size
            raw_name = rec[33:33+rec[32]]
            if raw_name in (b'\0', b'\1'):
                continue
            name = raw_name.decode('ascii')
            if name.endswith(';1'):
                name = name[:-2].rstrip('.')
            path = safe_name(parent + name)
            start, amount = numbers(rec)
            if rec[25] & 2:
                pending.append((path + '/', rec))
            else:
                if path in entries and path not in continuing:
                    raise ValueError('duplicate ISO primary file path')
                entry = entries.setdefault(path, {'path': path, 'volume': 0, 'extents': [], 'length': 0})
                if amount:
                    entry['extents'].append([start, amount])
                    entry['length'] += amount
                if rec[25] & 128:
                    continuing.add(path)
                else:
                    continuing.discard(path)
    if continuing:
        raise ValueError('incomplete ISO multi-extent file')
    for entry in entries.values():
        h = hashlib.sha256()
        for block in read_extents(source, entry['extents']):
            h.update(block)
        entry['sha256'] = h.hexdigest()
    return sorted(entries.values(), key=lambda entry: entry['path'])


def catalogue_entries(streams, lengths, explicit=None, automatic=True):
    if explicit:
        entries = load_json_file(explicit)
        if not isinstance(entries, list):
            raise ValueError('explicit catalogue must be an array of file descriptors')
        for entry in entries:
            safe_name(entry['path'])
            volume = integer(entry['volume'], 0, len(streams)-1)
            h, length = hashlib.sha256(), 0
            for start, count in entry['extents']:
                integer(start, 0, lengths[volume]-1)
                integer(count, 1, lengths[volume]-start)
                length += count
            for block in read_extents(streams[volume], entry['extents']):
                h.update(block)
            if entry.get('sha256', h.hexdigest()) != h.hexdigest():
                raise ValueError('explicit catalogue file hash differs from source')
            entry['sha256'], entry['length'] = h.hexdigest(), length
        return entries
    return iso_catalogue(streams[0], lengths[0]) if automatic and len(streams) == 1 else []


def make_family(total, k, reserve, rows, extents=None, layout='layered', layer=None):
    extents = extents or [[0, total]]
    count = Extents(extents, total).count
    k = min(k, count) if layout != 'volumes' else k
    geometry(k, reserve)
    if not rows or len(set(rows)) != len(rows) or any(not 0 <= r < reserve for r in rows):
        raise ValueError('invalid recovery row selection')
    layer = layer or (count + k - 1) // k
    # Use the smallest source count covering the chosen layer size.
    if layout != 'volumes':
        k = (count + layer - 1) // layer
    return {'k': k, 'reserve': reserve, 'layer': layer, 'extents': extents,
            'layout': layout, 'rows': sorted(rows)}


def create_archive(images, output, codec, families, sector=2048, files=None,
                   expected_source=None, augment=False, memory_mib=256, lengths_override=None,
                   metadata_k=META_K, metadata_reserve=None):
    if sector not in SECTORS:
        raise ValueError('unsupported sector size')
    with ExitStack() as stack:
        streams = [stack.enter_context(open(path, 'rb')) for path in images]
        actual_lengths = [os.fstat(s.fileno()).st_size for s in streams]
        lengths = lengths_override or actual_lengths
        if len(lengths) != len(streams) or any(a < n for a, n in zip(actual_lengths, lengths)):
            raise ValueError('source shorter than immutable image length')
        if not lengths or any(not n for n in lengths):
            raise ValueError('empty source images cannot be protected')
        if augment and len(images) != 1:
            raise ValueError('augmentation requires one source image')
        view = View(streams, lengths, sector)
        fingerprints = stack.enter_context(tempfile.TemporaryFile())
        for i in range(view.count):
            value = view.read(i)
            fingerprints.write(fingerprint(value, view.locate(i)[2], sector))
        source_hash = source_identity(fingerprints, sector, lengths, view.count)
        if expected_source and source_hash != expected_source:
            raise ValueError('source differs from the immutable family being extended')
        manifests, payloads = [], []
        hash_offset = view.count
        for proposed in families:
            f = dict(proposed)
            f['id'] = family_id(f, source_hash)
            f['hash_offset'] = hash_offset
            hash_offset += len(f['rows']) * f['layer']
            # Include transient Python buffers as well as native work memory.
            if (f['k'] + 4*f['reserve'] + pow2(f['k'] + f['reserve'])) * ((sector+63)//64*64) > memory_mib*1024*1024:
                raise ValueError('family exceeds --memory-mib; choose fewer source shards or a smaller reserve')
            spool = stack.enter_context(tempfile.TemporaryFile())
            spool.truncate(len(f['rows']) * f['layer'] * sector)
            for col in range(f['layer']):
                source = [view.read(index) if index is not None else bytes(sector)
                          for index in positions(f, col, view)]
                parity = codec.encode(source, f['reserve'], f['rows'])
                for slot, row in enumerate(f['rows']):
                    spool.seek((slot*f['layer'] + col)*sector)
                    spool.write(parity[row])
            spool.seek(0)
            for _ in range(len(f['rows']) * f['layer']):
                fingerprints.write(digest(spool.read(sector)))
            manifests.append(f)
            payloads.append(spool)
        manifest = {'profile': VERSION, 'sector': sector, 'lengths': lengths,
                    'image_hashes': [file_digest(s, n).hex() for s, n in zip(streams, lengths)],
                    'source_hash': source_hash, 'blocks': view.count,
                    'families': manifests, 'files': files or []}
        raw = canonical(manifest)
        if len(raw) > MAX_JSON:
            raise ValueError('manifest exceeds format bound')
        validate_manifest(manifest, sector, hash_offset*32)
        metadata = stack.enter_context(tempfile.TemporaryFile())
        metadata.write(struct.pack('<I', len(raw)))
        metadata.write(raw)
        fingerprints.seek(0)
        shutil.copyfileobj(fingerprints, metadata)
        ml = metadata.tell()
        root = file_digest(metadata)
        metadata_k = min(metadata_k,(ml+SHARE-1)//SHARE)
        if metadata_reserve is None:
            ratio = max(f['reserve']/f['k'] for f in manifests)
            metadata_reserve = pow2(max(META_M,math.ceil(2*ratio*metadata_k)))
        geometry(metadata_k,metadata_reserve)
        if (4*metadata_k+4*metadata_reserve+pow2(metadata_k+metadata_reserve))*SHARE > memory_mib*1024*1024:
            raise ValueError('metadata protection exceeds --memory-mib; adjust the reserve or memory budget')
        groups = (ml + metadata_k*SHARE-1)//(metadata_k*SHARE)
        meta_spool = stack.enter_context(tempfile.TemporaryFile())
        # Row-major storage separates shards of a metadata codeword.
        meta_spool.truncate(groups*(metadata_k+metadata_reserve)*SHARE)
        for group in range(groups):
            metadata.seek(group*metadata_k*SHARE)
            original = [metadata.read(SHARE).ljust(SHARE, b'\0') for _ in range(metadata_k)]
            parity = codec.encode(original, metadata_reserve)
            for row in range(metadata_k+metadata_reserve):
                meta_spool.seek((row*groups+group)*SHARE)
                meta_spool.write(original[row] if row<metadata_k else parity[row-metadata_k])
        parity_packets = sum(len(f['rows'])*((f['layer']+127)//128) for f in manifests)
        meta_order = [(row,group) for row in range(metadata_k+metadata_reserve) for group in range(groups)
                      if row>=metadata_k or (group*metadata_k+row)*SHARE < ml]
        meta_packets = len(meta_order)
        meta_done = packet_done = 0
        with atomic_output(output) as out:
            if augment:
                streams[0].seek(0)
                remaining = lengths[0]
                while remaining:
                    block = streams[0].read(min(remaining, 1024*1024))
                    out.write(block)
                    remaining -= len(block)
                out.write(bytes(-lengths[0] % sector))
            def emit_meta(target):
                nonlocal meta_done
                while meta_done < target:
                    row,group = meta_order[meta_done]
                    meta_spool.seek((row*groups+group)*SHARE)
                    write_record(out,META,sector,root,group,row,1,ml,meta_spool.read(SHARE),
                                 metadata_k=metadata_k,metadata_reserve=metadata_reserve)
                    meta_done += 1
            emit_meta(1)
            for fi, (f, spool) in enumerate(zip(manifests, payloads)):
                for slot, row in enumerate(f['rows']):
                    for col in range(0, f['layer'], 128):
                        count = min(128, f['layer']-col)
                        spool.seek((slot*f['layer']+col)*sector)
                        write_record(out, PARITY, sector, root, col, row, count, ml,
                                     spool.read(count*sector),fi,metadata_k,metadata_reserve)
                        packet_done += 1
                        emit_meta(meta_packets*packet_done//parity_packets)
            emit_meta(meta_packets)
        return {'source_hash': source_hash, 'catalogue_hash': root.hex(),
                'volumes': len(images), 'source_blocks': view.count,
                'families': [f['id'] for f in manifests], 'output': str(output),
                'engine': 'native FFT' if codec.native else 'scalar reference'}


class Recovery:
    def __init__(self, archives, view, codec, memory_mib=256, targets=None):
        self.archives, self.view, self.codec = archives, view, codec
        self.base = archives[0]
        self.B = self.base.B
        self.memory = memory_mib * 1024 * 1024
        if view.count*(4 if targets is not None else 1) > self.memory//2:
            raise ValueError('source-state bitmaps exceed --memory-mib')
        self.known = bytearray(view.count)
        self.targets = targets
        self.checked = bytearray(view.count) if targets is not None else None
        self.families = {}
        self.limits = {}
        for archive in archives:
            for f in archive.manifest['families']:
                self.families.setdefault(f['id'], []).append((archive, f))
        self.scan()

    def scan(self):
        for i in range(self.view.count):
            if self.targets is None or self.targets[i] or self.checked[i]:
                self.ensure(i, force=True)

    def ensure(self, index, force=False):
        if self.checked is not None and self.checked[index] and not force:
            return
        value = self.view.read(index)
        self.known[index] = value is not None and fingerprint(value, self.view.locate(index)[2], self.B) == self.base.source_digest(index)
        if self.checked is not None:
            self.checked[index] = 1

    def unresolved(self):
        return any(not good and (self.targets is None or self.targets[i]) for i,good in enumerate(self.known))

    def columns(self, family):
        if self.targets is None:
            return range(family['layer'])
        extents = Extents(family['extents'], self.view.count)
        result = set()
        for i, target in enumerate(self.targets):
            if not target or self.known[i]:
                continue
            if family['layout']=='volumes':
                result.add(self.view.locate(i)[1]//self.B)
            else:
                position = extents.position(i)
                if position is not None:
                    result.add(position % family['layer'] if family['layout']=='layered' else position//family['k'])
        return sorted(result)

    def accept(self, index, value):
        valid = self.view.locate(index)[2]
        if len(value) != self.B or any(value[valid:]) or fingerprint(value, valid, self.B) != self.base.source_digest(index):
            return False
        self.view.write(index, value)
        changed = not self.known[index]
        self.known[index] = 1
        if self.checked is not None:
            self.checked[index] = 1
        return changed

    def parity(self, entries, column):
        result = {}
        for archive, family in entries:
            for row, value in archive.recovery(family, column).items():
                if row in result and result[row] != value:
                    raise ValueError('conflicting parity for one immutable row identity')
                result[row] = value
        return result

    def ordinary(self):
        if self.targets is not None and not self.unresolved():
            return 0
        repaired = 0
        for entries in sorted(self.families.values(), key=lambda entries: entries[0][1]['k']):
            f = entries[0][1]
            for col in self.columns(f):
                indices = positions(f, col, self.view)
                if self.targets is not None and not any(i is not None and self.targets[i] and not self.known[i] for i in indices):
                    continue
                if self.checked is not None:
                    for i in indices:
                        if i is not None:
                            self.ensure(i)
                unknown = [j for j, i in enumerate(indices) if i is not None and not self.known[i]]
                if not unknown:
                    continue
                parity = self.parity(entries, col)
                if len(unknown) > len(parity):
                    continue
                if (f['k'] + pow2(f['k']+f['reserve']) + f['reserve']) * ((self.B+63)//64*64) > self.memory:
                    self.limits[f['id']] = 'native family working set exceeds --memory-mib'
                    continue
                originals = [bytes(self.B) if i is None else self.view.read(i) if self.known[i] else None
                             for i in indices]
                recovered = self.codec.decode(originals, f['reserve'], parity)
                if recovered is not None:
                    # Validate the entire candidate codeword before writing.
                    if any(fingerprint(value, self.view.locate(indices[j])[2], self.B) != self.base.source_digest(indices[j])
                           for j, value in recovered.items()):
                        raise ValueError('recovered data fails independent fingerprint')
                    for j, value in recovered.items():
                        repaired += self.accept(indices[j], value)
        return repaired

    def donors(self, paths, shifted=False):
        import sqlite3
        repaired = 0
        with tempfile.TemporaryDirectory(prefix='rs04-donors-') as temporary:
            db = sqlite3.connect(str(Path(temporary)/'index.sqlite'))
            try:
                db.execute('CREATE TABLE wanted (hash BLOB, sector INTEGER)')
                db.executemany('INSERT INTO wanted VALUES (?,?)',
                               ((self.base.source_digest(i), i) for i, known in enumerate(self.known)
                                if not known and (self.checked is None or self.checked[i] or self.targets[i])))
                db.execute('CREATE INDEX lookup ON wanted(hash)')
                for path in paths:
                    candidates = [path] if Path(path).is_file() else (p for p in Path(path).rglob('*') if p.is_file() and not p.is_symlink())
                    for candidate in candidates:
                        with open(candidate, 'rb') as source:
                            size = os.fstat(source.fileno()).st_size
                            # Match extracted files by content, never by name.
                            matches = [e for e in self.base.manifest['files'] if e['length'] == size]
                            if matches:
                                sha = file_digest(source).hex()
                                for entry in matches:
                                    if entry['sha256'] != sha:
                                        continue
                                    source.seek(0)
                                    for offset, count in entry['extents']:
                                        remaining = count
                                        while remaining:
                                            chunk = source.read(min(remaining, 1024*1024))
                                            self.view.write_bytes(entry['volume'],offset,chunk)
                                            offset += len(chunk)
                                            remaining -= len(chunk)
                            step = 1 if shifted else self.B
                            at = 0
                            while at < size:
                                source.seek(at)
                                value = source.read(self.B)
                                valid = len(value)
                                if not valid:
                                    break
                                key = fingerprint(value, valid, self.B)
                                for (index,) in db.execute('SELECT sector FROM wanted WHERE hash=?', (key,)):
                                    if not self.known[index]:
                                        repaired += self.accept(index, value.ljust(self.B, b'\0'))
                                at += step
                before = sum(self.known)
                self.scan()
                repaired += sum(self.known) - before
            finally:
                db.close()
        return repaired

    def joint(self, limit):
        # Components are built from verified equations, not from shared file
        # names or duplicated recovery volumes. Bound the graph independently
        # of the maximum size of one elimination problem.
        missing = []
        for i, good in enumerate(self.known):
            if not good and (self.checked is None or self.checked[i]):
                missing.append(i)
                if len(missing) > self.memory // 768:
                    return 0, 'residual graph exceeds memory limit'
        if not missing:
            return 0, 'complete'
        parent = {i:i for i in missing}
        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i
        def union(a, b):
            a, b = find(a), find(b)
            if a != b:
                parent[b] = a
        tasks = []
        for entries in self.families.values():
            f = entries[0][1]
            for col in range(f['layer']):
                indices = positions(f, col, self.view)
                if self.checked is not None and any(i is not None and not self.checked[i] for i in indices):
                    continue
                unknown = [i for i in indices if i in parent]
                if not unknown or not self.parity(entries, col):
                    continue
                for i in unknown[1:]:
                    union(unknown[0], i)
                tasks.append((entries, col, unknown[0]))
                if len(tasks) > self.memory // 768:
                    return 0, 'residual equation graph exceeds memory limit'
        components = defaultdict(list)
        for i in missing:
            components[find(i)].append(i)
        jobs = defaultdict(list)
        for entries, col, anchor in tasks:
            jobs[find(anchor)].append((entries, col))
        recovered_count = limited = deficient = 0
        for root, unknown in components.items():
            if len(unknown) > limit or len(unknown)*(len(unknown)*32+self.B*4) > self.memory:
                limited += 1
                continue
            lookup = {index:j for j,index in enumerate(unknown)}
            clipped = [False]
            def equations():
                count = 0
                for entries, col in jobs[root]:
                    f = entries[0][1]
                    indices = positions(f, col, self.view)
                    for row, parity in self.parity(entries, col).items():
                        coefficients = [0]*len(unknown)
                        value = bytearray(pack(parity))
                        for j, index in enumerate(indices):
                            if index is None:
                                continue
                            factor = coefficient(f['reserve'], row, j)
                            if index in lookup:
                                coefficients[lookup[index]] ^= factor
                            else:
                                self.codec.add_scaled(value, pack(self.view.read(index)), factor)
                        yield coefficients, value
                        count += 1
                        if count >= max(64, limit*16):
                            clipped[0] = True
                            return
            result = eliminate(equations(), len(unknown), self.codec.add_scaled)
            if result is None:
                if clipped[0]:
                    limited += 1
                else:
                    deficient += 1
                continue
            values = [unpack(v, self.B) for v in result]
            if any(fingerprint(v, self.view.locate(i)[2], self.B) != self.base.source_digest(i)
                   for i,v in zip(unknown,values)):
                raise ValueError('joint reconstruction fails independent fingerprints')
            recovered_count += sum(self.accept(i,v) for i,v in zip(unknown,values))
        if not limited and not deficient:
            return recovered_count, 'full rank'
        return recovered_count, f'{limited} components limited; {deficient} components rank deficient'

    def partial(self, hints, granularity=64):
        repaired = 0
        masks = {}
        for key, ranges in hints.items():
            i = integer(int(key), 0, self.view.count-1)
            mask = bytearray(self.B)
            for start, length in ranges:
                integer(start, 0, self.B-1)
                integer(length, 1, self.B-start)
                mask[start:start+length] = bytes([1])*length
            masks[i] = mask
        chunks = []
        for base in range(0, self.B, 64):
            size = min(64, self.B-base)
            if granularity == 64:
                chunks.append(list(range(base, base+size)))
            else:
                chunks.extend([[base+j, base+size//2+j] for j in range(size//2)])
        for entries in self.families.values():
            f = entries[0][1]
            if f['k']*self.B*4 > self.memory:
                continue
            for col in range(f['layer']):
                indices = positions(f, col, self.view)
                if not any(i in masks and not self.known[i] for i in indices):
                    continue
                parity = self.parity(entries, col)
                if not parity:
                    continue
                values = [bytearray(self.view.read(i) or bytes(self.B)) if i is not None else bytearray(self.B) for i in indices]
                valid = [bytearray([1])*self.B if i is None or self.known[i] else bytearray(masks.get(i, bytes(self.B))) for i in indices]
                for chunk in chunks:
                    original = [bytes(value[p] for p in chunk) if all(mask[p] for p in chunk) else None for value,mask in zip(values,valid)]
                    if sum(v is None for v in original) > len(parity):
                        continue
                    result = self.codec.decode(original, f['reserve'], {r: bytes(v[p] for p in chunk) for r,v in parity.items()})
                    if result is None:
                        continue
                    for j, value in result.items():
                        for p, byte in zip(chunk, value):
                            values[j][p] = byte
                            valid[j][p] = 1
                for i, value, mask in zip(indices, values, valid):
                    if i is not None and not self.known[i] and all(mask):
                        repaired += self.accept(i, bytes(value))
        return repaired

    def report(self):
        checked = self.view.count if self.checked is None else sum(self.checked)
        missing = checked - sum(self.known)
        whole = []
        for stream, length, sha in zip(self.view.streams, self.view.lengths, self.base.manifest['image_hashes']):
            if self.checked is not None:
                whole.append(None)
                continue
            try:
                whole.append(stream is not None and file_digest(stream, length).hex() == sha)
            except ValueError:
                whole.append(False)
        return {'source_hash': self.base.source_hash, 'source_blocks': self.view.count,
                'verified_blocks': sum(self.known), 'missing_or_corrupt_blocks': missing,
                'unexamined_blocks': self.view.count-checked,
                'resource_limits': self.limits,
                'unavailable_catalogues': getattr(self.base, 'metadata_failures', []),
                'images_verified': whole, 'complete': not missing and all(whole)}


@contextmanager
def recovery_outputs(images, outputs, lengths, resume, source_hash):
    """Keep .partial checkpoints on interruption; rehash them on every resume."""
    if len(outputs) != len(lengths) or len(images) != len(lengths):
        raise ValueError('one source and output path is required per volume; use - for a missing source')
    destinations = [Path(path) for path in outputs]
    all_outputs = destinations + [Path(str(p)+'.partial') for p in destinations] + [Path(str(p)+'.partial.json') for p in destinations]
    if len({str(p.resolve()) for p in all_outputs}) != len(all_outputs):
        raise ValueError('output paths must be distinct')
    for source in images:
        if source != '-' and any(Path(source).resolve() == p.resolve() or
                                (p.exists() and os.path.samefile(source, p)) for p in all_outputs):
            raise ValueError('repair output cannot replace a source image')
    with ExitStack() as stack:
        streams = []
        for number, (source, path, length) in enumerate(zip(images, destinations, lengths)):
            if path.exists() or path.is_symlink():
                raise ValueError(f'output already exists: {path}')
            partial = Path(str(path)+'.partial')
            marker = Path(str(path)+'.partial.json')
            expected = {'source_hash': source_hash, 'volume': number, 'length': length}
            if partial.is_symlink():
                raise ValueError('checkpoint is a symbolic link')
            if partial.exists():
                if not resume:
                    raise ValueError(f'checkpoint exists; use --resume: {partial}')
                if marker.is_symlink() or not marker.is_file() or load_json_file(marker) != expected:
                    raise ValueError('checkpoint identity differs from requested source')
                stream = stack.enter_context(open(partial, 'r+b'))
            else:
                if marker.exists() or marker.is_symlink():
                    raise ValueError('orphaned checkpoint identity file')
                stream = stack.enter_context(open(partial, 'x+b'))
                with atomic_output(marker) as out:
                    out.write(canonical(expected))
                if source != '-':
                    with open(source, 'rb') as original:
                        remaining = length
                        while remaining:
                            value = original.read(min(remaining, 1024*1024))
                            if not value:
                                break
                            stream.write(value)
                            remaining -= len(value)
            stream.truncate(length)
            streams.append(stream)
        yield streams
        for stream in streams:
            stream.flush()
            os.fsync(stream.fileno())


def publish_outputs(outputs):
    for output in outputs:
        partial = str(output)+'.partial'
        os.link(partial, output)
        os.unlink(partial)
        os.unlink(str(output)+'.partial.json')


def extract_files(recovery, directory, selected=None):
    if not recovery.base.manifest['files']:
        raise ValueError('no rescue file catalogue is available')
    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    root = root.resolve()
    result = []
    for entry in recovery.base.manifest['files']:
        if selected and entry['path'] not in selected:
            continue
        safe_name(entry['path'])
        volume = entry['volume']
        affected = set()
        for offset, count in entry['extents']:
            affected.update(range(recovery.view.starts[volume]+offset//recovery.B,
                                  recovery.view.starts[volume]+(offset+count+recovery.B-1)//recovery.B))
        if any(not recovery.known[i] for i in affected):
            result.append({'path': entry['path'], 'status': 'unrecoverable'})
            continue
        path = root / entry['path']
        parent = root
        for component in PurePosixPath(entry['path']).parts[:-1]:
            parent = parent / component
            if parent.is_symlink():
                raise ValueError('catalogue extraction refuses symbolic-link directories')
            parent.mkdir(exist_ok=True)
        h = hashlib.sha256()
        with atomic_output(path) as out:
            for offset,count in entry['extents']:
                for value in recovery.view.read_bytes(volume,offset,count):
                    h.update(value)
                    out.write(value)
            if h.hexdigest() != entry['sha256']:
                raise ValueError('extracted file fails independent file digest')
        result.append({'path': entry['path'], 'status': 'verified'})
    if selected and not set(selected).issubset({e['path'] for e in result}):
        raise ValueError('selected file is not in the rescue catalogue')
    return result


def row_selection(value, count, reserve):
    if value is None:
        return list(range(count))
    rows = []
    for term in value.split(','):
        if ':' in term:
            start, end = map(int, term.split(':'))
            if not 0 <= start < end <= reserve:
                raise ValueError('row interval outside reserve')
            rows.extend(range(start, end))
        else:
            rows.append(integer(int(term), 0, reserve-1))
    if not rows or len(rows) != len(set(rows)):
        raise ValueError('empty or duplicated row selection')
    return sorted(rows)


def source_setup(paths, B, stack):
    streams = [stack.enter_context(open(path, 'rb')) for path in paths]
    lengths = [os.fstat(s.fileno()).st_size for s in streams]
    return streams, lengths, View(streams, lengths, B)


def command_create(args, codec):
    with ExitStack() as stack:
        streams, lengths, view = source_setup([args.image], args.sector, stack)
        k = min(args.data_shards, view.count)
        count = args.parity if args.parity is not None else max(1, math.ceil(k/5))
        reserve = args.reserve or pow2(max(count, (k+1)//2))
        rows = row_selection(args.rows, count, reserve)
        families = [make_family(view.count, k, reserve, rows)]
        if args.protect:
            extents = sorted([list(map(int, value.split(':'))) for value in args.protect])
            n = Extents(extents, view.count).count
            pk = min(k, n)
            pm = max(1, (pk+2)//3)
            families.append(make_family(view.count, pk, pow2(max(pm, (pk+1)//2)), list(range(pm)), extents))
        if args.local_group:
            families.append(make_family(view.count, args.local_group, 1, [0], layout='grouped'))
        files = catalogue_entries(streams, lengths, args.catalogue, not args.no_catalogue)
    return create_archive([args.image], args.output, codec, families, args.sector, files,
                          augment=args.augment, memory_mib=args.memory_mib,
                          metadata_k=args.metadata_shards, metadata_reserve=args.metadata_reserve)


def command_extend(args, codec):
    with ExitStack() as stack:
        archives = load_archives(args.archives, codec, stack, args.catalogue_hash, args.memory_mib)
        base = archives[0]
        index = integer(args.family, 0, len(base.manifest['families'])-1)
        family = dict(base.manifest['families'][index])
        rows = row_selection(args.rows, 0, family['reserve'])
        used = set()
        for archive in archives:
            for f in archive.manifest['families']:
                if f['id'] == family['id']:
                    used.update(f['rows'])
        if used.intersection(rows):
            raise ValueError('requested parity rows already exist; copies do not add independent equations')
        family['rows'] = rows
        return create_archive(args.images, args.output, codec, [family], base.B,
                              base.manifest['files'], expected_source=base.source_hash,
                              memory_mib=args.memory_mib, lengths_override=base.manifest['lengths'],
                              metadata_k=args.metadata_shards, metadata_reserve=args.metadata_reserve)


def command_collection(args, codec):
    if not 1 <= args.parity <= 32768:
        raise ValueError('invalid collection parity count')
    with ExitStack() as stack:
        streams, lengths, view = source_setup(args.images, args.sector, stack)
        reserve = args.reserve or pow2(args.parity)
        geometry(len(args.images), reserve)
        files = catalogue_entries(streams, lengths, args.catalogue, False)
        outputs = [f'{args.output_prefix}.{row:04d}.ecc' for row in range(args.parity)]
        if any(Path(p).exists() or Path(p).is_symlink() for p in outputs):
            raise ValueError('a collection output already exists')
        results = []
        for row, output in enumerate(outputs):
            family = make_family(view.count, len(args.images), reserve, [row],
                                 layout='volumes', layer=max(view.counts))
            results.append(create_archive(args.images, output, codec, [family], args.sector,
                                          files, memory_mib=args.memory_mib,
                                          metadata_k=args.metadata_shards, metadata_reserve=args.metadata_reserve))
        return {'outputs': results}


def command_recover(args, codec):
    with ExitStack() as stack:
        archives = load_archives(args.archives, codec, stack, args.catalogue_hash, args.memory_mib)
        base = archives[0]
        lengths = base.manifest['lengths']
        if len(args.images) != len(lengths):
            raise ValueError('supply source images in recorded volume order, with - for a missing image')
        if args.command in ('verify', 'info'):
            streams = [stack.enter_context(open(p, 'rb')) if p != '-' else None for p in args.images]
            recovery = Recovery(archives, View(streams, lengths, base.B), codec, args.memory_mib)
            report = recovery.report()
            if args.command == 'info':
                report['catalogues'] = [a.manifest for a in archives]
            return report
        targets = None
        if args.command == 'extract':
            files = base.manifest['files']
            if not files:
                raise ValueError('no rescue file catalogue is available')
            if args.file and not set(args.file).issubset({e['path'] for e in files}):
                raise ValueError('selected file is not in the rescue catalogue')
            if base.manifest['blocks']*4 > args.memory_mib*1024*1024//2:
                raise ValueError('selective recovery bitmaps exceed --memory-mib')
            targets = bytearray(base.manifest['blocks'])
            streams = [stack.enter_context(open(p,'rb')) if p!='-' else None for p in args.images]
            view = OverlayView(streams,lengths,base.B,stack)
            for entry in files:
                if args.file and entry['path'] not in args.file:
                    continue
                for offset,count in entry['extents']:
                    start = view.starts[entry['volume']] + offset//base.B
                    end = view.starts[entry['volume']] + (offset+count+base.B-1)//base.B
                    targets[start:end] = bytes([1])*(end-start)
            workspace = nullcontext(view)
        else:
            outputs, resume = args.outputs, args.resume
            workspace = recovery_outputs(args.images, outputs, lengths, resume, base.source_hash)
        with workspace as working:
            view = working if args.command=='extract' else View(working,lengths,base.B)
            recovery = Recovery(archives,view,codec,args.memory_mib,targets)
            before = sum(recovery.known)
            if args.donor:
                recovery.donors(args.donor, args.search_shifted)
            while recovery.ordinary():
                pass
            joint_status = 'disabled'
            if args.partial_map:
                hints = load_json_file(args.partial_map)
                recovery.partial(hints, args.partial_granularity)
                while recovery.ordinary():
                    pass
            if args.joint_limit and recovery.unresolved():
                _, joint_status = recovery.joint(args.joint_limit)
            report = recovery.report()
            report['recovered_blocks'] = sum(recovery.known) - before
            report['joint_status'] = joint_status
            if args.command == 'extract':
                report['files'] = extract_files(recovery, args.directory, args.file)
                report['selected_files_verified'] = all(e['status']=='verified' for e in report['files'])
            elif args.extract:
                report['files'] = extract_files(recovery, args.extract)
        if args.command == 'repair':
            if report['complete']:
                publish_outputs(outputs)
                report['outputs'] = outputs
                if args.rebuild_ecc:
                    directory = Path(args.rebuild_ecc)
                    directory.mkdir(parents=True, exist_ok=True)
                    report['rebuilt_ecc'] = []
                    for number, archive in enumerate(archives):
                        output = directory/f'recovery-{number:04d}.ecc'
                        report['rebuilt_ecc'].append(create_archive(outputs, output, codec,
                            archive.manifest['families'], archive.B, archive.manifest['files'],
                            expected_source=archive.source_hash, memory_mib=args.memory_mib,
                            metadata_k=args.metadata_shards, metadata_reserve=args.metadata_reserve))
            else:
                report['checkpoints'] = [str(p)+'.partial' for p in outputs]
        return report


def command_kit(args):
    directory = Path(args.directory)
    directory.mkdir()
    source = Path(__file__).resolve().parent
    files = ['rs04_archive.py', 'rs04_archive_codec.py']
    for name in files:
        shutil.copyfile(source/name, directory/name)
    spec = source.parent/'documentation'/'codec-specs'/'rs04-archival.md'
    if not spec.is_file():
        spec = source/'FORMAT.md'
    if spec.is_file():
        shutil.copyfile(spec, directory/'FORMAT.md')
        files.append('FORMAT.md')
    license_file = source/'COPYING' if (source/'COPYING').is_file() else source.parent/'COPYING'
    for candidate, name in ((source/'rs04_archive_vectors.json', 'vectors.json'),
                             (source/'rs04-archive-vectors.py', 'rs04-archive-vectors.py'),
                             (license_file, 'COPYING')):
        if candidate.is_file():
            shutil.copyfile(candidate, directory/name)
            files.append(name)
    (directory/'README.txt').write_text('RS04 v2 development recovery kit\n\n'
        'Requires Python 3.8 or later with its standard library. No network or third-party packages.\n'
        'Run: python3 rs04_archive.py --scalar --help\n'
        'This is an experimental format, not a frozen archival release.\n', encoding='utf-8')
    files.append('README.txt')
    manifest = {name: digest((directory/name).read_bytes()).hex() for name in files}
    (directory/'kit-manifest.json').write_bytes(canonical(manifest)+b'\n')
    return {'directory': str(directory), 'files': manifest}


def command_inspect(paths):
    containers = {}
    for path in paths:
        for record in scan_records(path):
            key = (str(path),record['root'])
            entry = containers.setdefault(key,{'path':str(path),'catalogue_hash':record['root'].hex(),
                'sector':record['B'],'catalogue_bytes':record['meta_length'],'records':0,
                'first_record':record['offset']-record['B'],'last_record':0})
            entry['records'] += 1
            entry['last_record'] = max(entry['last_record'],record['offset']-record['B'])
    if not containers:
        raise ValueError('no RS04 v2 archival bootstrap records found')
    return {'bootstrap_only':True,'catalogues':list(containers.values())}


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--scalar', action='store_true', help='use the independent offline scalar implementation')
    p.add_argument('--codec-library', help='path to the optional native codec shared library')
    p.add_argument('--memory-mib', type=int, default=256, help='working-memory budget (default: 256 MiB)')
    p.add_argument('--metadata-shards', type=int, default=META_K, help='maximum original shards per metadata group (default: 64)')
    p.add_argument('--metadata-reserve', type=int, help='metadata parity shards per group; default scales with source protection')
    commands = p.add_subparsers(dest='command', required=True)
    c = commands.add_parser('create', help='create an independently recoverable v2 ECC volume')
    c.add_argument('image')
    c.add_argument('output')
    c.add_argument('--sector', type=int, choices=SECTORS, default=2048)
    c.add_argument('--data-shards', type=int, default=1024)
    c.add_argument('--reserve', type=int, help='fixed power-of-two parity capacity')
    c.add_argument('--parity', type=int, help='number of parity rows to write initially')
    c.add_argument('--rows', help='explicit row IDs, e.g. 0,2,4:8 (end exclusive)')
    c.add_argument('--catalogue', help='explicit JSON file catalogue with byte extents')
    c.add_argument('--no-catalogue', action='store_true', help='disable primary ISO 9660 catalogue discovery')
    c.add_argument('--augment', action='store_true', help='write source bytes followed by v2 records')
    c.add_argument('--protect', action='append', metavar='START:COUNT', help='sector extent receiving extra protection')
    c.add_argument('--local-group', type=int, default=0, help='add one local XOR row per contiguous group')
    e = commands.add_parser('extend', help='write additional rows of an existing immutable family')
    e.add_argument('output')
    e.add_argument('--archives', nargs='+', required=True)
    e.add_argument('--catalogue-hash', action='append', help='select a catalogue identity in a nested container')
    e.add_argument('--images', nargs='+', required=True)
    e.add_argument('--family', type=int, default=0)
    e.add_argument('--rows', required=True)
    cc = commands.add_parser('collection-create', help='write separate parity volumes protecting complete images')
    cc.add_argument('images', nargs='+')
    cc.add_argument('--output-prefix', required=True)
    cc.add_argument('--parity', type=int, default=2)
    cc.add_argument('--reserve', type=int)
    cc.add_argument('--sector', type=int, choices=SECTORS, default=2048)
    cc.add_argument('--catalogue')
    inspect = commands.add_parser('inspect',help='list bootstrap catalogue identities without decoding their contents')
    inspect.add_argument('archives',nargs='+')
    for action in ('verify', 'info', 'repair', 'extract'):
        command = commands.add_parser(action)
        command.add_argument('archives', nargs='+')
        command.add_argument('--catalogue-hash',action='append',help='select a catalogue identity; repeat for extension volumes')
        command.add_argument('--images', nargs='+', required=True,
                             help='source images in volume order; - denotes a missing image')
        if action in ('repair', 'extract'):
            command.add_argument('--donor', action='append', help='file or directory of other surviving data')
            command.add_argument('--search-shifted', action='store_true', help='also scan donors at every byte offset (slow)')
            command.add_argument('--joint-limit', type=int, default=64, help='maximum unknown sectors per joint component; 0 disables')
            command.add_argument('--partial-map', help='JSON byte validity hints by global sector index')
            command.add_argument('--partial-granularity', type=int, choices=(2,64), default=64)
        if action == 'repair':
            command.add_argument('--outputs', nargs='+', required=True)
            command.add_argument('--resume', action='store_true')
            command.add_argument('--rebuild-ecc', help='directory for verified regenerated ECC volumes')
            command.add_argument('--extract', help='also extract verified catalogue files')
        if action == 'extract':
            command.add_argument('--directory', required=True)
            command.add_argument('--file', action='append', help='catalogue path to recover; repeat to select files')
    kit = commands.add_parser('kit', help='export an offline standard-library recovery kit')
    kit.add_argument('directory')
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        integer(args.memory_mib, 8, 1024*1024)
        integer(args.metadata_shards, 1, 256)
        if args.metadata_reserve is not None:
            geometry(1, args.metadata_reserve)
        codec = Codec(args.codec_library, args.scalar)
        if args.command == 'create':
            integer(args.data_shards, 1, 65535)
            integer(args.local_group, 0, 65535)
            result = command_create(args, codec)
        elif args.command == 'extend':
            result = command_extend(args, codec)
        elif args.command == 'collection-create':
            result = command_collection(args, codec)
        elif args.command == 'kit':
            result = command_kit(args)
        elif args.command == 'inspect':
            result = command_inspect(args.archives)
        else:
            if hasattr(args, 'joint_limit'):
                integer(args.joint_limit, 0, 4096)
            result = command_recover(args, codec)
        print(json.dumps(result, sort_keys=True, indent=2))
        if args.command == 'extract':
            return 0 if result['selected_files_verified'] else 2
        return 2 if result.get('complete') is False else 0
    except (OSError, ValueError, KeyError, TypeError, struct.error, OverflowError, ImportError) as error:
        print(json.dumps({'error': str(error)}), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('Interrupted; repair checkpoints are retained as .partial files.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
