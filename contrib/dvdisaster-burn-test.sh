#!/usr/bin/env bash
#  Tests for dvdisaster-burn.sh which need no burner: xorriso writes to a
#  file ("stdio:" pseudo drive) which dvdisaster reads back as a simulated
#  drive; appending a session is tested with a simulated write-once disc.
#  Usage: contrib/dvdisaster-burn-test.sh [path to dvdisaster binary]

here=$(cd "$(dirname "$0")" && pwd)
burn=$here/dvdisaster-burn.sh
REAL=$(cd "$here/.." && pwd)/dvdisaster
[ $# -ge 1 ] && REAL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$REAL" ] || { echo "dvdisaster binary $REAL not found; build it first"; exit 1; }
command -v xorriso >/dev/null || { echo "xorriso not found"; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp" || exit 1
mkdir -p data/sub data2
head -c 3000000 /dev/urandom >data/big.bin
echo hello >data/sub/small.txt
echo second >data2/new.txt

# dvdisaster wrapper: "-d stdio:FILE" becomes the simulated drive FILE;
# with DAMAGE set, a sector of FILE is overwritten before reading it.
# "-d wo:FILE" has two sessions: 0 to S1-1 and NWA to the end of FILE.
cat >dvdisaster <<'EOF'
#!/usr/bin/env bash
args=()
while [ $# -gt 0 ]; do
   case $1 in
      -d) shift
	  disc=${1#*:}
	  [ -n "$DAMAGE" ] && printf XXXX | dd of="$disc" bs=1 seek=204900 conv=notrunc 2>/dev/null
	  args+=(--debug --sim-cd="$disc" --spinup-delay=0)
	  case $1 in wo:*) args+=(--sim-toc=d0-$((S1-1))/d$NWA-$(($(stat -c %s "$disc")/2048-1))) ;; esac ;;
      *) args+=("$1") ;;
   esac
   shift
done
exec "$REAL" --no-progress "${args[@]}"
EOF

# xorriso wrapper: "wo:FILE" is an appendable write-once disc holding
# FILE; its next session starts at NWA. Calls are logged in xorriso.log.
cat >xorriso <<'EOF'
#!/usr/bin/env bash
args=() disc=
for a in "$@"; do
   case $a in wo:*|dev=wo:*) disc=${a#*wo:}; a=${a%%wo:*}$disc ;; esac
   args+=("$a")
done
[ -n "$disc" ] || exec xorriso "$@"
echo "$*" >>xorriso.log
case " $* " in
   *" -tell_media_space "*) echo "Media current: DVD+R"
			    echo "Media status : is written , is appendable"
			    echo "Media space  : 100000s" ;;
   *" -msinfo "*) echo "0,$NWA" ;;
   *" mkisofs "*) exec xorriso "${args[@]}" ;;
   *" cdrecord "*) dd if="${args[-1]}" of="$disc" bs=2048 seek=$NWA conv=notrunc 2>/dev/null ;;
esac
EOF
chmod +x dvdisaster xorriso
export REAL DVDISASTER=$tmp/dvdisaster

failed=0
fail() { echo "FAILED: $*"; failed=$((failed+1)); }

check()   # description, expected exit code, expected output (grep -E), command...
{  local desc=$1 want=$2 pattern=$3; shift 3
   "$@" >out.txt 2>&1; local rc=$?
   if [ $rc -eq "$want" ] && grep -qE -- "$pattern" out.txt; then
      echo "ok:     $desc"
   else
      fail "$desc (exit $rc, expected $want; output:)"
      sed 's/^/    /' out.txt | tail -20
   fi
}

check "help" 0 "Usage:" "$burn" --help
check "no folder" 2 "exactly one FOLDER" "$burn"
check "missing folder" 2 "is not a directory" "$burn" nonexistent
check "unknown option" 2 "unknown option" "$burn" --bogus data
check "missing argument" 2 "needs an argument" "$burn" -n
check "unknown method" 2 "unknown method" "$burn" -m RS05 data
check "--multi needs RS04" 2 "needs RS04" "$burn" --multi -m rs03 -d stdio:x data
check "--multi needs a device" 2 "needs --device" "$burn" --multi data
check "--force-blank needs a device" 2 "needs --device" "$burn" --force-blank data

check "dry run prints but does nothing" 0 "cdrecord -v dev=/dev/sr0 padsize=0 blank=as_needed -multi" \
      "$burn" --dry-run --multi -d /dev/sr0 data
[ -e data.iso ] && fail "dry run created data.iso"

for m in RS02 RS03 RS04; do
   check "image only, $m" 0 "^OK: " "$burn" -m $m -n 2400 -o $m.iso data
   check "image $m is augmented" 0 "good image" "$REAL" -i $m.iso -t
done
check "existing image is not overwritten" 2 "exists" "$burn" -n 2400 -o RS04.iso data

check "burn to an empty pseudo drive and verify" 0 "^OK: the disc" "$burn" -d stdio:"$tmp"/disc.iso data
cmp -s disc.iso data.iso || fail "burned disc differs from the image"
[ -e data-readback.iso ] && fail "read-back image was not removed"

check "non-empty medium is refused" 2 "not blank" "$burn" -n 2400 -o 2.iso -d stdio:"$tmp"/disc.iso data
check "--multi refuses overwritable media" 2 "can not append" "$burn" --multi -o 3.iso -d stdio:"$tmp"/disc.iso data
check "--force-blank overwrites a rewritable medium" 0 "^OK: the disc" \
      "$burn" --force-blank -o 4.iso -d stdio:"$tmp"/disc.iso data

DAMAGE=1 check "damaged disc fails verification" 1 "^FAILED: the disc" "$burn" -n 2400 -o 5.iso -d stdio:"$tmp"/disc5.iso data
[ -e 5-readback.iso ] || fail "read-back image of the damaged disc was not kept"

check "--multi: first session" 0 "^OK: the disc" "$burn" --multi -o ms1.iso -d stdio:"$tmp"/ms.iso data
grep -q "cdrecord.* -multi" out.txt || fail "first session was not burned with -multi"
export S1=$(($(stat -c %s ms.iso)/2048)) NWA=$(($(stat -c %s ms.iso)/2048+100))
XORRISO=$tmp/xorriso check "--multi: second session" 0 "^OK: the disc" "$burn" --multi -o ms2.iso -d wo:"$tmp"/ms.iso data2
grep -q -- "-C 0,$NWA -M wo:$tmp/ms.iso" xorriso.log || fail "second session was not built on the first one"
grep -q "cdrecord.* -multi" xorriso.log && ! grep -q "blank=" xorriso.log || fail "second session was not appended"
grep -q "set              : 2 of 2" out.txt || fail "second session was not verified"

echo
echo "$failed test(s) failed"
[ $failed -eq 0 ]
