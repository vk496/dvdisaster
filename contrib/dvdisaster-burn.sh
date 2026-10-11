#!/usr/bin/env bash
#  dvdisaster-burn.sh: folder -> ISO image -> augmented with error correction
#  data -> burned -> read back and verified, using xorriso and dvdisaster.
#  Tests: contrib/dvdisaster-burn-test.sh

usage()
{  cat <<EOF
Usage: $(basename "$0") [options] FOLDER

Creates an ISO image of FOLDER with xorriso, augments it with dvdisaster
error correction data and tests the image. With --device it also burns the
image and verifies the disc by reading it back with dvdisaster.

  -d, --device DEV      burn to DEV (e.g. /dev/sr0); nothing is burned without it
  -o, --image FILE      image file to create (default: ./FOLDERNAME.iso)
  -m, --method M        RS04 (default), RS03 or RS02
  -n, --redundancy X    passed to dvdisaster -n: 20%, normal, high, a medium
                        (CD, DVD, DVD9, BD, ...) or a sector count. Default: fill
                        the medium in the drive, or dvdisaster's smallest
                        standard medium without --device; 20% with --multi
  -V, --volid ID        volume id (default: FOLDERNAME)
      --multi           keep the disc appendable, or append a new session to a
                        disc written this way (RS04 only, write-once media and
                        CD-RW/sequential DVD-RW)
      --speed S         burn speed, passed to xorriso
      --force-blank     blank a non-empty rewritable medium (all data on it is lost)
      --dry-run         only print the commands
  -h, --help            show this help

Environment: DVDISASTER and XORRISO name the programs to use.
Exit status: 0 verified, 1 verification failed, 2 usage error, 3 a step failed.
EOF
}

DVDISASTER=${DVDISASTER:-dvdisaster}
XORRISO=${XORRISO:-xorriso}

die()   # exit code, message
{  echo "$(basename "$0"): $2" >&2
   exit "$1"
}

# Print a command, and run it unless in dry-run mode.
# The program variables may hold several words, so they are split here.
run()
{  local prog=$1; shift
   local args; args=$(printf '%q ' "$@")
   echo "+ $prog ${args% }"
   [ -n "$dry_run" ] && return 0
   $prog "$@"
}

step() { echo; echo "==> $*"; }

device= image= method=RS04 redundancy= volid= multi= speed= force_blank= dry_run=

while [ $# -gt 0 ]; do
   case $1 in
      -d|--device|-o|--image|-m|--method|-n|--redundancy|-V|--volid|--speed)
	 [ $# -ge 2 ] || die 2 "option $1 needs an argument"
	 case $1 in
	    -d|--device) device=$2 ;;
	    -o|--image) image=$2 ;;
	    -m|--method) method=$(echo "$2" | tr a-z A-Z) ;;
	    -n|--redundancy) redundancy=$2 ;;
	    -V|--volid) volid=$2 ;;
	    --speed) speed=$2 ;;
	 esac
	 shift 2 ;;
      --multi) multi=1; shift ;;
      --force-blank) force_blank=1; shift ;;
      --dry-run) dry_run=1; shift ;;
      -h|--help) usage; exit 0 ;;
      --) shift; break ;;
      -*) die 2 "unknown option $1 (see --help)" ;;
      *) break ;;
   esac
done

[ $# -eq 1 ] || die 2 "exactly one FOLDER expected (see --help)"
folder=$1
[ -d "$folder" ] || die 2 "$folder is not a directory"
case $method in RS02|RS03|RS04) ;; *) die 2 "unknown method $method (RS02, RS03 or RS04)" ;; esac
if [ -n "$multi" ]; then
   [ "$method" = RS04 ] || die 2 "--multi needs RS04; RS02/RS03 can only protect single-session discs"
   [ -n "$device" ] || die 2 "--multi needs --device"
fi
[ -z "$force_blank" ] || [ -n "$device" ] || die 2 "--force-blank needs --device"

for prog in "$XORRISO" "$DVDISASTER"; do
   set -- $prog
   command -v "$1" >/dev/null || die 3 "$1 not found (it is required)"
done

name=$(basename "$(cd "$folder" && pwd)")
[ -n "$image" ] || image=./$name.iso
[ -n "$volid" ] || volid=${name:0:32}
readback=${image%.iso}-readback.iso
[ -e "$image" ] && die 2 "$image exists; remove it or choose another one with --image"
[ -n "$device" ] && [ -e "$readback" ] && die 2 "$readback exists; remove it first"

# Check the medium: never overwrite data unless explicitly asked to.

msinfo= blank=blank=as_needed
if [ -n "$device" ]; then
   step "Checking the medium in $device"
   if [ -n "$dry_run" ]; then
      run "$XORRISO" -outdev "$device" -tell_media_space
      echo "(dry run: assuming a blank medium; -n would default to its free space)"
      status="is blank" profile= space=
   else
      info=$($XORRISO -outdev "$device" -tell_media_space 2>&1) || { echo "$info"; die 3 "can not query the medium in $device"; }
      profile=$(echo "$info" | sed -n 's/^Media current: //p')
      status=$(echo "$info" | sed -n 's/^Media status : //p')
      space=$(echo "$info" | sed -n 's/^Media space  : \([0-9]*\)s.*/\1/p')
      overall=$(echo "$info" | sed -n 's/^Media blocks : .* \([0-9]*\) overall.*/\1/p')
      echo "Medium: $profile, $status, ${space:-?} free sectors"
   fi

   case $profile in
      DVD+RW*|DVD-RAM*|BD-RE*|*overwrite*) overwritable=1 ;;
      *) overwritable= ;;
   esac
   case $profile in
      *RW*|*RAM*|BD-RE*|*overwrite*) rewritable=1 ;;
      *) rewritable= ;;
   esac

   case $status in
      "is blank") ;;
      *appendable*)
	 if [ -n "$multi" ] && [ -z "$force_blank" ]; then
	    # Overwritable media have only one session; xorriso emulates
	    # further sessions by rewriting the start of the first one,
	    # which would damage the error correction data protecting it.
	    [ -n "$overwritable" ] && die 2 "--multi can not append to $profile media"
	    msinfo=$($XORRISO -as cdrecord dev="$device" -msinfo 2>/dev/null) || die 3 "can not get the session info of $device"
	    blank=
	    echo "Appending a new session at sector ${msinfo#*,}"
	 elif [ -z "$force_blank" ]; then
	    die 2 "medium in $device is not blank ($status); use --multi to append a session or --force-blank to erase it"
	 fi ;;
      *)
	 [ -n "$force_blank" ] || die 2 "medium in $device is not usable ($status)" ;;
   esac
   if [ -n "$force_blank" ] && [ "$status" != "is blank" ]; then
      [ -n "$rewritable" ] || die 2 "--force-blank: $profile can not be blanked"
      space=$overall   # the free space reported does not count what will be erased
   fi

   if [ -z "$redundancy" ]; then
      if [ -n "$multi" ]; then redundancy=20%
      elif [ -n "$space" ]; then redundancy=$space
      fi
   elif [ -n "$overwritable" ]; then
      echo "Note: sectors after the new image keep their old contents on $profile media;"
      echo "      old error correction data there may disturb verifying. Omit -n to fill the medium."
   fi
fi

step "Creating $image from $folder"
iso_opts=(-r -J -joliet-long -iso-level 3 -no-pad -V "$volid")
[ -n "$msinfo" ] && iso_opts+=(-C "$msinfo" -M "$device")
run "$XORRISO" -as mkisofs "${iso_opts[@]}" -o "$image" "$folder" || die 3 "creating the image failed"

step "Augmenting $image with $method error correction data"
aug_opts=(-i "$image" -m"$method")
[ "$method" = RS02 ] || aug_opts+=(-o image)
[ -n "$redundancy" ] && aug_opts+=(-n "$redundancy")
run "$DVDISASTER" "${aug_opts[@]}" -c || die 3 "augmenting the image failed"

if [ -z "$device" ]; then
   step "Testing $image"
   if run "$DVDISASTER" -i "$image" -t; then
      echo; echo "OK: $image is augmented and tested; burn it with --device or any burning program (without padding)."
      exit 0
   fi
   echo; echo "FAILED: $image did not pass the test."
   exit 1
fi

step "Burning $image to $device"
burn_opts=(-v dev="$device" padsize=0)
[ -n "$blank" ] && burn_opts+=("$blank")
[ -n "$multi" ] && burn_opts+=(-multi)
[ -n "$speed" ] && burn_opts+=(speed="$speed")
run "$XORRISO" -as cdrecord "${burn_opts[@]}" "$image" || die 3 "burning failed"

step "Reading the disc back into $readback"
run "$DVDISASTER" -d "$device" -i "$readback" -r; read_rc=$?

step "Verifying $readback"
run "$DVDISASTER" -i "$readback" -t; test_rc=$?

echo
if [ -n "$dry_run" ]; then
   echo "Dry run: nothing was done."
elif [ $read_rc -eq 0 ] && [ $test_rc -eq 0 ]; then
   rm -f "$readback"
   echo "OK: the disc in $device was read back completely and verified."
else
   echo "FAILED: the disc in $device did not verify (read: exit $read_rc, test: exit $test_rc)."
   echo "The read-back image was kept as $readback."
   exit 1
fi
exit 0
