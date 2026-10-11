#!/usr/bin/env bash

. common.bash

ISOSIZE=8000
RIPSECTORS=2000
RIPBYTES=$((RIPSECTORS*2352))

MASTERISO=$ISODIR/rs04f-master.iso
MASTERECC=$ISODIR/rs04f-master.ecc
MASTERRIP=$ISODIR/rs04f-master.bin
MASTERRIPECC=$ISODIR/rs04f-master-bin.ecc
TMPISO=$TMPDIR/rs04f-tmp.iso
TMPECC=$TMPDIR/rs04f-tmp.ecc
SIMISO=$TMPDIR/rs04f-sim.iso
REFERENCE=../contrib/rs04-reference.py

CODEC_PREFIX=RS04f
init_regtest_cleanup

# Master files are recreated on every run since RS04 is still experimental.
# Master ecc file: 8000 sectors, normal redundancy -> 223+31 layers of 36 sectors,
# tag interval 64; ecc file sector 0 = tag 0, 1-16 = CRC sectors, 17- = parity.

rm -f $MASTERISO $MASTERECC $MASTERRIP $MASTERRIPECC
$NEWVER --regtest --debug -i$MASTERISO --random-image $ISOSIZE >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$MASTERISO -e$MASTERECC -mRS04 -o file -c >>$LOGFILE 2>&1

# CD-DA rip: 2000 sectors of 2352 bytes taken from the random image

dd if=$MASTERISO of=$MASTERRIP bs=2352 skip=100 count=$RIPSECTORS >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$MASTERRIP -e$MASTERRIPECC --sector-size 2352 -n 20% -mRS04 -o file -c >>$LOGFILE 2>&1

# Compare an ecc file with the one computed by the reference encoder.
# On a mismatch the ecc file is removed so that the following verify fails.
# Skipped if python3 is not available.

function reference_check()
{  if command -v python3 >/dev/null 2>&1; then
      if ! python3 $REFERENCE --check "$@" >>$LOGFILE 2>&1; then
	 echo "reference check FAILED" >>$LOGFILE
	 rm -f "$2"
      fi
   else
      echo "python3 not found; reference check skipped" >>$LOGFILE
   fi
}

# Rip of the master with a read offset of $1 samples

function shifted_rip()
{  local bytes=$((4*$1))

   if [ $1 -ge 0 ]; then
      (head -c $bytes /dev/zero; head -c $((RIPBYTES-bytes)) $MASTERRIP) >$2
   else
      bytes=$((-bytes))
      (tail -c $((RIPBYTES-bytes)) $MASTERRIP; head -c $bytes /dev/zero) >$2
   fi
}

### Creation tests

REGTEST_SECTION="Creation tests"

# Create ecc file with default redundancy

if try "ecc file creation" ecc_create; then (
  run_regtest ecc_create "--debug --set-version $SETVERSION -mRS04 -o file -c" $MASTERISO $TMPECC
) & limit_jobs; fi

# Same with 4 threads; must give the same ecc file

if try "ecc file creation with 4 threads" ecc_create_threads; then (
  run_regtest ecc_create_threads "--debug --set-version $SETVERSION -mRS04 -o file -x 4 -c" $MASTERISO $TMPECC
) & limit_jobs; fi

# Portable codec kernels instead of SIMD; must give the same ecc file

if try "ecc file creation with portable codec" ecc_create_portable; then (
  run_regtest ecc_create_portable "--debug --set-version $SETVERSION -mRS04 -o file --encoding-algorithm 32bit -c" $MASTERISO $TMPECC
) & limit_jobs; fi

# Create with explicit redundancies

if try "ecc file creation with 25% redundancy" ecc_create_percent; then (
  run_regtest ecc_create_percent "--debug --set-version $SETVERSION -mRS04 -n 25% -o file -c" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "ecc file creation with 40 roots" ecc_create_roots; then (
  run_regtest ecc_create_roots "--debug --set-version $SETVERSION -mRS04 -n 40r -o file -c" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "ecc file creation with size limit" ecc_create_size; then (
  run_regtest ecc_create_size "--debug --set-version $SETVERSION -mRS04 -n 4m -o file -c" $MASTERISO $TMPECC
) & limit_jobs; fi

# Small memory budget: other code parameters and many encoding passes

if try "ecc file creation with small memory budget" ecc_create_low_memory; then (
  $NEWVER --regtest --debug --set-version $SETVERSION -i$MASTERISO -e$TMPECC -mRS04 -o file --rs04-memory 1 -c >>$LOGFILE 2>&1
  run_regtest ecc_create_low_memory "-t" $MASTERISO $TMPECC
) & limit_jobs; fi

# Invalid parameters

if try "redundancy out of range" ecc_bad_redundancy; then (
  run_regtest ecc_bad_redundancy "--debug --set-version $SETVERSION -mRS04 -n 2% -o file -c" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "invalid redundancy" ecc_invalid_redundancy; then (
  run_regtest ecc_invalid_redundancy "--debug --set-version $SETVERSION -mRS04 -n 200x -o file -c" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "invalid sector size" ecc_invalid_sector_size; then (
  run_regtest ecc_invalid_sector_size "--debug --set-version $SETVERSION -mRS04 --sector-size 2000 -o file -c" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "missing image" ecc_missing_image; then (
  run_regtest ecc_missing_image "--debug --set-version $SETVERSION -mRS04 -o file -c" $ISODIR/no.iso $TMPECC
) & limit_jobs; fi

# Tiny images; the 1 sector image gives the minimal set of the spec

if try "ecc file for 1 sector" ecc_tiny_1; then (
  head -c 2048 $MASTERISO >$TMPISO
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -e$TMPECC -mRS04 -n 50% -o file -c >>$LOGFILE 2>&1
  reference_check $TMPISO $TMPECC --ratio 0.5
  run_regtest ecc_tiny_1 "-t" $TMPISO $TMPECC
) & limit_jobs; fi

if try "ecc file for 10 sectors" ecc_tiny_10; then (
  head -c 20480 $MASTERISO >$TMPISO
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -e$TMPECC -mRS04 -n 50% -o file -c >>$LOGFILE 2>&1
  reference_check $TMPISO $TMPECC --ratio 0.5
  run_regtest ecc_tiny_10 "-t" $TMPISO $TMPECC
) & limit_jobs; fi

# Bit exactness against contrib/rs04-reference.py

if try "bit-exact ecc file, 2048 byte sectors, partial last sector" reference_2048; then (
  head -c $((301*2048+1000)) $MASTERISO >$TMPISO
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -e$TMPECC -mRS04 -n 200% -o file -c >>$LOGFILE 2>&1
  reference_check $TMPISO $TMPECC --ratio 2.0
  run_regtest reference_2048 "-t" $TMPISO $TMPECC
) & limit_jobs; fi

if try "bit-exact ecc file, 2352 byte sectors" reference_2352; then (
  RIP=$TEST_TMPDIR/rs04f-rip.bin
  head -c $((300*2352)) $MASTERRIP >$RIP
  $NEWVER --regtest --debug --set-version $SETVERSION -i$RIP -e$TMPECC --sector-size 2352 -mRS04 -n 200% -o file -c >>$LOGFILE 2>&1
  reference_check $RIP $TMPECC --ratio 2.0
  run_regtest reference_2352 "-t" $RIP $TMPECC
) & limit_jobs; fi

if try "bit-exact ecc file, 2324 byte sectors" reference_2324; then (
  RIP=$TEST_TMPDIR/rs04f-form2.bin
  head -c $((150*2324)) $MASTERRIP >$RIP
  $NEWVER --regtest --debug --set-version $SETVERSION -i$RIP -e$TMPECC --sector-size 2324 -mRS04 -n 200% -o file -c >>$LOGFILE 2>&1
  reference_check $RIP $TMPECC --ratio 2.0
  run_regtest reference_2324 "-t" $RIP $TMPECC
) & limit_jobs; fi

### Verification tests

REGTEST_SECTION="Verify tests"

if try "good image" good; then (
  run_regtest good "-t" $MASTERISO $MASTERECC
) & limit_jobs; fi

if try "good image, quick test" good_quick; then (
  run_regtest good_quick "-tq" $MASTERISO $MASTERECC
) & limit_jobs; fi

if try "missing image" no_image; then (
  run_regtest no_image "-t" $ISODIR/no.iso $MASTERECC
) & limit_jobs; fi

if try "image with missing sectors" missing_data_sectors; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 1000-1099 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 5000 >>$LOGFILE 2>&1
  run_regtest missing_data_sectors "-t" $TMPISO $MASTERECC
) & limit_jobs; fi

if try "image with CRC errors" crc_errors; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --byteset 300,1,1 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 7999,2047,0 >>$LOGFILE 2>&1
  run_regtest crc_errors "-t" $TMPISO $MASTERECC
) & limit_jobs; fi

# Damaged tags, CRC and parity sectors in the ecc file

if try "damaged ecc file" ecc_damaged; then (
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPECC --erase 0 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPECC --erase 5-6 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPECC --erase 60-70 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPECC --byteset 500,0,1 >>$LOGFILE 2>&1
  run_regtest ecc_damaged "-t" $MASTERISO $TMPECC
) & limit_jobs; fi

# Tag 0 is lost: the ecc file is recognized by searching for other tags

if try "missing tag 0" tag0_lost; then (
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPECC --erase 0 >>$LOGFILE 2>&1
  run_regtest tag0_lost "-t" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "invalid tag 0" tag0_invalid; then (
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPECC --byteset 0,40,1 >>$LOGFILE 2>&1
  run_regtest tag0_invalid "-t" $MASTERISO $TMPECC
) & limit_jobs; fi

# All tags claim a newer format version (needs python3 for the checksums)

if try "tags of an unsupported version" tag_newer_version; then (
  command -v python3 >/dev/null 2>&1 || exit 0
  cp $MASTERECC $TMPECC
  python3 - $TMPECC <<'EOF'
import sys, struct, zlib
f = open(sys.argv[1], 'r+b')
for pos in range(0, 1150*2048, 64*2048):
    f.seek(pos); tag = bytearray(f.read(2048))
    tag[16] = 2; tag[28:32] = bytes(4)
    struct.pack_into('<I', tag, 28, zlib.crc32(bytes(tag)) ^ 0xFFFFFFFF)
    f.seek(pos); f.write(tag)
EOF
  run_regtest tag_newer_version "-t" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "image truncated by 10 sectors" image_truncated; then (
  dd if=$MASTERISO of=$TMPISO bs=2048 count=$((ISOSIZE-10)) >>$LOGFILE 2>&1
  run_regtest image_truncated "-t" $TMPISO $MASTERECC
) & limit_jobs; fi

if try "image with 56 extra bytes" image_plus56; then (
  cp $MASTERISO $TMPISO
  dd if="$RNDSEQ" count=1 bs=56 >>$TMPISO 2>/dev/null
  run_regtest image_plus56 "-t" $TMPISO $MASTERECC
) & limit_jobs; fi

if try "ecc file truncated" ecc_truncated; then (
  dd if=$MASTERECC of=$TMPECC bs=2048 count=1000 >>$LOGFILE 2>&1
  run_regtest ecc_truncated "-t" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "ecc file with 1 extra sector" ecc_plus1; then (
  cp $MASTERECC $TMPECC
  dd if=$MASTERISO bs=2048 count=1 >>$TMPECC 2>/dev/null
  run_regtest ecc_plus1 "-t" $MASTERISO $TMPECC
) & limit_jobs; fi

if try "ecc file of another (smaller) image" wrong_image; then (
  $NEWVER --debug -i$TMPISO --random-image 30 --random-seed 5 >>$LOGFILE 2>&1
  run_regtest wrong_image "-t" $TMPISO $MASTERECC
) & limit_jobs; fi

### Repair tests

REGTEST_SECTION="Repair tests"

if try "fixing good image" fix_good; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  run_regtest fix_good "-f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing missing sectors and CRC errors" fix_correctable; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPISO --erase 1000-1050 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 7999 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 300,1,1 >>$LOGFILE 2>&1
  run_regtest fix_correctable "-f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing with 3 threads" fix_correctable_threads; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPISO --erase 1000-1050 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 7999 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 300,1,1 >>$LOGFILE 2>&1
  run_regtest fix_correctable_threads "-x 3 -f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing with portable codec" fix_correctable_portable; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPISO --erase 1000-1050 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 7999 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 300,1,1 >>$LOGFILE 2>&1
  run_regtest fix_correctable_portable "--encoding-algorithm 32bit -f" $TMPISO $TMPECC
) & limit_jobs; fi

# Small memory budget: one codeword per pass

if try "fixing with small memory budget" fix_low_memory; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPISO --erase 1000-1050 >>$LOGFILE 2>&1
  run_regtest fix_low_memory "--rs04-memory 1 -f" $TMPISO $TMPECC
) & limit_jobs; fi

# All CRC sectors lost: their codewords are repaired first

if try "fixing lost CRC sectors" fix_crc_sectors; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPECC --erase 1-16 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 2000-2009 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 3000,100,1 >>$LOGFILE 2>&1
  run_regtest fix_crc_sectors "-f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing damaged ecc file" fix_ecc_damaged; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPECC --erase 0 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPECC --erase 60-70 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPECC --erase 1149 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPECC --byteset 500,0,1 >>$LOGFILE 2>&1
  run_regtest fix_ecc_damaged "-f" $TMPISO $TMPECC
) & limit_jobs; fi

# 32 erasures in every codeword, but only 31 ecc layers

if try "fixing uncorrectable image" fix_uncorrectable; then (
  cp $MASTERISO $TMPISO
  cp $MASTERECC $TMPECC
  $NEWVER --debug -i$TMPISO --erase 100-1251 >>$LOGFILE 2>&1
  run_regtest fix_uncorrectable "-f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing truncated image" fix_truncated_image; then (
  dd if=$MASTERISO of=$TMPISO bs=2048 count=$((ISOSIZE-100)) >>$LOGFILE 2>&1
  cp $MASTERECC $TMPECC
  run_regtest fix_truncated_image "-f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing truncated ecc file" fix_truncated_ecc; then (
  cp $MASTERISO $TMPISO
  dd if=$MASTERECC of=$TMPECC bs=2048 count=1100 >>$LOGFILE 2>&1
  run_regtest fix_truncated_ecc "-f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing image with 56 extra bytes" fix_image_plus56; then (
  cp $MASTERISO $TMPISO
  dd if="$RNDSEQ" count=1 bs=56 >>$TMPISO 2>/dev/null
  cp $MASTERECC $TMPECC
  run_regtest fix_image_plus56 "-f" $TMPISO $TMPECC
) & limit_jobs; fi

if try "fixing image with 56 extra bytes, --truncate" fix_image_plus56_truncate; then (
  cp $MASTERISO $TMPISO
  dd if="$RNDSEQ" count=1 bs=56 >>$TMPISO 2>/dev/null
  cp $MASTERECC $TMPECC
  run_regtest fix_image_plus56_truncate "--truncate -f" $TMPISO $TMPECC
) & limit_jobs; fi

# Data whose last sector is incomplete; the last sector is lost

if try "fixing partial last sector" fix_partial_last_sector; then (
  cp $MASTERISO $TMPISO
  dd if="$RNDSEQ" count=1 bs=56 >>$TMPISO 2>/dev/null
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -e$TMPECC -mRS04 -o file -c >>$LOGFILE 2>&1
  cp $TMPISO $TEST_TMPDIR/good.iso
  dd if=$TEST_TMPDIR/good.iso of=$TMPISO bs=2048 count=7999 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 10 >>$LOGFILE 2>&1
  run_regtest fix_partial_last_sector "-f" $TMPISO $TMPECC
) & limit_jobs; fi

### CD-DA rips

REGTEST_SECTION="Audio tests"

if try "ecc file for CD-DA rip" audio_create; then (
  run_regtest audio_create "--debug --set-version $SETVERSION --sector-size 2352 -n 20% -mRS04 -o file -c" $MASTERRIP $TMPECC
) & limit_jobs; fi

if try "good CD-DA rip" audio_good; then (
  run_regtest audio_good "-t" $MASTERRIP $MASTERRIPECC
) & limit_jobs; fi

if try "rip with read offset +100 samples" audio_offset_verify; then (
  RIP=$TEST_TMPDIR/rs04f-rip.bin
  shifted_rip 100 $RIP
  run_regtest audio_offset_verify "-t" $RIP $MASTERRIPECC
) & limit_jobs; fi

if try "fixing rip with read offset +100 samples" audio_offset_fix; then (
  RIP=$TEST_TMPDIR/rs04f-rip.bin
  shifted_rip 100 $RIP
  run_regtest audio_offset_fix "-f" $RIP $MASTERRIPECC
) & limit_jobs; fi

if try "fixing rip with read offset -300 samples" audio_offset_fix_negative; then (
  RIP=$TEST_TMPDIR/rs04f-rip.bin
  shifted_rip -300 $RIP
  run_regtest audio_offset_fix_negative "-f" $RIP $MASTERRIPECC
) & limit_jobs; fi

if try "fixing rip with read offset and damage" audio_offset_fix_damaged; then (
  RIP=$TEST_TMPDIR/rs04f-rip.bin
  shifted_rip 1000 $RIP
  dd if=/dev/zero of=$RIP bs=2352 seek=500 count=20 conv=notrunc >>$LOGFILE 2>&1
  run_regtest audio_offset_fix_damaged "-f" $RIP $MASTERRIPECC
) & limit_jobs; fi

if try "fixing rip with given read offset" audio_offset_given; then (
  RIP=$TEST_TMPDIR/rs04f-rip.bin
  shifted_rip 100 $RIP
  run_regtest audio_offset_given "--audio-offset 100 -f" $RIP $MASTERRIPECC
) & limit_jobs; fi

if try "fixing rip with wrong given read offset" audio_offset_wrong; then (
  RIP=$TEST_TMPDIR/rs04f-rip.bin
  shifted_rip 100 $RIP
  run_regtest audio_offset_wrong "--audio-offset 0 -f" $RIP $MASTERRIPECC
) & limit_jobs; fi

collect_results
exit $nbfailed
