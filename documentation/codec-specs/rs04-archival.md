# RS04 archival development format

This document specifies the version 2 development container implemented by
`contrib/rs04_archive.py`. The ordinary RS04 method continues to read and write
the earlier version 1 draft. Version 2 is selected explicitly through
`dvdisaster --rs04-archive`. It is experimental and is not an archival format
freeze. The specification, scalar reader and native kernel are shipped together
so that its decoding does not depend on a network service or package registry.

## Supported operations

The CLI creates separate ECC volumes or augmented images, extends immutable
families, verifies and repairs data, rebuilds damaged ECC, extracts verified
catalogue files, imports surviving content from donors, and protects complete
images with separate collection parity volumes. Optional selected-range and
local families, bounded joint solving and partial-sector reconstruction share
the same source fingerprints.

`inspect` lists bootstrap catalogue identities without asserting that their
contents are decodable. An explicit `--catalogue-hash` selector can exclude
embedded or unrelated containers before decoding metadata. This permits an
augmented image to contain an older recovery container as ordinary source data
without combining their different source maps.

Automatic catalogue discovery covers the primary ISO 9660 namespace, including
multiple extents. Joliet, Rock Ridge and UDF names are not interpreted as primary
names. For those namespaces, raw audio and arbitrary images, an explicit file
catalogue can supply byte extents and original names. It must be created while
that information is available. Catalogue extraction never claims that a whole
filesystem has been repaired.

The frontend operates on image files. Optical acquisition, drive offsets,
sessions and existing RS01 through RS04 v1 recovery remain available through the
ordinary application. There is no v2 GUI writer or automatic drive acquisition
adapter in this development implementation. A collection can protect any exact
immutable image bytes, including an already augmented image, but never its own
newly generated collection parity recursively.

## Numbers and limits

All binary integers are unsigned and little-endian. Sector size `B` is 2048,
2324 or 2352. Source volume lengths are positive and below 2^63 bytes; there are
at most 64 source volumes. Pad each final source sector virtually with zero
bytes, independently for each source volume. Padding is not written to a
repaired source file and is excluded from the whole-image digest.

There are at most 64 families per catalogue and 100000 catalogue file entries.
The JSON descriptor is at most 4 MiB. The catalogue length is bounded by 2^40
bytes. A record body is at most 1 MiB; a parity record contains at most 128
sectors. The current reader bounds its inventory to 4194304 records. Decoders
may impose lower operational resource limits but must report those as limits,
not mathematical inability to recover.

## Record framing and discovery

A record consists of one `B`-byte header, a body and zero padding up to a
multiple of `B` bytes. Parity body sectors therefore remain whole sectors.
Separate ECC files start at a record. Augmented images start with the exact
original bytes, zero padding to a `B` boundary, and records. The original image
length comes from the recovered catalogue, not from the container file length.

The first 136 header bytes have this layout:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 16 | Magic `*dvdisaster*RS04` |
| 16 | 2 | Version, exactly 2 |
| 18 | 2 | Record type, 1 for metadata and 2 for parity |
| 20 | 2 | Sector bytes `B` |
| 22 | 2 | Header bytes, exactly `B` |
| 24 | 4 | Body byte length before padding |
| 28 | 4 | zlib CRC32 of the entire header with this field zero |
| 32 | 32 | SHA-256 of the complete catalogue bytes |
| 64 | 8 | Metadata group or first parity codeword column |
| 72 | 4 | Metadata shard index or parity row ID |
| 76 | 4 | 1 for metadata or count of parity body sectors |
| 80 | 8 | Complete unpadded catalogue byte length |
| 88 | 32 | SHA-256 of the unpadded record body |
| 120 | 4 | Family index in the catalogue, 0 for metadata |
| 124 | 4 | Required feature flags, currently 0 |
| 128 | 4 | Metadata originals per codeword `K` |
| 132 | 4 | Metadata parity reserve `R`, a power of two |

All remaining header bytes are zero. SHA-256 outputs are stored in the standard
32-byte digest order. The header CRC uses the standard zlib final inversion,
unlike the earlier dvdisaster CRC convention. The full catalogue digest is the
container identity. Every required record repeats enough bootstrap information
to reconstruct its catalogue independently of a special first or final header.

Discovery scans for magic, validates sector size and bounded header/body
lengths, and checks the header CRC before trusting addressing fields. A valid
record permits skipping to the next record boundary. After an invalid header,
scanning resumes at the following byte so a damaged header does not hide later
records. Version 1 tags are not reinterpreted as version 2 records.

Unknown record types with bit 15 set are optional and may be skipped when
their header bounds, CRC and zero required flags are valid. Other unknown
types, nonzero required flags or nonzero reserved header bytes are unsupported
required semantics. A reader must refuse them. An optional record cannot
change the meaning of source mappings, hash commitments or coding equations.

## Catalogue representation

Catalogue bytes are `u32 descriptorLength`, canonical JSON descriptor bytes,
then an array of 32-byte digests. There is no padding in the committed catalogue.
The descriptor uses ASCII JSON with sorted keys, separators `,` and `:`, ASCII
escaping, no duplicate keys, no NaN or infinity, and no trailing bytes. The
implementation's canonical form is Python `json.dumps` with `sort_keys=True`,
`ensure_ascii=True`, `separators=(',', ':')`, and `allow_nan=False`. Defined
numeric fields are integers. Unsupported descriptor fields are rejected.

The descriptor has exactly these top-level fields:

| Field | Meaning |
| --- | --- |
| `profile` | 2 |
| `sector` | `B` |
| `lengths` | Original byte length of each source volume in order |
| `image_hashes` | SHA-256 of each original byte stream, as lowercase hex |
| `source_hash` | Ordered source identity described below |
| `blocks` | Sum of the padded source sector counts |
| `families` | Array of family descriptors |
| `files` | Optional rescue catalogue entries, or an empty array |

Source block indices concatenate source volumes in order, padding the last
sector of each volume virtually. The digest array first contains `blocks`
source fingerprints. It then contains the parity digests of each family in
descriptor order, row-major in the family's declared `rows` order and then
codeword-column order. All digest offsets count digests, not bytes. No gaps or
unreferenced digest tail are permitted.

A source sector fingerprint is:

```
SHA256("RS04v2 content block\0" || u32(B) || u32(validBytes) || validSourceBytes)
```

`validBytes` is `B` except at an incomplete source-volume tail. Location is not
part of a content fingerprint, so identical bytes can be reused elsewhere.
Parity sector fingerprints are plain SHA-256 of all `B` bytes. Their catalogue
locations bind them to the family, row and column.

The ordered source identity is:

```
SHA256("RS04v2 source\0" || canonicalJSON({"sector":B,"lengths":lengths})
       || concatenatedSourceFingerprints)
```

Its descriptor representation is lowercase hexadecimal. The reader recomputes
this identity after verifying the catalogue. Extension volumes must agree on
sector size, source lengths and every source fingerprint, in addition to the
source identity. A digest stored on the same medium establishes integrity
relative to that digest; publisher authenticity requires an externally trusted
identity. Digital signing and hash-algorithm agility are not part of profile 2.

## Independent metadata protection

Split catalogue bytes into 65536-byte metadata source shards, zero-padding the
last shard and final group. Each group contains `K` originals and `R` parity
shards, using the parameters repeated in every header. They obey the same
field bounds as source families: `K >= 1`, `R >= 1`, `R` a power of two, and
`K + R <= 65536`. Compute parity with the immutable-family equations below,
using `k=K,M=R`. Metadata indices `0..K-1` are originals; `K..K+R-1` are parity
rows `0..R-1`. Every metadata body is exactly 65536 bytes and its count is 1.
Original shards starting beyond the committed catalogue length are known
zeros and need not have physical records. Recovered padding must be zero.

A valid body SHA-256 is required before a metadata shard is used. Recover each
group with at least `K` distinct valid or known-zero shards, concatenate its originals, and
truncate to the catalogue length. The result must match the catalogue SHA-256
from the record headers. Only then parse its descriptor or use source/parity
fingerprints. Conflicting length or sector descriptors for the same catalogue
identity are rejected. Multiple identical physical records add survival copies,
not new equations.

The writer stores metadata shards row-major across groups and intersperses
them across parity records. This separates shards of one codeword. Up to `R`
missing or detected-corrupt physical shards per group are correctable; placement
alone does not guarantee recovery from a particular scratch geometry.

The default writer uses `K=min(64,ceil(catalogue_bytes/65536))`. It sets `R`
to the next power of two at least `max(64,ceil(2*K*max(M/k)))`, where `M/k`
ranges over source families and includes their full reserved capacity. Thus a
full group normally has 64 originals and at least 64 parity shards; a small
catalogue uses one original and 64 parity shards, about 4.06 MiB before headers.
Larger reserves increase this overhead. This budget reduces the chance that a
short metadata codeword fails before a long source codeword under scattered
damage. It is still a separate correction budget, not a whole-container MDS
guarantee. The writer checks the memory budget before coding metadata.

`--metadata-shards` and `--metadata-reserve` override this writer policy.
Metadata coding parameters do not alter the catalogue hash. Readers may try
different encodings of the same committed catalogue, then use parity records
from any encoding after verifying the catalogue. An unavailable catalogue is
reported; other independently decodable catalogues can still supply recovery
equations.

## Immutable recovery families

Each family has exactly `id`, `k`, `reserve`, `layer`, `extents`, `layout`,
`rows`, and `hash_offset`. `reserve=M` is a power of two, at most 32768, and
`1 <= k <= 65536-M`. `M` is fixed for the family's lifetime. `rows` lists
distinct row IDs in `[0,M)`. A volume may carry any nonempty subset. Appending
new rows creates a new container catalogue without changing the family.

`extents` is an ordered array of `[globalSourceSectorStart,count]` intervals.
Intervals are positive, within the complete source map, sorted and disjoint.
Let `X` be their concatenated source sectors, `N` its length, and `L=layer`.
For ordinary layouts, `(k-1)*L < N <= k*L`; missing padding entries of `X`
are known zero sectors. The source vector in column `i` is:

| Layout | Original shard `j` of column `i` |
| --- | --- |
| `layered` | `X[j*L+i]` or virtual zero |
| `grouped` | `X[i*k+j]` or virtual zero |
| `volumes` | Sector `i` of source volume `j`, or virtual zero at its end |

For `volumes`, `k` equals the source-volume count, `L` is the largest padded
volume length in sectors and `extents` is exactly `[[0,blocks]]`. Volume layout
is the collection's outer code. Grouped layout permits small local families.
Selected extents use the same source-sector identities as the global family.

The family ID is:

```
SHA256("RS04v2 family\0" || binarySourceIdentity
       || canonicalJSON({"k":k,"reserve":M,"layer":L,
                         "extents":extents,"layout":layout}))
```

Thus row inventory, physical container and hash-table offset do not affect
equation identity. The same row from another container cannot be counted twice.

### Field and equations

Use GF(2^16) with polynomial `0x1002D`, and the Cantor basis and sector-to-shard
mapping of the existing RS04 draft. The basis, in polynomial representation, is:

```
0001 ACCA 3C0E 163E C582 ED2E 914C 4012
6C98 10D8 6A72 B900 FDB8 FB34 FF38 991E
```

A stored integer's bits select basis elements by XOR. Let `omega_j` be the
field element denoted by stored integer `j`. For every symbol position, the
full mother code satisfies:

```
c_(M+i) = d_i, 0 <= i < k
sum(c_j * omega_j^t, j=0..M+k-1) = 0, 0 <= t < M
```

Recovery row `r` is `c_r`. Omitted rows are punctured symbols. Any `k` distinct
symbols of this mother code determine all originals. With `r` distinct parity
rows stored, up to `r` erasures among those stored parity symbols and the `k`
originals are correctable. The native kernel uses the FFT; the scalar reader
computes the equivalent coefficients directly:

```
P(x) = product(x + omega_a, a=0..M-1)
D = product(omega_a, a=1..M-1)
G[r,i] = P(omega_(M+i)) / (D * (omega_r + omega_(M+i)))
```

Products over an empty set equal one. The characteristic is two, so addition
and subtraction are XOR. Coefficients in the Python reference use polynomial
representation; source and parity symbols use the Cantor representation.

For each full 64-byte block of a shard, bytes 0 through 31 are the low bytes
and bytes 32 through 63 are the high bytes of 32 stored symbols. For a sector
tail of `r` bytes (`r=20` or `48`), put its first `r/2` bytes at positions
0 through `r/2-1` of the final shard block and the rest at positions 32 through
`32+r/2-1`; all unused bytes are zero. The inverse mapping must reject nonzero
unused bytes. A 2048-byte sector is already in shard format. Metadata shards
are multiples of 64 and use the same byte layout directly.

### Coding vector

For `k=5,M=4`, originals `0001 0002 0003 0004 0005` at one stored-symbol
position produce rows `000E 0001 000C 0002`. It is valid to store rows 0 and 2
first and add rows 1 and 3 later. This differs from the earlier construction
with `M=2`, whose parity for those originals is `0004 0005`.

## Reading and recovery

Data is verified only against the independently recovered source fingerprints.
Parity is verified sector by sector against its committed family fingerprints.
A record's body hash is an additional check; corruption of one parity sector
does not invalidate other sectors whose committed fingerprints still match.
No missing header causes readable payload to become implicitly trusted.

First copy the supplied image bytes into separate repair checkpoints and mark
each verified source sector. Reuse hash-matched donor blocks or catalogue file
extents. Decode independently repairable families repeatedly until no progress
remains. Optional partial-sector and joint decoding can then recover more.
Reconstructed sectors must match their source fingerprints before being
accepted. A completed source image must also match its full image digest.

Joint solving combines equations only after substituting verified originals.
Row identities deduplicate copies. Rank deficiency and memory/unknown limits
are distinct from success. The bounded solver partitions the residual graph
using verified equations and applies the default limit of 64 unknowns
independently to each connected component. It also bounds graph memory and
equations attempted per component.

Partial validity maps are external hints, keyed by global source sector index,
with `[byteStart,byteCount]` ranges. The decoder can work on 64-byte chunks or
individual stored 16-bit symbols. Individual symbol pairs span the low and high
halves of each shard block. Hints never establish final integrity: the complete
reconstructed sector must match its committed fingerprint. Incorrect hints may
prevent recovery but must not publish a falsely verified source image.

Donor search defaults to aligned full sectors and exact file-hash matches.
`--search-shifted` tries every byte offset and can be expensive; profile 2 does
not store rolling hashes. Donor indexing uses temporary SQLite storage from
Python's standard library rather than requiring an in-memory index of all
missing blocks.

## Rescue files and output handling

A file catalogue entry has exactly `path`, `volume`, `extents`, `length` and
`sha256`. `volume` is a zero-based source-volume index. Extents are ordered
`[byteOffset,byteLength]` pairs whose sum is `length`; zero-length files have
no extents. Paths are relative POSIX paths without empty, dot, dot-dot,
backslash, NUL or colon components. The file digest covers the concatenated
file extents without virtual padding.

Extraction requires verified sectors covering every selected extent and
verifies the extracted file digest independently. Unsupported or missing
catalogue information is an error. Parent symbolic links and existing output
files are refused. A successful selected-file extraction may coexist with an
unrecoverable whole image; these scopes are reported separately.

Selected extraction may read source sectors on demand through a sparse scratch
overlay, attempting smaller local families before global families. It need not
read or copy unrelated source sectors. Unexamined source sectors must remain
distinct from known erasures. Joint equations may use only examined verified
originals or explicitly modeled unknowns; an unexamined sector is never treated
as a valid equation input. The CLI reports full-image verification as null in
this selective mode.

Repair destinations must differ from inputs. Work is retained in
`OUTPUT.partial`, with a companion `OUTPUT.partial.json` committing source
identity, volume index and expected length. Resume requires matching identity
and rehashes the checkpoint; it never treats zero-filled holes as good by
default. The final output name is created exclusively only after all source
sectors and the complete image digest verify. Files are flushed and fsynced
before publication. Existing output files are not replaced. A collection may
leave already published, verified outputs if publication of a later volume
fails; publication of multiple filesystem paths is not transactional.

`--rebuild-ecc` regenerates complete independent ECC containers only after the
source images verify. This repairs recovery material as well as user data.

## Exit codes and recovery kit

Exit 0 means the requested operation succeeded; for `extract`, it means the
selected files verified, not necessarily that every source image verified.
Exit 2 means verified recovery is incomplete. Exit 1 means invalid input,
unsupported semantics or an operational error. Exit 130 means interruption;
repair checkpoints remain available. The native frontend returns the same
status as the helper. The helper emits structured JSON results.

The `kit` command exports the Python scalar codec, container reader, this
format specification when installed, usage instructions and a SHA-256 file
manifest. Python 3.8 or later with its standard library is required. No native
library is required for the scalar reader. FFT acceleration is optional and
uses `build/librs04-codec.so` or an explicitly supplied library path.

## Validation and remaining release work

`make archive-check` runs native/reference compatibility and end-to-end damage
tests. The legacy RS04 suites must continue to pass because the original API
and v1 layout are retained. Profile 2 has independent equations and test cases,
but its container reader currently has one implementation. A second independent
container reader, representative disc-scale benchmarks, real defect maps,
cross-platform packaging and a format review remain required before declaring
an archival freeze. No simulated or software-injected loss count establishes a
disc lifetime or a physical scratch guarantee.
