/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2017 Carsten Gnoerlich.
 *  Copyright (C) 2019-2026 The dvdisaster development team.
 *
 *  The RS04 erasure codec is a C port of the GF(2^16) part of Leopard-RS:
 *  Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.
 *  (BSD-3-Clause, see license text in rs04-ff16.c)
 *
 *  Email: support@dvdisaster.org
 *
 *  This file is part of dvdisaster.
 *
 *  dvdisaster is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  dvdisaster is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with dvdisaster. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RS04FF16_H
#define RS04FF16_H

#include <glib.h>

/***
 *** RS04 erasure codec over GF(2^16), bit compatible with Leopard-RS v1
 *** (catid/leopard @ 6e5725e, FF16 code path) and with the high-rate
 *** codec of reed-solomon-simd for 64-byte multiple shards.
 ***
 * Parameters (k = original_count, m = recovery_count):
 *   1 <= m <= k,  NextPow2(m) + k <= 65536,
 *   buffer_bytes > 0 and a multiple of 64.
 *   Note: the limit is NOT k + m <= 65536; e.g. k=49152, m=16384 works,
 *   but k=49153, m=16383 does not (NextPow2(16383) = 16384).
 *
 * Buffer format: each buffer is a sequence of 64-byte blocks; block byte i
 * (0..31) is the low byte and byte i+32 the high byte of 16-bit symbol i.
 *
 * Unlike stock leo_encode(), GF(2^16) is used for ALL sizes; stock Leopard1
 * switches to GF(2^8) when NextPow2(NextPow2(m) + k) <= 256 and then produces
 * different parity (except for the k==1 and m==1 special cases).
 *
 * Special cases (identical to Leopard1):
 *   m == 1: the recovery buffer is the XOR of all originals.
 *   k == 1: (implies m == 1) the recovery buffer is a copy of the original.
 *
 * Memory: work[] buffers are provided by the caller (work_count buffers of
 * buffer_bytes each, no alignment required). The decoder additionally
 * allocates ~170KiB of scratch per call. Tables are 512KiB of static memory,
 * built on first use (thread safe). Encode and decode are reentrant.
 */

G_BEGIN_DECLS

#define RS04_SUCCESS          0   /* Operation succeeded */
#define RS04_NEED_MORE_DATA  -1   /* Not enough recovery data received */
#define RS04_TOO_MUCH_DATA   -2   /* Buffer counts are too high */
#define RS04_INVALID_SIZE    -3   /* Buffer size must be a multiple of 64 bytes */
#define RS04_INVALID_COUNTS  -4   /* Invalid counts provided */
#define RS04_INVALID_INPUT   -5   /* A function parameter was invalid */

/* Optional: build the tables now instead of on first use. */
void RS04InitGF16(void);

/* Data kernels; all variants give identical results.
   RS04_KERNEL_AUTO (the default) picks the fastest one supported by the CPU:
   GFNI (AVX2 + GF2P8AFFINEQB), AVX2 or SSSE3 (x86, runtime detection),
   NEON (aarch64), else scalar.
   RS04SelectKernel() returns FALSE if the variant is not available here;
   call it before starting encoder/decoder threads. */
#define RS04_KERNEL_AUTO     0
#define RS04_KERNEL_SCALAR   1
#define RS04_KERNEL_SSSE3    2
#define RS04_KERNEL_AVX2     3
#define RS04_KERNEL_NEON     4
#define RS04_KERNEL_GFNI     5

int RS04SelectKernel(int kernel);
const char *RS04KernelName(void);

/* Number of work[] buffers needed by RS04Encode() / RS04Decode(). */
unsigned RS04EncodeWorkCount(unsigned original_count, unsigned recovery_count);
unsigned RS04DecodeWorkCount(unsigned original_count, unsigned recovery_count);

/* Like leo_encode(): recovery data is returned in work[0..recovery_count-1].
   work[recovery_count..work_count-1] are clobbered. */
int RS04Encode(guint64 buffer_bytes, unsigned original_count, unsigned recovery_count,
               unsigned work_count, const void* const* original, void** work);

/* Like leo_decode(): original[i] / recovery[i] are NULL when missing.
   Each missing original i is restored into work[i]; the content of
   other work[] buffers is undefined. */
int RS04Decode(guint64 buffer_bytes, unsigned original_count, unsigned recovery_count,
               unsigned work_count, const void* const* original,
               const void* const* recovery, void** work);

G_END_DECLS

#endif /* RS04FF16_H */
