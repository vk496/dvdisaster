#!/usr/bin/env bash

# RS04 workflows from creating the ecc data to repairing what was read
# from a damaged medium, using the simulated drive (--sim-cd, --sim-toc):
# multi-session discs with one RS04 set per session, audio CDs read with
# C2 error pointers and a drive read offset, and CD Extra discs carrying
# the ecc file of their audio session in the data session.

. common.bash

TMPISO=$TMPDIR/rs04r-tmp.iso
TMPECC=$TMPDIR/rs04r-tmp.ecc
SIMISO=$TMPDIR/rs04r-sim.iso
NO_FILE=$TMPDIR/none.file

CODEC_PREFIX=RS04r
init_regtest_cleanup

# Master files are recreated on every run since RS04 is still experimental.

MASTERDIR=$TMPDIR/regtest_${CODEC_PREFIX}/_master
mkdir -p $MASTERDIR

# Multi-session disc: each session image gets its own RS04 set before it
# is written, without knowing where it will be on the disc.
# Session 1: 5000 sectors + RS04 20% (LBA 0 - 5995), 300 sectors between
# the sessions, session 2: 3000 sectors + RS04 30% (LBA 6296 - 10212).
# MSDISC is what reading the disc must give; the gap is stored as zeros.

S1=$MASTERDIR/session1.iso
S2=$MASTERDIR/session2.iso
MSDISC=$MASTERDIR/multisession.iso
MSTOC="--sim-toc=d0-5995/d6296-10212"

$NEWVER --regtest --debug -i$S1 --random-image 5000 >>$LOGFILE 2>&1
$NEWVER --regtest --debug -i$S2 --random-image 3000 --random-seed 7 >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$S1 -mRS04 -n 20% -c >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$S2 -mRS04 -n 30% -c >>$LOGFILE 2>&1
(cat $S1; head -c $((300*2048)) /dev/zero; cat $S2) >$MSDISC

# Single session data disc protected by an ecc file

PLAIN=$MASTERDIR/plain.iso
PLAINECC=$MASTERDIR/plain.ecc

$NEWVER --regtest --debug -i$PLAIN --random-image 5000 --random-seed 11 >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$PLAIN -e$PLAINECC -mRS04 -o file -c >>$LOGFILE 2>&1

# Audio CD: two tracks, 2560 sectors of 2352 bytes

AUDIO=$MASTERDIR/audio.bin
AUDIOECC=$MASTERDIR/audio.ecc
AUDIOTOC="--sim-toc=a0-1279,a1280-2559"
C2DSM=$MASTERDIR/c2.iso    # dead sector marker "c2 error" (simulated C2 errors)

$NEWVER --regtest --debug -i$AUDIO --random-image 2940 >>$LOGFILE 2>&1
$NEWVER --regtest --debug --set-version $SETVERSION -i$AUDIO -e$AUDIOECC --sector-size 2352 -n 20% -mRS04 -o file -c >>$LOGFILE 2>&1
$NEWVER --regtest --debug -i$C2DSM --random-image 1 >>$LOGFILE 2>&1
$NEWVER --debug -i$C2DSM --erase "0:c2 error" >>$LOGFILE 2>&1

# CD Extra: the audio session, 700 sectors between the sessions and a
# data session holding the ecc file of the audio rip (as a plain sequence
# of sectors; a real disc would have a file system around it).
# With audio tracks, the simulated disc stores raw 2352 byte sectors;
# data sectors have their 2048 bytes of user data at offset 16.

CDEXTRA=$MASTERDIR/cdextra.bin
ECCSECTORS=$(( ($(wc -c <$AUDIOECC) + 2047) / 2048 ))
CDXDATA=3260
CDXTOC="--sim-toc=a0-1279,a1280-2559/d$CDXDATA-$((CDXDATA+ECCSECTORS-1))"

cp $AUDIOECC $MASTERDIR/data.iso
head -c $((ECCSECTORS*2048 - $(wc -c <$AUDIOECC))) /dev/zero >>$MASTERDIR/data.iso
(cat $AUDIO; head -c $((700*2352)) /dev/zero
 for ((i=0; i<ECCSECTORS; i++)); do
    head -c 16 /dev/zero
    dd if=$MASTERDIR/data.iso bs=2048 skip=$i count=1 2>/dev/null
    head -c 288 /dev/zero
 done) >$CDEXTRA

### Helper functions

# Preparatory steps of a workflow; their output goes to the log file

function step()
{  echo "$NEWVER --regtest --no-progress $*" >>$LOGFILE
   $NEWVER --regtest --no-progress "$@" >>$LOGFILE 2>&1
}

# read_disc <simulated disc> <toc> <image> [options]

function read_disc()
{  local sim=$1 toc=$2 image=$3
   shift 3
   step -i$image -e$NO_FILE --debug --sim-cd=$sim $toc --fixed-speed-values --spinup-delay=0 -r "$@"
}

# Simulated C2 errors in sectors $2... of the 2352 byte disc image $1

function c2_errors()
{  local disc=$1 s
   shift
   for s in "$@"; do
      dd if=$C2DSM of=$disc bs=2352 seek=$s count=1 conv=notrunc >>$LOGFILE 2>&1
   done
}

# Damage of the multi-session disc used by several tests:
# unreadable sectors in the data and added zone of both sets,
# and a sector which reads fine but is corrupted

function damaged_multisession()
{  cp $MSDISC $1
   $NEWVER --debug -i$1 --erase 100-109 >>$LOGFILE 2>&1
   $NEWVER --debug -i$1 --erase 5200-5210 >>$LOGFILE 2>&1
   $NEWVER --debug -i$1 --erase 7000-7015 >>$LOGFILE 2>&1
   $NEWVER --debug -i$1 --erase 10000 >>$LOGFILE 2>&1
   $NEWVER --debug -i$1 --byteset 300,1,1 >>$LOGFILE 2>&1
}

### Multi-session discs

REGTEST_SECTION="Multi-session discs"

if try "reading the disc" ms_read; then (
  extra_args="--debug --sim-cd=$MSDISC $MSTOC --fixed-speed-values"
  run_regtest ms_read "--spinup-delay=0 -r" $TMPISO $NO_FILE
) & limit_jobs; fi

# The CRC sums cached while reading are complete (from the RS04 data and,
# for parity, tags and gaps, from the image), so the RS01 ecc file created
# in the same run equals one created from the image file.

if try "reading the disc and creating an RS01 ecc file" ms_read_create_rs01; then (
  extra_args="--debug --sim-cd=$MSDISC $MSTOC --fixed-speed-values --set-version $SETVERSION"
  run_regtest ms_read_create_rs01 "--spinup-delay=0 -mRS01 -r -c" $TMPISO $TMPECC
) & limit_jobs; fi

if try "medium info lists both sets" ms_medium_info; then (
  extra_args="--debug --sim-cd=$MSDISC $MSTOC"
  run_regtest ms_medium_info "--medium-info" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "verifying the image read from the disc" ms_verify; then (
  read_disc $MSDISC $MSTOC $TMPISO
  run_regtest ms_verify "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "reading the damaged disc" ms_read_damaged; then (
  damaged_multisession $SIMISO
  extra_args="--debug --sim-cd=$SIMISO $MSTOC --fixed-speed-values"
  run_regtest ms_read_damaged "--spinup-delay=0 -r" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing the image read from the damaged disc" ms_fix; then (
  damaged_multisession $SIMISO
  read_disc $SIMISO $MSTOC $TMPISO
  run_regtest ms_fix "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

# Set 1 has 18 ecc layers of 53 sectors: 1100 lost data sectors are
# beyond repair, but the second set is still repaired.

if try "fixing the disc, one session beyond repair" ms_fix_partial; then (
  cp $MSDISC $SIMISO
  $NEWVER --debug -i$SIMISO --erase 100-1199 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --erase 7000-7015 >>$LOGFILE 2>&1
  read_disc $SIMISO $MSTOC $TMPISO
  run_regtest ms_fix_partial "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "verifying the disc, one session beyond repair" ms_fix_partial_verify; then (
  cp $MSDISC $SIMISO
  $NEWVER --debug -i$SIMISO --erase 100-1199 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --erase 7000-7015 >>$LOGFILE 2>&1
  read_disc $SIMISO $MSTOC $TMPISO
  step -i$TMPISO -e$NO_FILE -f
  run_regtest ms_fix_partial_verify "-t" $TMPISO $NO_FILE
) & limit_jobs; fi

# Sectors which read fine but are corrupted: data sectors of both sets
# and a CRC sector of set 1 (5005: data sectors 2048-2559 lose their CRC)

if try "CRC errors while reading" ms_crc_errors; then (
  cp $MSDISC $SIMISO
  $NEWVER --debug -i$SIMISO --byteset 300,1,1 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --byteset 5005,100,1 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --byteset 7000,2047,1 >>$LOGFILE 2>&1
  extra_args="--debug --sim-cd=$SIMISO $MSTOC --fixed-speed-values"
  run_regtest ms_crc_errors "--spinup-delay=0 -r" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing CRC errors from reading" ms_crc_fix; then (
  cp $MSDISC $SIMISO
  $NEWVER --debug -i$SIMISO --byteset 300,1,1 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --byteset 5005,100,1 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --byteset 7000,2047,1 >>$LOGFILE 2>&1
  read_disc $SIMISO $MSTOC $TMPISO
  run_regtest ms_crc_fix "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

# Tag 0 of set 1 (behind the ISO file system) is unreadable; the set is
# found at the end of session 1, so the CRC error in sector 300 is noticed.

if try "reading the disc, tag 0 of set 1 unreadable" ms_read_tag0_lost; then (
  cp $MSDISC $SIMISO
  $NEWVER --debug -i$SIMISO --erase 5000 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --byteset 300,1,1 >>$LOGFILE 2>&1
  extra_args="--debug --sim-cd=$SIMISO $MSTOC --fixed-speed-values"
  run_regtest ms_read_tag0_lost "--spinup-delay=0 -r" $TMPISO $NO_FILE
) & limit_jobs; fi

# Adaptive reading stops when every codeword of both sets is correctable

if try "adaptive reading of the damaged disc" ms_adaptive; then (
  damaged_multisession $SIMISO
  extra_args="--debug --sim-cd=$SIMISO $MSTOC --fixed-speed-values"
  run_regtest ms_adaptive "--spinup-delay=0 -r --adaptive-read" $TMPISO $NO_FILE
) & limit_jobs; fi

if try "fixing the image from adaptive reading" ms_adaptive_fix; then (
  damaged_multisession $SIMISO
  read_disc $SIMISO $MSTOC $TMPISO --adaptive-read
  run_regtest ms_adaptive_fix "-f" $TMPISO $NO_FILE
) & limit_jobs; fi

# Session 1 ends with 2 unreadable run-out sectors (TAO) which belong
# to no set; they do not keep adaptive reading from finishing.

if try "adaptive reading, unreadable session run-out" ms_adaptive_run_out; then (
  cp $MSDISC $SIMISO
  $NEWVER --debug -i$SIMISO --erase 5996-5997 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --erase 7000-7015 >>$LOGFILE 2>&1
  extra_args="--debug --sim-cd=$SIMISO --sim-toc=d0-5997/d6296-10212 --fixed-speed-values"
  run_regtest ms_adaptive_run_out "--spinup-delay=0 -r --adaptive-read" $TMPISO $NO_FILE
) & limit_jobs; fi

### Data disc with an ecc file

REGTEST_SECTION="Data disc with ecc file"

if try "adaptive reading with ecc file" ecc_file_adaptive; then (
  cp $PLAIN $SIMISO
  $NEWVER --debug -i$SIMISO --erase 1000-1100 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --byteset 3000,0,1 >>$LOGFILE 2>&1
  extra_args="--debug --sim-cd=$SIMISO --fixed-speed-values"
  run_regtest ecc_file_adaptive "--spinup-delay=0 -r --adaptive-read" $TMPISO $PLAINECC
) & limit_jobs; fi

if try "fixing the image from adaptive reading with ecc file" ecc_file_adaptive_fix; then (
  cp $PLAIN $SIMISO
  $NEWVER --debug -i$SIMISO --erase 1000-1100 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --byteset 3000,0,1 >>$LOGFILE 2>&1
  step -i$TMPISO -e$PLAINECC --debug --sim-cd=$SIMISO --fixed-speed-values --spinup-delay=0 -r --adaptive-read
  run_regtest ecc_file_adaptive_fix "-f" $TMPISO $PLAINECC
) & limit_jobs; fi

### Audio CDs

REGTEST_SECTION="Audio CDs"

if try "ripping the audio CD" audio_read; then (
  extra_args="--debug --sim-cd=$AUDIO $AUDIOTOC --fixed-speed-values"
  run_regtest audio_read "--spinup-delay=0 -r" $TEST_TMPDIR/rip.bin $NO_FILE
) & limit_jobs; fi

if try "creating the ecc file for the rip" audio_ecc_create; then (
  read_disc $AUDIO $AUDIOTOC $TEST_TMPDIR/rip.bin
  run_regtest audio_ecc_create "--debug --set-version $SETVERSION --sector-size 2352 -n 20% -mRS04 -o file -c" $TEST_TMPDIR/rip.bin $TEST_TMPDIR/rip.ecc
) & limit_jobs; fi

# The damaged CD is read with a drive which has a read offset of
# +100 samples; C2 errors in sectors 500, 501 and 1500 affect
# two sectors each because of the offset.

if try "ripping the damaged CD with another drive" audio_read_offset; then (
  cp $AUDIO $TEST_TMPDIR/sim.bin
  c2_errors $TEST_TMPDIR/sim.bin 500 501 1500
  extra_args="--debug --sim-cd=$TEST_TMPDIR/sim.bin $AUDIOTOC:offset=100 --fixed-speed-values"
  run_regtest audio_read_offset "--spinup-delay=0 -r -j 0" $TEST_TMPDIR/rip.bin $NO_FILE
) & limit_jobs; fi

if try "fixing the rip, read offset +100 samples" audio_fix_offset; then (
  read_disc $AUDIO $AUDIOTOC $TEST_TMPDIR/rip.bin
  step -i$TEST_TMPDIR/rip.bin -e$TEST_TMPDIR/rip.ecc --debug --set-version $SETVERSION --sector-size 2352 -n 20% -mRS04 -o file -c
  cp $AUDIO $TEST_TMPDIR/sim.bin
  c2_errors $TEST_TMPDIR/sim.bin 500 501 1500
  read_disc $TEST_TMPDIR/sim.bin $AUDIOTOC:offset=100 $TEST_TMPDIR/damaged.bin -j 0
  run_regtest audio_fix_offset "-f" $TEST_TMPDIR/damaged.bin $TEST_TMPDIR/rip.ecc
) & limit_jobs; fi

if try "fixing the rip, read offset -1234 samples" audio_fix_offset_negative; then (
  cp $AUDIO $TEST_TMPDIR/sim.bin
  c2_errors $TEST_TMPDIR/sim.bin 10 2000
  read_disc $TEST_TMPDIR/sim.bin $AUDIOTOC:offset=-1234 $TEST_TMPDIR/damaged.bin -j 0
  run_regtest audio_fix_offset_negative "-f" $TEST_TMPDIR/damaged.bin $AUDIOECC
) & limit_jobs; fi

### CD Extra

REGTEST_SECTION="CD Extra"

if try "ripping the damaged CD Extra" cdextra_read; then (
  cp $CDEXTRA $TEST_TMPDIR/sim.bin
  c2_errors $TEST_TMPDIR/sim.bin 700 2000
  extra_args="--debug --sim-cd=$TEST_TMPDIR/sim.bin $CDXTOC --fixed-speed-values"
  run_regtest cdextra_read "--spinup-delay=0 -r --cdda -j 0" $TEST_TMPDIR/rip.bin $NO_FILE
) & limit_jobs; fi

# The ecc file is taken from the data session of the same disc

if try "verifying the rip with the ecc file from the disc" cdextra_verify; then (
  cp $CDEXTRA $TEST_TMPDIR/sim.bin
  c2_errors $TEST_TMPDIR/sim.bin 700 2000
  read_disc $TEST_TMPDIR/sim.bin $CDXTOC $TEST_TMPDIR/rip.bin --cdda -j 0
  read_disc $TEST_TMPDIR/sim.bin $CDXTOC $TEST_TMPDIR/data.iso
  dd if=$TEST_TMPDIR/data.iso of=$TEST_TMPDIR/disc.ecc bs=2048 skip=$CDXDATA count=$ECCSECTORS >>$LOGFILE 2>&1
  run_regtest cdextra_verify "-t" $TEST_TMPDIR/rip.bin $TEST_TMPDIR/disc.ecc
) & limit_jobs; fi

# The rip also holds the gap and the data session; --truncate leaves the audio part

if try "fixing the audio part of the CD Extra rip" cdextra_fix; then (
  cp $CDEXTRA $TEST_TMPDIR/sim.bin
  c2_errors $TEST_TMPDIR/sim.bin 700 2000
  read_disc $TEST_TMPDIR/sim.bin $CDXTOC $TEST_TMPDIR/rip.bin --cdda -j 0
  read_disc $TEST_TMPDIR/sim.bin $CDXTOC $TEST_TMPDIR/data.iso
  dd if=$TEST_TMPDIR/data.iso of=$TEST_TMPDIR/disc.ecc bs=2048 skip=$CDXDATA count=$ECCSECTORS >>$LOGFILE 2>&1
  run_regtest cdextra_fix "--truncate -f" $TEST_TMPDIR/rip.bin $TEST_TMPDIR/disc.ecc
) & limit_jobs; fi

collect_results
exit $nbfailed
