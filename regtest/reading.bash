#!/usr/bin/env bash

# Codec independent reading tests:
# multi-session media and audio CDs, using the simulated drive
# with a session/track layout given by --sim-toc.

. common.bash

MASTERISO=$ISODIR/reading-master.iso    # 3000 sectors of 2048 bytes
AUDIOBIN=$ISODIR/reading-audio.bin      # 2560 sectors of 2352 bytes
C2DSM=$ISODIR/reading-c2-dsm.iso        # sector 0: dead sector marker "c2 error"
TMPISO=$TMPDIR/reading-tmp.iso
TMPECC=$TMPDIR/reading-tmp.ecc
SIMISO=$TMPDIR/reading-sim.iso

MULTISESSION="--sim-toc=d0-999/d1600-2999"
AUDIOCD="--sim-toc=a0-1279,a1280-2559"
CDEXTRA="--sim-toc=a0-1279/d1880-2559"

CODEC_PREFIX=READ
init_regtest_cleanup

# Create master images

if ! file_exists $MASTERISO; then
    echo "$NEWVER --regtest --debug -i$MASTERISO --random-image 3000" >>$LOGFILE
    $NEWVER --regtest --debug -i$MASTERISO --random-image 3000 >>$LOGFILE 2>&1
    echo -e "$FILE_MSG"
    FILE_MSG=""
fi

# 2940 sectors of 2048 bytes are 2560 sectors of 2352 bytes

if ! file_exists $AUDIOBIN; then
    echo "$NEWVER --regtest --debug -i$AUDIOBIN --random-image 2940" >>$LOGFILE
    $NEWVER --regtest --debug -i$AUDIOBIN --random-image 2940 >>$LOGFILE 2>&1
    echo -e "$FILE_MSG"
    FILE_MSG=""
fi

if ! file_exists $C2DSM; then
    $NEWVER --regtest --debug -i$C2DSM --random-image 1 >>$LOGFILE 2>&1
    $NEWVER --debug -i$C2DSM --erase "0:c2 error" >>$LOGFILE 2>&1
    echo -e "$FILE_MSG"
    FILE_MSG=""
fi

# Put a dead sector marker (2048 bytes) into sector $3 of the 2352 byte image $2

function put_dsm_2352()
{  dd if=$1 of=$2 bs=2352 seek=$3 count=1 conv=notrunc >>$LOGFILE 2>&1
}

### Multi-session tests

REGTEST_SECTION="Multi-session reading tests"

# Two sessions; the 600 sectors between them are stored as zeros

if try "reading multi-session medium" multisession_read; then (
  extra_args="--debug --sim-cd=$MASTERISO $MULTISESSION --fixed-speed-values"
  run_regtest multisession_read "--spinup-delay=0 -r" $TMPISO $ISODIR/no.ecc
) & limit_jobs; fi

if try "scanning multi-session medium" multisession_scan; then (
  extra_args="--debug --sim-cd=$MASTERISO $MULTISESSION --fixed-speed-values"
  run_regtest multisession_scan "--spinup-delay=0 -s" $ISODIR/no.iso $ISODIR/no.ecc
) & limit_jobs; fi

# Unreadable sectors at the end of the first session (TAO run-out)
# are reported separately; others are normal read errors.

if try "reading defective multi-session medium" multisession_defective; then (
  cp $MASTERISO $SIMISO
  $NEWVER --debug -i$SIMISO --erase 998-999 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --erase 2000 >>$LOGFILE 2>&1

  extra_args="--debug --sim-cd=$SIMISO $MULTISESSION --fixed-speed-values"
  run_regtest multisession_defective "--spinup-delay=0 -r" $TMPISO $ISODIR/no.ecc
) & limit_jobs; fi

# Same with adaptive reading: the TAO run-out is not counted as missing

if try "reading defective multi-session medium, adaptive" multisession_defective_adaptive; then (
  cp $MASTERISO $SIMISO
  $NEWVER --debug -i$SIMISO --erase 998-999 >>$LOGFILE 2>&1
  $NEWVER --debug -i$SIMISO --erase 2000 >>$LOGFILE 2>&1

  extra_args="--debug --sim-cd=$SIMISO $MULTISESSION --fixed-speed-values"
  run_regtest multisession_defective_adaptive "--spinup-delay=0 -r --adaptive-read" $TMPISO $ISODIR/no.ecc
) & limit_jobs; fi

if try "reading multi-session medium, adaptive" multisession_adaptive; then (
  extra_args="--debug --sim-cd=$MASTERISO $MULTISESSION --fixed-speed-values"
  run_regtest multisession_adaptive "--spinup-delay=0 -r --adaptive-read" $TMPISO $ISODIR/no.ecc
) & limit_jobs; fi

# RS02 augmented images describe one session only; still refused

if try "reading multi-session medium with RS02 data" multisession_rs02_refused; then (
  cp $MASTERISO $SIMISO
  $NEWVER --regtest --debug -i$SIMISO -mRS02 -n 3600 -c >>$LOGFILE 2>&1

  extra_args="--debug --sim-cd=$SIMISO --sim-toc=d0-999/d1600-3599 --fixed-speed-values"
  run_regtest multisession_rs02_refused "--spinup-delay=0 -r" $TMPISO $ISODIR/no.ecc
) & limit_jobs; fi

if try "medium info of multi-session medium" multisession_medium_info; then (
  extra_args="--debug --sim-cd=$MASTERISO $MULTISESSION"
  run_regtest multisession_medium_info "--medium-info" $ISODIR/no.iso $ISODIR/no.ecc
) & limit_jobs; fi

### Audio CD tests

REGTEST_SECTION="Audio CD reading tests"

# The .cue sheet is checked via the ecc file md5 slot

if try "reading audio CD" audio_read; then (
  extra_args="--debug --sim-cd=$AUDIOBIN $AUDIOCD --fixed-speed-values"
  run_regtest audio_read "--spinup-delay=0 -r" $TEST_TMPDIR/audio.bin $TEST_TMPDIR/audio.cue
) & limit_jobs; fi

# Sector 500 has C2 errors: unreadable

if try "reading audio CD with C2 errors" audio_c2_error; then (
  cp $AUDIOBIN $TEST_TMPDIR/sim.bin
  put_dsm_2352 $C2DSM $TEST_TMPDIR/sim.bin 500

  extra_args="--debug --sim-cd=$TEST_TMPDIR/sim.bin $AUDIOCD --fixed-speed-values"
  run_regtest audio_c2_error "--spinup-delay=0 -r -j 0" $TEST_TMPDIR/audio.bin $TEST_TMPDIR/audio.cue
) & limit_jobs; fi

# Drive without C2 pointers: sector 500 is silently interpolated

if try "reading audio CD without C2 support" audio_no_c2; then (
  cp $AUDIOBIN $TEST_TMPDIR/sim.bin
  put_dsm_2352 $C2DSM $TEST_TMPDIR/sim.bin 500

  extra_args="--debug --sim-cd=$TEST_TMPDIR/sim.bin $AUDIOCD:noc2 --fixed-speed-values"
  run_regtest audio_no_c2 "--spinup-delay=0 -r" $TEST_TMPDIR/audio.bin $TEST_TMPDIR/audio.cue
) & limit_jobs; fi

# Complete an audio image with a missing sector

if try "completing audio CD image" audio_complete; then (
  cp $AUDIOBIN $TEST_TMPDIR/sim.bin
  put_dsm_2352 $C2DSM $TEST_TMPDIR/sim.bin 500
  $NEWVER --regtest --debug --sim-cd=$TEST_TMPDIR/sim.bin $AUDIOCD --fixed-speed-values --spinup-delay=0 -i $TEST_TMPDIR/audio.bin -r -j 0 >>$LOGFILE 2>&1

  extra_args="--debug --sim-cd=$AUDIOBIN $AUDIOCD --fixed-speed-values"
  run_regtest audio_complete "--spinup-delay=0 -r" $TEST_TMPDIR/audio.bin $TEST_TMPDIR/audio.cue
) & limit_jobs; fi

if try "reading audio CD, adaptive" audio_adaptive_refused; then (
  extra_args="--debug --sim-cd=$AUDIOBIN $AUDIOCD --fixed-speed-values"
  run_regtest audio_adaptive_refused "--spinup-delay=0 -r --adaptive-read" $TEST_TMPDIR/audio.bin $ISODIR/no.ecc
) & limit_jobs; fi

# CD Extra: audio session followed by a data session

if try "reading CD Extra as data image" cdextra_read; then (
  extra_args="--debug --sim-cd=$AUDIOBIN $CDEXTRA --fixed-speed-values"
  run_regtest cdextra_read "--spinup-delay=0 -r" $TMPISO $ISODIR/no.ecc
) & limit_jobs; fi

if try "reading CD Extra as CD-DA image" cdextra_cdda; then (
  extra_args="--debug --sim-cd=$AUDIOBIN $CDEXTRA --fixed-speed-values"
  run_regtest cdextra_cdda "--spinup-delay=0 -r --cdda" $TEST_TMPDIR/cdextra.bin $TEST_TMPDIR/cdextra.cue
) & limit_jobs; fi

if try "medium info of CD Extra" cdextra_medium_info; then (
  extra_args="--debug --sim-cd=$AUDIOBIN $CDEXTRA"
  run_regtest cdextra_medium_info "--medium-info" $ISODIR/no.iso $ISODIR/no.ecc
) & limit_jobs; fi

collect_results
exit $nbfailed
