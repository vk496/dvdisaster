# Using the RS04 archival development CLI

Build the application and optional native codec:

```sh
./configure --with-gui=no
make -j4
make archive-check
./dvdisaster --rs04-archive --help
```

The frontend requires Python 3.8 or later and its standard library. It uses
the native FFT codec when built and has an independent scalar fallback.
`--rs04-archive` must be the first application argument. Global frontend
options, such as `--scalar` and `--memory-mib`, precede the subcommand.

Metadata has its own error correction, with at least 64 parity shards per
group by default. Its reserve grows with the reserved source protection.
Small containers therefore have about 4 MiB of metadata overhead. Global
`--metadata-shards` and `--metadata-reserve` options can change this policy;
lowering the reserve also lowers tolerance to damaged metadata. These coding
parameters are stored in each record header.
`RS04_PYTHON` can select the interpreter used by the native launcher.

## Create and extend recovery data

```sh
./dvdisaster --rs04-archive create original.iso base.ecc \
  --data-shards 1024 --reserve 512 --parity 128

./dvdisaster --rs04-archive extend extra.ecc \
  --archives base.ecc --images original.iso --rows 128:256
```

The reserve is a fixed power-of-two capacity for the family's parity rows.
It must be chosen when creating the family. Row intervals are half-open:
`128:256` writes rows 128 through 255. Repeated rows are physical copies and
do not add correction capacity. Creating extension rows requires the verified
original bytes. The command refuses a changed source image.

An image may itself contain another recovery container. Use `inspect FILE` to
list its bootstrap catalogue identities, then pass `--catalogue-hash HASH` to
`verify`, `repair`, `extract`, or `extend` to select the intended container.
Repeat the selector when combining extension volumes. Inspection reports
bootstrap information only; recovery still verifies the complete catalogue.

For an augmented image, add `--augment` to `create` and give a new output
image path. The exact source bytes remain at the beginning. The v2 reader can
discover records in the augmented file, including after losing its first
metadata records. Existing `-mRS04` workflows continue to use the v1 draft;
repair older media with that reader before migrating verified bytes to v2.

Add `--sector 2352` for raw audio or `--sector 2324` for Form 2 payloads.
Offsets and the image's byte range must already agree with the source used at
creation; the new frontend does not perform drive acquisition or audio offset
search itself.

## Verify and repair

```sh
./dvdisaster --rs04-archive verify base.ecc extra.ecc --images damaged.iso

./dvdisaster --rs04-archive repair base.ecc extra.ecc \
  --images damaged.iso --outputs repaired.iso --rebuild-ecc repaired-ecc
```

Only a fully verified source image is published as `repaired.iso`. Incomplete
work remains in `repaired.iso.partial` and its identity sidecar. Resume with
`--resume`, supplying additional recovery volumes or donor files as available.
Each resume rechecks the content against the catalogue. Existing final outputs
are refused, and the original source is not modified.

`--rebuild-ecc` also regenerates verified recovery containers, including their
metadata and parity. Exit 2 means recovery is incomplete; exit 1 means an
operational/input error. JSON output distinguishes verified files, source
images and regenerated ECC.

## Use surviving files and partial reads

```sh
./dvdisaster --rs04-archive repair base.ecc \
  --images damaged.iso --outputs repaired.iso --donor old-backup-directory
```

Aligned donor blocks are matched by content. Catalogue file hashes also allow
matching renamed extracted files and mapping their bytes back into the image.
Add `--search-shifted` to search every byte offset for displaced blocks; this
is deliberately slower. The donor index uses temporary disk storage.

If raw acquisition retained reliable parts of otherwise damaged sectors,
`--partial-map hints.json` supplies byte-range hints. For example:

```json
{"0": [[0, 1024]], "1": [[1024, 1024]]}
```

This says that the first half of global sector 0 and second half of global
sector 1 are usable candidate data. `--partial-granularity 2` permits recovery
at the stored 16-bit symbol level; the default is 64-byte chunks. These hints
do not bypass the final sector hashes. Hints must describe the supplied image
bytes; they do not ask the drive for new data.

## Recover files after filesystem damage

The writer automatically captures primary ISO 9660 file extents when present.
For another namespace, use `create --catalogue files.json`, where each entry
contains a safe relative path, source volume number and ordered byte extents:

```json
[
  {"path": "photos/holiday.jpg", "volume": 0,
   "extents": [[4194304, 512000]]}
]
```

The writer computes lengths and file hashes from the original image. Use:

```sh
./dvdisaster --rs04-archive extract base.ecc \
  --images damaged.iso --directory rescued --file photos/holiday.jpg
```

The protected catalogue can survive loss of filesystem directories. Extraction
can succeed for selected files even if the complete image cannot be repaired.
Selected extraction reads source sectors on demand and stores repairs in a
sparse temporary overlay; it does not copy or verify the entire source image.
Small local families are attempted first. The protected catalogue must still be
recovered and verified. `images_verified: null` denotes an image not checked in
full, and `unexamined_blocks` distinguishes unread sectors from damaged ones.
Files with unresolved bytes are reported as unrecoverable. Namespace-specific
automatic discovery for Joliet, Rock Ridge and UDF is not implemented; their
byte extents can be supplied explicitly while readable.

## Add protection for selected content and local repairs

```sh
./dvdisaster --rs04-archive create original.iso protected.ecc \
  --protect 0:256 --protect 4096:128 --local-group 32
```

`--protect` intervals are sector start and count, not byte offsets or half-open
row intervals. Together they define an extra family over the selected ranges.
`--local-group` adds one XOR recovery row per small contiguous group. Both
consume additional capacity; they supplement the main family rather than
increasing its MDS guarantee for free.

When independent family decoding stalls, the frontend can combine equations
across the remaining unknown sectors. `--joint-limit 64` is the default;
`--joint-limit 0` disables it. A rank or resource limit is reported explicitly.
The limit applies to each connected component, so many small independent
residual problems can be solved even when their combined unknown count is large.

## Protect against complete volume loss

```sh
./dvdisaster --rs04-archive collection-create disc1.iso disc2.iso disc3.iso \
  --output-prefix collection --parity 2

./dvdisaster --rs04-archive repair collection.0000.ecc collection.0001.ecc \
  --images - disc2.iso - --outputs recovered1.iso recovered2.iso recovered3.iso
```

Each parity row is written to a separate independently described container.
Here any two complete volume losses, including parity volumes, are tolerated
when all remaining required stripes and metadata are intact. Additional damage
on surviving volumes uses the same stripe budget. `-` means a missing input
volume; source order must match creation order. Short images are padded only
virtually. Keep parity containers on independently stored media to obtain the
intended failure separation.

## Export an offline recovery kit

```sh
./dvdisaster --rs04-archive kit recovery-kit
python3 recovery-kit/rs04_archive.py --scalar --help
```

The kit contains the scalar reader, codec, format description, vectors, license, instructions and
a SHA-256 file manifest. It requires no network or third-party Python packages.
The development format remains subject to review; retain the matching reader
with any experimental archives. See the [binary format](codec-specs/rs04-archival.md)
for exact encodings and the conditions still required for an archival freeze.
