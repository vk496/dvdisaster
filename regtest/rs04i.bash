#!/usr/bin/env bash

. common.bash

ISOSIZE=8000

PLAINISO=$ISODIR/rs04i-plain.iso
MASTERISO=$ISODIR/rs04i-master.iso
MSISO=$ISODIR/rs04i-multisession.iso
TMPISO=$TMPDIR/rs04i-tmp.iso
TMPECC=$TMPDIR/rs04i-tmp.ecc
SIMISO=$TMPDIR/rs04i-sim.iso
NO_FILE=$TMPDIR/none.file
REFERENCE=../contrib/rs04-reference.py

CODEC_PREFIX=RS04i
init_regtest_cleanup

# Master files are recreated on every run since RS04 is still experimental.
# Master image: 8000 data sectors, 25% redundancy -> 191+47 layers of 42 sectors,
# tag interval 64; sector 8000 = tag 0, 8001-8016 = CRC sectors, 8017- = parity,
# 10022 sectors in total.

rm -f $PLAINISO $MASTERISO $MSISO
$NEWVER --regtest --debug -i$PLAINISO --random-image $ISOSIZE >>$LOGFILE 2>&1
cp $PLAINISO $MASTERISO
$NEWVER --regtest --debug --set-version $SETVERSION -i$MASTERISO -mRS04 -n 25% -c >>$LOGFILE 2>&1

# Two sessions: 5000 sectors + RS04 (sectors 0-5995), 300 sectors gap,
# 3000 sectors + RS04 (sectors 6296-10212)

MSDIR=$TMPDIR/regtest_${CODEC_PREFIX}/_ms
mkdir -p $MSDIR
$NEWVER --regtest --debug -i$MSDIR/s1.iso --random-image 5000 >>$LOGFILE 2>&1
$NEWVER --regtest --debug -i$MSDIR/s2.iso --random-image 3000 --random-seed 7 >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$MSDIR/s1.iso -mRS04 -n 20% -c >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$MSDIR/s2.iso -mRS04 -n 30% -c >>$LOGFILE 2>&1
dd if=$PLAINISO of=$MSDIR/gap.iso bs=2048 skip=100 count=300 >>$LOGFILE 2>&1
cat $MSDIR/s1.iso $MSDIR/gap.iso $MSDIR/s2.iso >$MSISO
rm -rf $MSDIR

# See rs04f.bash

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

### Creation tests

REGTEST_SECTION="Creation tests"

if try "augmenting image" ecc_create; then (
  cp $PLAINISO $TMPISO
  run_regtest ecc_create "--debug --set-version $SETVERSION -mRS04 -n 25% -c" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "augmenting image with 4 threads" ecc_create_threads; then (
  cp $PLAINISO $TMPISO
  run_regtest ecc_create_threads "--debug --set-version $SETVERSION -mRS04 -n 25% -x 4 -c" $TMPISO $NO_FILE
) & limit_jobs; fi

# Fill the smallest medium (CD); redundancy is capped at 200%

if try "augmenting image, fill medium" ecc_create_fill; then (
  cp $PLAINISO $TMPISO
  run_regtest ecc_create_fill "--debug --set-version $SETVERSION -mRS04 -c" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "augmenting image, given medium size" ecc_create_medium_size; then (
  cp $PLAINISO $TMPISO
  run_regtest ecc_create_medium_size "--debug --set-version $SETVERSION -mRS04 -n 12000 -c" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "augmenting image, medium too small" ecc_create_medium_too_small; then (
  cp $PLAINISO $TMPISO
  run_regtest ecc_create_medium_too_small "--debug --set-version $SETVERSION -mRS04 -n 8100 -c" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "augmenting image which is already augmented" ecc_recreate; then (
  cp $MASTERISO $TMPISO
  run_regtest ecc_recreate "--debug --set-version $SETVERSION -mRS04 -n 25% -c" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "augmenting RS03 augmented image" ecc_from_rs03; then (
  cp $PLAINISO $TMPISO
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -mRS03 -n 10000 -c >>$LOGFILE 2>&1
  run_regtest ecc_from_rs03 "--debug --set-version $SETVERSION -mRS04 -n 25% -c" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "augmenting image with incomplete last sector" ecc_non_multiple; then (
  cp $PLAINISO $TMPISO
  dd if="$RNDSEQ" count=1 bs=56 >>$TMPISO 2>/dev/null
  run_regtest ecc_non_multiple "--debug --set-version $SETVERSION -mRS04 -n 25% -c" $TMPISO $NO_FILE
) & limit_jobs; fi

# Tiny images

if try "augmenting 1 sector image" ecc_tiny_1; then (
  head -c 2048 $PLAINISO >$TMPISO
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -mRS04 -c >>$LOGFILE 2>&1
  run_regtest ecc_tiny_1 "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "augmenting 150 sector image" ecc_tiny_150; then (
  $NEWVER --debug -i$TMPISO --random-image 150 >>$LOGFILE 2>&1
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -mRS04 -c >>$LOGFILE 2>&1
  run_regtest ecc_tiny_150 "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

# Bit exactness of the added zone against contrib/rs04-reference.py

if try "bit-exact augmented image" reference_check; then (
  head -c $((301*2048)) $PLAINISO >$TEST_TMPDIR/data.iso
  cp $TEST_TMPDIR/data.iso $TMPISO
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -mRS04 -n 200% -c >>$LOGFILE 2>&1
  tail -c +$((301*2048+1)) $TMPISO >$TEST_TMPDIR/zone.ecc
  reference_check $TEST_TMPDIR/data.iso $TEST_TMPDIR/zone.ecc --ratio 2.0
  test -f $TEST_TMPDIR/zone.ecc || rm -f $TMPISO
  run_regtest reference_check "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

### Verification tests

REGTEST_SECTION="Verify tests"

if try "good image" good; then (
  run_regtest good "-t" $MASTERISO $NO_FILE
) & limit_jobs; fi

if try "good image, quick test" good_quick; then (
  run_regtest good_quick "-tq" $MASTERISO $NO_FILE
) & limit_jobs; fi

if try "image with missing sectors" missing_data_sectors; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 1000-1099 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 7999 >>$LOGFILE 2>&1
  run_regtest missing_data_sectors "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "image with CRC errors" crc_errors; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --byteset 300,1,1 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 5000,2047,0 >>$LOGFILE 2>&1
  run_regtest crc_errors "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

# Tag 0, a CRC sector, tag 1 and some parity sectors lost; CRC error in parity

if try "damaged added zone" zone_damaged; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 8000 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 8005 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 8060-8070 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 9000,0,1 >>$LOGFILE 2>&1
  run_regtest zone_damaged "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "image truncated by 122 sectors" truncated; then (
  dd if=$MASTERISO of=$TMPISO bs=2048 count=9900 >>$LOGFILE 2>&1
  run_regtest truncated "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

# Nothing of the added zone left: no RS04 data recognized

if try "image truncated into the data" truncated_into_data; then (
  dd if=$MASTERISO of=$TMPISO bs=2048 count=7990 >>$LOGFILE 2>&1
  run_regtest truncated_into_data "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "image with trailing garbage" trailing_garbage; then (
  cp $MASTERISO $TMPISO
  dd if=$PLAINISO bs=2048 skip=1000 count=300 >>$TMPISO 2>/dev/null
  run_regtest trailing_garbage "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "missing tag 0" tag0_lost; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 8000 >>$LOGFILE 2>&1
  run_regtest tag0_lost "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

# Neither tag 0 nor the image end can be used: needs a scan of the whole image

if try "missing tag 0 and trailing garbage" tag0_lost_garbage; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 8000 >>$LOGFILE 2>&1
  dd if=$PLAINISO bs=2048 skip=1000 count=300 >>$TMPISO 2>/dev/null
  run_regtest tag0_lost_garbage "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "missing tag 0 and trailing garbage, -a RS04" tag0_lost_garbage_assume; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 8000 >>$LOGFILE 2>&1
  dd if=$PLAINISO bs=2048 skip=1000 count=300 >>$TMPISO 2>/dev/null
  run_regtest tag0_lost_garbage_assume "-a RS04 -t" $TMPISO $NO_FILE
) & limit_jobs; fi

# An RS04 ecc file at the end of an image must not be taken for an augmented image

if try "ecc file at the end of an image" embedded_ecc_file; then (
  $NEWVER --debug -i$TEST_TMPDIR/other.iso --random-image 1000 --random-seed 3 >>$LOGFILE 2>&1
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TEST_TMPDIR/other.iso -e$TEST_TMPDIR/other.ecc -mRS04 -n 50% -o file -c >>$LOGFILE 2>&1
  dd if=$PLAINISO of=$TMPISO bs=2048 count=7000 >>$LOGFILE 2>&1
  cat $TEST_TMPDIR/other.ecc >>$TMPISO
  run_regtest embedded_ecc_file "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

# Multi-session image with two sets

if try "two sessions" multisession; then (
  run_regtest multisession "-t" $MSISO $NO_FILE
) & limit_jobs; fi

if try "two sessions, tag 0 of the first set missing" multisession_gap_scan; then (
  cp $MSISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 5000 >>$LOGFILE 2>&1
  run_regtest multisession_gap_scan "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

### Repair tests

REGTEST_SECTION="Repair tests"

if try "fixing good image" fix_good; then (
  cp $MASTERISO $TMPISO
  run_regtest fix_good "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing missing sectors and CRC errors" fix_correctable; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 1000-1050 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 7999 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 300,1,1 >>$LOGFILE 2>&1
  run_regtest fix_correctable "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing lost CRC sectors" fix_crc_sectors; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 8001-8016 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 2000-2009 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 3000,100,1 >>$LOGFILE 2>&1
  run_regtest fix_crc_sectors "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing damaged added zone" fix_zone_damaged; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 8000 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 8060-8070 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 10021 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --byteset 9000,0,1 >>$LOGFILE 2>&1
  run_regtest fix_zone_damaged "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

# 48 erasures in some codewords, but only 47 ecc layers

if try "fixing partially uncorrectable image" fix_uncorrectable; then (
  cp $MASTERISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 100-2080 >>$LOGFILE 2>&1
  run_regtest fix_uncorrectable "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing truncated image" fix_truncated; then (
  dd if=$MASTERISO of=$TMPISO bs=2048 count=9900 >>$LOGFILE 2>&1
  run_regtest fix_truncated "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing truncated image with 3 threads" fix_truncated_threads; then (
  dd if=$MASTERISO of=$TMPISO bs=2048 count=9900 >>$LOGFILE 2>&1
  run_regtest fix_truncated_threads "-x 3 -f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing image with trailing garbage" fix_trailing_garbage; then (
  cp $MASTERISO $TMPISO
  dd if=$PLAINISO bs=2048 skip=1000 count=300 >>$TMPISO 2>/dev/null
  $NEWVER --debug -i$TMPISO --erase 4000-4010 >>$LOGFILE 2>&1
  run_regtest fix_trailing_garbage "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing 1 sector image" fix_tiny_1; then (
  head -c 2048 $PLAINISO >$TMPISO
  $NEWVER --regtest --debug --set-version $SETVERSION -i$TMPISO -mRS04 -c >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 0 >>$LOGFILE 2>&1
  run_regtest fix_tiny_1 "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing two sessions" fix_multisession; then (
  cp $MSISO $TMPISO
  $NEWVER --debug -i$TMPISO --erase 10-20 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 5000 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 7000-7010 >>$LOGFILE 2>&1
  $NEWVER --debug -i$TMPISO --erase 10212 >>$LOGFILE 2>&1
  run_regtest fix_multisession "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

### Other actions

REGTEST_SECTION="Other tests"

if try "strip RS04 data from augmented image" strip_ecc; then (
  cp $MASTERISO $TMPISO
  run_regtest strip_ecc "-z" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "reading augmented image" read_good; then (
  cp $MASTERISO $SIMISO
  rm -f $TMPISO
  extra_args="--debug --sim-cd=$SIMISO --fixed-speed-values"
  run_regtest read_good "--spinup-delay=0 -r" $TMPISO $NO_FILE
) & limit_jobs; fi

collect_results
exit $nbfailed
