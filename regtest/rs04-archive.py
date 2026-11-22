#!/usr/bin/env python3
"""End-to-end damage tests for the RS04 archival development profile."""
import contextlib
import io
import itertools
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'contrib'))
import rs04_archive as archive
from rs04_archive_codec import Codec, coefficient, pack, unpack


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='rs04-archive-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.codec = Codec()
        self.rng = random.Random(731)

    def data(self, count, B=2048, tail=0, name='source.bin'):
        value = self.rng.randbytes(count*B+tail)
        path = self.root/name
        path.write_bytes(value)
        return path, value

    def create(self, source, families=None, name='base.ecc', B=2048, files=None, augment=False):
        count = (source.stat().st_size+B-1)//B
        if families is None:
            families = [archive.make_family(count, min(4,count), 4, [0,2])]
        path = self.root/name
        archive.create_archive([source], path, self.codec, families, B, files, augment=augment)
        return path

    def cli(self, *args, code=0):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            actual = archive.main([str(v) for v in args])
        self.assertEqual(actual, code, stderr.getvalue()+stdout.getvalue())
        return json.loads(stdout.getvalue()) if stdout.getvalue() else json.loads(stderr.getvalue())

    def damage(self, source, indices, B=2048, name='damaged.bin'):
        value = bytearray(source.read_bytes())
        for i in indices:
            start, end = i*B, min(len(value),(i+1)*B)
            value[start:end] = bytes(end-start)
        path = self.root/name
        path.write_bytes(value)
        return path

    def corrupt_record(self, path, record, header=False):
        with open(path, 'r+b') as out:
            offset = record['offset'] - record['B'] if header else record['offset']
            out.seek(offset)
            value = out.read(1)
            out.seek(offset)
            out.write(bytes([value[0]^0x80]))

    def test_native_and_reference_agree(self):
        if not self.codec.native:
            self.skipTest('native acceleration is not built')
        scalar = Codec(scalar=True)
        self.assertIsNotNone(self.codec.region)
        for factor in (0,1,0xCAFE,0xFFFF):
            source=self.rng.randbytes(64)
            native_value=bytearray(self.rng.randbytes(64))
            scalar_value=bytearray(native_value)
            self.codec.add_scaled(native_value,source,factor)
            scalar.add_scaled(scalar_value,source,factor)
            self.assertEqual(native_value,scalar_value)
        for B in (64, 2048, 2324, 2352):
            for k, reserve in ((1,1),(1,4),(3,2),(5,4),(2,8)):
                with self.subTest(B=B,k=k,reserve=reserve):
                    source = [self.rng.randbytes(B) for _ in range(k)]
                    parity = self.codec.encode(source, reserve)
                    self.assertEqual(parity, scalar.encode(source, reserve))
                    source[0], expected = None, source[0]
                    rows = {reserve-1:parity[reserve-1]}
                    self.assertEqual(self.codec.decode(source,reserve,rows), {0:expected})
                    self.assertEqual(scalar.decode(source,reserve,rows), {0:expected})

    def test_arbitrary_parity_rows_and_erasure_positions(self):
        source = [self.rng.randbytes(64) for _ in range(3)]
        parity = self.codec.encode(source,4)
        for rows in ((0,), (3,), (0,2), (1,2,3)):
            total = len(source)+len(rows)
            for count in range(len(rows)+1):
                for lost in itertools.combinations(range(total),count):
                    original = [None if i in lost else value for i,value in enumerate(source)]
                    recovery = {r:parity[r] for j,r in enumerate(rows) if 3+j not in lost}
                    got = self.codec.decode(original,4,recovery)
                    self.assertEqual(got, {i:source[i] for i in range(3) if i in lost})

    def test_all_sector_sizes_and_partial_last_sector(self):
        for B in archive.SECTORS:
            with self.subTest(B=B):
                source, value = self.data(8,B,17,f'source-{B}.bin')
                ecc = self.create(source,name=f'{B}.ecc',B=B)
                damaged = self.damage(source,[0,8],B,name=f'damaged-{B}')
                output = self.root/f'fixed-{B}'
                result = self.cli('repair',ecc,'--images',damaged,'--outputs',output)
                self.assertTrue(result['complete'])
                self.assertEqual(output.read_bytes(),value)

    def test_primary_iso_catalogue_survives_directory_loss(self):
        B=2048
        data=bytearray(self.rng.randbytes(64*B))
        def record(name,extent,length,flags):
            size=(33+len(name)+1)//2*2
            r=bytearray(size)
            r[0]=size
            r[2:6]=extent.to_bytes(4,'little')
            r[6:10]=extent.to_bytes(4,'big')
            r[10:14]=length.to_bytes(4,'little')
            r[14:18]=length.to_bytes(4,'big')
            r[25]=flags
            r[28:30]=(1).to_bytes(2,'little')
            r[30:32]=(1).to_bytes(2,'big')
            r[32]=len(name)
            r[33:33+len(name)]=name
            return r
        root=record(b'\0',20,B,2)
        pvd=bytearray(B)
        pvd[:7]=b'\1CD001\1'
        pvd[156:190]=root
        data[16*B:17*B]=pvd
        data[17*B:17*B+7]=b'\xffCD001\1'
        entries=root+record(b'\1',20,B,2)+record(b'PHOTO.JPG;1',40,1000,0)
        data[20*B:21*B]=bytes(B)
        data[20*B:20*B+len(entries)]=entries
        value=self.rng.randbytes(1000)
        data[40*B:40*B+1000]=value
        source=self.root/'source.iso'
        source.write_bytes(data)
        ecc=self.root/'iso.ecc'
        self.cli('create',source,ecc,'--data-shards','8','--reserve','1','--parity','1')
        damaged=self.damage(source,[0,8,16,24,4,12,20,28])
        destination=self.root/'extracted'
        result=self.cli('extract',ecc,'--images',damaged,'--directory',destination,'--joint-limit','0')
        self.assertFalse(result['complete'])
        self.assertEqual((destination/'PHOTO.JPG').read_bytes(),value)

    def test_multiple_catalogue_groups_and_four_losses_per_group(self):
        source=self.root/'large-sparse.bin'
        with source.open('wb') as out:
            out.truncate(32769*2048)
        ecc=self.root/'base.ecc'
        archive.create_archive([source],ecc,self.codec,[archive.make_family(32769,8,2,[0])],metadata_k=16)
        metadata=[r for r in archive.scan_records(ecc) if r['kind']==archive.META]
        self.assertEqual({r['group'] for r in metadata},{0,1})
        for record in metadata:
            if record['row'] < 4:
                self.corrupt_record(ecc,record)
        self.cli('verify',ecc,'--images',source)

    def test_truncated_tail_and_lost_parity_header(self):
        source,value=self.data(8)
        ecc=self.create(source)
        records=archive.scan_records(ecc)
        self.corrupt_record(ecc,next(r for r in records if r['kind']==archive.PARITY),header=True)
        with open(ecc,'r+b') as out:
            out.truncate(ecc.stat().st_size-100)
        damaged=self.damage(source,[0])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output)
        self.assertEqual(output.read_bytes(),value)

    def test_complete_container_vectors_are_stable(self):
        import importlib.util
        spec=importlib.util.spec_from_file_location('archive_vectors',ROOT/'contrib'/'rs04-archive-vectors.py')
        module=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        expected=json.loads((ROOT/'contrib'/'rs04_archive_vectors.json').read_text())
        self.assertEqual(module.vectors(),expected)

    def test_metadata_survives_four_corrupt_shards(self):
        source,value = self.data(12)
        ecc = self.create(source)
        metadata = [r for r in archive.scan_records(ecc) if r['kind']==archive.META]
        for r in metadata[:4]:
            self.corrupt_record(ecc,r)
        damaged = self.damage(source,[0])
        output = self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output)
        self.assertEqual(output.read_bytes(),value)

    def test_metadata_beyond_budget_is_rejected_before_output(self):
        source,_ = self.data(8)
        ecc = self.create(source)
        for r in [r for r in archive.scan_records(ecc) if r['kind']==archive.META]:
            self.corrupt_record(ecc,r)
        output=self.root/'fixed'
        result=self.cli('repair',ecc,'--images',source,'--outputs',output,code=1)
        self.assertIn('catalogue',result['error'])
        self.assertFalse(output.exists())
        self.assertFalse(Path(str(output)+'.partial').exists())

    def test_default_metadata_survives_full_parity_budget(self):
        source,value=self.data(8)
        ecc=self.create(source)
        records=[r for r in archive.scan_records(ecc) if r['kind']==archive.META]
        self.assertEqual(records[0]['metadata_reserve'],64)
        self.assertEqual(len(records),65)
        for record in records[:64]:
            self.corrupt_record(ecc,record)
        damaged=self.damage(source,[0])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output)
        self.assertEqual(output.read_bytes(),value)

    def test_alternate_metadata_encoding_preserves_catalogue_identity(self):
        source,value=self.data(8)
        families=[archive.make_family(8,4,4,[0,1])]
        weak=self.root/'weak.ecc'
        strong=self.root/'strong.ecc'
        archive.create_archive([source],weak,self.codec,families,metadata_reserve=16)
        archive.create_archive([source],strong,self.codec,families)
        weak_records=archive.scan_records(weak)
        strong_records=archive.scan_records(strong)
        self.assertEqual(weak_records[0]['root'],strong_records[0]['root'])
        self.assertNotEqual(weak_records[0]['metadata_reserve'],strong_records[0]['metadata_reserve'])
        for record in weak_records:
            if record['kind']==archive.META:
                self.corrupt_record(weak,record)
        for record in strong_records:
            if record['kind']==archive.PARITY:
                self.corrupt_record(strong,record)
        damaged=self.damage(source,[0])
        output=self.root/'fixed'
        self.cli('repair',weak,strong,'--images',damaged,'--outputs',output)
        self.assertEqual(output.read_bytes(),value)

    def test_unavailable_extension_catalogue_is_reported(self):
        source,value=self.data(8)
        ecc=self.create(source)
        extra=self.root/'extra.ecc'
        self.cli('extend',extra,'--archives',ecc,'--images',source,'--rows','3')
        for record in archive.scan_records(extra):
            if record['kind']==archive.META:
                self.corrupt_record(extra,record)
        damaged=self.damage(source,[0])
        output=self.root/'fixed'
        result=self.cli('repair',ecc,extra,'--images',damaged,'--outputs',output)
        self.assertEqual(len(result['unavailable_catalogues']),1)
        self.assertEqual(output.read_bytes(),value)

    def test_silent_parity_corruption_and_lost_metadata_header(self):
        source,value=self.data(8)
        ecc=self.create(source)
        records=archive.scan_records(ecc)
        self.corrupt_record(ecc,next(r for r in records if r['kind']==archive.META),header=True)
        self.corrupt_record(ecc,next(r for r in records if r['kind']==archive.PARITY))
        damaged=self.damage(source,[0])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output)
        self.assertEqual(output.read_bytes(),value)

    def test_extension_adds_information_and_resume_uses_it(self):
        source,value=self.data(8)
        family=archive.make_family(8,4,4,[0])
        base=self.create(source,[family])
        extra=self.root/'extra.ecc'
        self.cli('extend',extra,'--archives',base,'--images',source,'--rows','2')
        damaged=self.damage(source,[0,2])
        output=self.root/'fixed'
        result=self.cli('repair',base,'--images',damaged,'--outputs',output,'--joint-limit','0',code=2)
        self.assertEqual(result['missing_or_corrupt_blocks'],2)
        self.assertFalse(output.exists())
        self.cli('repair',base,extra,'--images',damaged,'--outputs',output,'--resume')
        self.assertEqual(output.read_bytes(),value)
        self.assertFalse(Path(str(output)+'.partial.json').exists())
        self.cli('extend',self.root/'duplicate.ecc','--archives',base,extra,'--images',source,'--rows','2',code=1)

    def test_duplicate_volume_does_not_add_equations(self):
        source,_=self.data(8)
        base=self.create(source,[archive.make_family(8,4,4,[0])])
        copy=self.root/'copy.ecc'
        shutil.copyfile(base,copy)
        damaged=self.damage(source,[0,2])
        result=self.cli('repair',base,copy,'--images',damaged,'--outputs',self.root/'fixed',code=2)
        self.assertFalse(result['complete'])

    def test_extension_rejects_different_source(self):
        source,_=self.data(8)
        base=self.create(source,[archive.make_family(8,4,4,[0])])
        wrong=self.damage(source,[0])
        result=self.cli('extend',self.root/'extra','--archives',base,'--images',wrong,'--rows','2',code=1)
        self.assertIn('source differs',result['error'])

    def test_joint_solver_recovers_stalled_families(self):
        source,value=self.data(8)
        families=[archive.make_family(8,4,2,[0,1]),archive.make_family(8,3,1,[0])]
        ecc=self.create(source,families)
        damaged=self.damage(source,range(6))
        first=self.root/'without-joint'
        self.cli('repair',ecc,'--images',damaged,'--outputs',first,'--joint-limit','0',code=2)
        output=self.root/'fixed'
        result=self.cli('repair',ecc,'--images',damaged,'--outputs',output)
        self.assertEqual(result['joint_status'],'full rank')
        self.assertEqual(output.read_bytes(),value)

    def test_local_repair_unlocks_global_family(self):
        source,value=self.data(8)
        families=[archive.make_family(8,8,1,[0]),archive.make_family(8,2,1,[0],layout='grouped')]
        ecc=self.create(source,families)
        damaged=self.damage(source,[0,2])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--joint-limit','0')
        self.assertEqual(output.read_bytes(),value)

    def test_selected_local_repair_does_not_read_whole_image(self):
        source,value=self.data(64)
        families=[archive.make_family(64,8,2,[0]),archive.make_family(64,2,1,[0],layout='grouped')]
        ecc=self.create(source,families)
        damaged=bytearray(value)
        damaged[:2048]=bytes(2048)
        class Counter(io.BytesIO):
            read_bytes=0
            def read(self,size=-1):
                value=super().read(size)
                self.read_bytes+=len(value)
                return value
        with archive.ExitStack() as stack:
            archives=archive.load_archives([ecc],self.codec,stack)
            counted=Counter(damaged)
            view=archive.OverlayView([counted],[len(value)],2048,stack)
            targets=bytearray(64)
            targets[0]=1
            recovery=archive.Recovery(archives,view,self.codec,targets=targets)
            while recovery.ordinary():
                pass
            self.assertTrue(recovery.known[0])
            self.assertLessEqual(counted.read_bytes,3*2048)
            self.assertEqual(view.read(0),value[:2048])
            self.assertEqual(recovery.report()['unexamined_blocks'],62)

    def test_joint_limit_applies_to_components_not_entire_image(self):
        source,value=self.data(16)
        families=[]
        for start in (0,8):
            families.extend([archive.make_family(16,4,2,[0,1],[[start,8]]),
                             archive.make_family(16,3,1,[0],[[start,8]])])
        ecc=self.create(source,families)
        damaged=self.damage(source,list(range(6))+list(range(8,14)))
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--joint-limit','6')
        self.assertEqual(output.read_bytes(),value)

    def test_unknown_optional_record_is_ignored(self):
        source,_=self.data(4)
        ecc=self.create(source)
        first=archive.scan_records(ecc)[0]
        with open(ecc,'ab') as out:
            archive.write_record(out,0x8001,first['B'],first['root'],0,0,0,first['meta_length'],b'future optional metadata')
        self.cli('verify',ecc,'--images',source)

    def test_selected_extent_protection(self):
        source,value=self.data(8)
        ecc=self.root/'selected.ecc'
        self.cli('create',source,ecc,'--data-shards','8','--reserve','1','--parity','1','--protect','0:1')
        damaged=self.damage(source,[0,1])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--joint-limit','0')
        self.assertEqual(output.read_bytes(),value)

    def test_donor_blocks_do_not_consume_parity(self):
        source,value=self.data(8)
        ecc=self.create(source,[archive.make_family(8,8,1,[0])])
        damaged=self.damage(source,[0,1,2,3])
        donor=self.root/'renamed-fragments.bin'
        donor.write_bytes(value[:3*2048])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--donor',donor)
        self.assertEqual(output.read_bytes(),value)

    def test_shifted_donor_is_found_by_content(self):
        source,value=self.data(2)
        ecc=self.create(source,[archive.make_family(2,2,1,[0])])
        damaged=self.damage(source,[0,1])
        donor=self.root/'shifted'
        donor.write_bytes(b'prefix!'+value[:2048])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--donor',donor,'--search-shifted')
        self.assertEqual(output.read_bytes(),value)

    def test_file_catalogue_and_renamed_extracted_donor(self):
        source,value=self.data(4)
        entry={'path':'photos/example.bin','volume':0,'extents':[[100,4096]],'length':4096,
               'sha256':archive.digest(value[100:4196]).hex()}
        ecc=self.create(source,[archive.make_family(4,4,1,[0])],files=[entry])
        damaged=self.damage(source,[0,1])
        donor=self.root/'renamed-photo'
        donor.write_bytes(value[100:4196])
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--donor',donor)
        self.assertEqual(output.read_bytes(),value)

    def test_extract_intact_file_when_image_is_unrecoverable(self):
        source,value=self.data(4)
        entry={'path':'photos/survives.bin','volume':0,'extents':[[3*2048,2048]],'length':2048,
               'sha256':archive.digest(value[3*2048:]).hex()}
        ecc=self.create(source,[archive.make_family(4,4,1,[0])],files=[entry])
        damaged=self.damage(source,[0,1,2])
        destination=self.root/'extracted'
        result=self.cli('extract',ecc,'--images',damaged,'--directory',destination)
        self.assertFalse(result['complete'])
        self.assertTrue(result['selected_files_verified'])
        self.assertEqual((destination/'photos/survives.bin').read_bytes(),value[3*2048:])

    def test_partial_sector_and_symbol_recovery(self):
        source,value=self.data(2)
        ecc=self.create(source,[archive.make_family(2,2,1,[0])])
        for granularity in (64,2):
            with self.subTest(granularity=granularity):
                bad=bytearray(value)
                if granularity==64:
                    bad[1024:2048]=bytes(1024)
                    bad[2048:3072]=bytes(1024)
                    hints={'0':[[0,1024]],'1':[[1024,1024]]}
                else:
                    bad[0]^=1
                    bad[2049]^=1
                    hints={'0':[[1,2047]],'1':[[0,1],[2,2046]]}
                damaged=self.root/f'partial-{granularity}'
                damaged.write_bytes(bad)
                partial_map=self.root/'hints.json'
                partial_map.write_text(json.dumps(hints))
                output=self.root/f'fixed-{granularity}'
                self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--partial-map',partial_map,
                         '--partial-granularity',granularity,'--joint-limit','0')
                self.assertEqual(output.read_bytes(),value)

    def test_bad_partial_hints_cannot_publish_bad_data(self):
        source,_=self.data(2)
        ecc=self.create(source,[archive.make_family(2,2,1,[0])])
        damaged=self.damage(source,[0,1])
        hints=self.root/'hints.json'
        hints.write_text(json.dumps({'0':[[0,2048]]}))
        output=self.root/'fixed'
        result=self.cli('repair',ecc,'--images',damaged,'--outputs',output,'--partial-map',hints,code=2)
        self.assertFalse(result['complete'])
        self.assertFalse(output.exists())

    def test_collection_recovers_any_two_volume_losses(self):
        sources=[]
        values=[]
        for i,length in enumerate((3,2,1)):
            source,value=self.data(length,tail=11,name=f'volume-{i}')
            sources.append(source)
            values.append(value)
        prefix=self.root/'collection'
        self.cli('collection-create',*sources,'--output-prefix',prefix,'--parity','2')
        parities=[Path(str(prefix)+f'.{i:04d}.ecc') for i in range(2)]
        for losses in itertools.combinations(range(5),2):
            with self.subTest(losses=losses):
                archives=[p for i,p in enumerate(parities,3) if i not in losses]
                images=['-' if i in losses else str(p) for i,p in enumerate(sources)]
                outputs=[self.root/f'fixed-{losses[0]}-{losses[1]}-{i}' for i in range(3)]
                # Losing both parity volumes leaves intact originals: there
                # is no metadata input for this CLI, so no repair is needed.
                if not archives:
                    self.assertTrue(all(s.read_bytes()==v for s,v in zip(sources,values)))
                    continue
                self.cli('repair',*archives,'--images',*images,'--outputs',*outputs)
                self.assertEqual([p.read_bytes() for p in outputs],values)

    def test_augmented_image_retains_source_prefix(self):
        source,value=self.data(8)
        augmented=self.create(source,name='augmented.iso',augment=True)
        self.assertEqual(augmented.read_bytes()[:len(value)],value)
        damaged=self.damage(augmented,[0],name='damaged.iso')
        output=self.root/'fixed'
        self.cli('repair',damaged,'--images',damaged,'--outputs',output)
        self.assertEqual(output.read_bytes(),value)

    def test_regenerate_damaged_ecc(self):
        source,value=self.data(8)
        ecc=self.create(source)
        for record in archive.scan_records(ecc):
            if record['kind']==archive.PARITY:
                self.corrupt_record(ecc,record)
        directory=self.root/'rebuilt'
        output=self.root/'fixed'
        self.cli('repair',ecc,'--images',source,'--outputs',output,'--rebuild-ecc',directory)
        self.assertEqual(output.read_bytes(),value)
        self.cli('verify',directory/'recovery-0000.ecc','--images',source)

    def test_nested_containers_can_be_selected_by_catalogue_identity(self):
        source,_=self.data(4)
        inner=self.create(source,name='inner.iso',augment=True)
        outer=self.root/'outer.iso'
        created=self.cli('create',inner,outer,'--augment','--data-shards','16','--reserve','4','--parity','2')
        listed=self.cli('inspect',outer)
        self.assertTrue(listed['bootstrap_only'])
        self.assertEqual(len(listed['catalogues']),2)
        self.cli('verify',outer,'--images',inner,code=1)
        report=self.cli('verify',outer,'--images',inner,'--catalogue-hash',created['catalogue_hash'])
        self.assertTrue(report['complete'])

    def test_header_mutations_are_bounded_and_do_not_crash(self):
        source,_=self.data(1)
        ecc=self.create(source)
        first=archive.scan_records(ecc)[0]
        with open(ecc,'rb') as original:
            original.seek(first['offset']-first['B'])
            header=original.read(first['B'])
        trial=self.root/'mutated'
        for offset,value in ((16,3),(20,0),(24,0xFFFFFFFF),(124,1)):
            candidate=bytearray(header)
            struct.pack_into('<I',candidate,offset,value)
            candidate[28:32]=bytes(4)
            struct.pack_into('<I',candidate,28,zlib.crc32(candidate))
            trial.write_bytes(candidate)
            try:
                archive.scan_records(trial)
            except ValueError:
                pass
        with self.assertRaises(ValueError):
            archive.parse_json(b'{"profile":2,"profile":3}')
        huge=self.root/'too-large.json'
        with huge.open('wb') as out:
            out.truncate(archive.MAX_JSON+1)
        with self.assertRaises(ValueError):
            archive.load_json_file(huge)

    def test_checkpoint_identity_is_checked_before_writing(self):
        source,_=self.data(8)
        ecc=self.create(source)
        output=self.root/'fixed'
        partial=Path(str(output)+'.partial')
        partial.write_bytes(b'valuable unrelated bytes')
        Path(str(output)+'.partial.json').write_text('{}')
        self.cli('repair',ecc,'--images',source,'--outputs',output,'--resume',code=1)
        self.assertEqual(partial.read_bytes(),b'valuable unrelated bytes')

    def test_unsafe_catalogue_paths_and_symlink_directories(self):
        source,value=self.data(1)
        entry={'path':'../escape','volume':0,'extents':[[0,2048]],'length':2048,'sha256':archive.digest(value).hex()}
        with self.assertRaises(ValueError):
            self.create(source,files=[entry])
        entry['path']='folder/file'
        ecc=self.create(source,files=[entry])
        destination=self.root/'extracted'
        destination.mkdir()
        outside=self.root/'outside'
        outside.mkdir()
        (destination/'folder').symlink_to(outside,target_is_directory=True)
        self.cli('extract',ecc,'--images',source,'--directory',destination,code=1)
        self.assertFalse((outside/'file').exists())

    def test_foreign_archives_and_unknown_required_flags(self):
        source,_=self.data(4)
        ecc=self.create(source)
        other,_=self.data(4,name='other.bin')
        foreign=self.create(other,name='foreign.ecc')
        self.cli('verify',ecc,foreign,'--images',source,code=1)
        first=archive.scan_records(ecc)[0]
        with open(ecc,'r+b') as out:
            out.seek(first['offset']-first['B'])
            header=bytearray(out.read(first['B']))
            struct.pack_into('<I',header,124,1)
            header[28:32]=bytes(4)
            struct.pack_into('<I',header,28,zlib.crc32(header))
            out.seek(first['offset']-first['B'])
            out.write(header)
        self.cli('verify',ecc,'--images',source,code=1)

    def test_offline_kit_uses_scalar_without_source_checkout(self):
        source,_=self.data(4)
        ecc=self.create(source)
        kit=self.root/'kit'
        self.cli('kit',kit)
        completed=subprocess.run([sys.executable,'-E','-s','-S',str(kit/'rs04_archive.py'),'--scalar',
                                  'verify',str(ecc),'--images',str(source)],
                                 cwd=self.root,capture_output=True,text=True)
        self.assertEqual(completed.returncode,0,completed.stderr)
        self.assertTrue(json.loads(completed.stdout)['complete'])

    def test_cli_launcher_forwards_arguments_and_status(self):
        executable=ROOT/'dvdisaster'
        if not executable.exists():
            self.skipTest('application is not built')
        result=subprocess.run([str(executable),'--rs04-archive','--help'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('collection-create',result.stdout)


if __name__=='__main__':
    unittest.main(verbosity=2)
