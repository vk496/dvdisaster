/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2017 Carsten Gnoerlich.
 *  Copyright (C) 2019-2026 The dvdisaster development team.
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

/*
 *  This file is a C port of the GF(2^16) codec of Leopard-RS
 *  (https://github.com/catid/leopard, commit 6e5725e, 2024-03-23:
 *  leopard.cpp, LeopardCommon.cpp/.h, LeopardFF16.cpp/.h),
 *  distributed under the following license:
 *
 *  Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of Leopard-RS nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/*** src type: no GUI code ***/

#ifdef RS04_STANDALONE
 #include <glib.h>
 #include <string.h>
#else
 #include "dvdisaster.h"
#endif

#include "rs04-ff16.h"

/***
 *** Algorithm overview (see Leopard-RS, LeopardCommon.h for details)
 ***
 * Codewords live in the additive-FFT (LCH) basis over GF(2^16) with the
 * symbol layout  [recovery (M = NextPow2(m))] [original (k)] [zero padding].
 *
 * Encoder:  Recovery = FFT( IFFT(Data_0) xor IFFT(Data_1) xor ... ),
 *           walking the originals M buffers at a time (skew offset grows by M).
 * Decoder:  Original = -ErrLocator * FFT( Derivative( IFFT( ErrLocator * Received ) ) ),
 *           with the error locator evaluated by a mod-65535 Walsh-Hadamard
 *           transform against the precomputed FWHT(Log[]) table.
 *
 * Only the data kernels below (xor_mem, mul_mem and the fft/ifft
 * butterflies) touch buffer bytes. They come in scalar, SSSE3, AVX2 and
 * NEON variants (rs04-ff16-simd.h), all giving identical results.
 */

#define FF_BITS       16
#define FF_ORDER      65536
#define FF_MODULUS    65535
#define FF_POLYNOMIAL 0x1002D

typedef guint16 ffe_t;

/* Basis used for generating logarithm tables */
static const ffe_t cantor_basis[FF_BITS] =
{  0x0001, 0xACCA, 0x3C0E, 0x163E,
   0xC582, 0xED2E, 0x914C, 0x4012,
   0x6C98, 0x10D8, 0x6A72, 0xB900,
   0xFDB8, 0xFB34, 0xFF38, 0x991E
};

static ffe_t log_lut[FF_ORDER];
static ffe_t exp_lut[FF_ORDER];

/* FFT skew factors (logarithms; FF_MODULUS = log(0) means "skip multiply").
   Leopard indexes "FFTSkew - 1", so we keep one pad element in front. */
static ffe_t fft_skew_storage[FF_MODULUS + 1];
#define fft_skew (fft_skew_storage + 1)
#define fft_skew_m1 (fft_skew_storage)

/* Log[1 << i] for build_lut() */
static ffe_t log_bits[FF_BITS];

/* FWHT(Log[i]) for the error locator evaluation */
static ffe_t log_walsh[FF_ORDER];

/***
 *** Field operations
 ***/

/* z = x + y (mod FF_MODULUS), partial reduction: FF_MODULUS may be returned */
static inline ffe_t add_mod(ffe_t a, ffe_t b)
{  unsigned sum = (unsigned)a + b;
   return (ffe_t)(sum + (sum >> FF_BITS));
}

/* z = x - y (mod FF_MODULUS), partial reduction: FF_MODULUS may be returned */
static inline ffe_t sub_mod(ffe_t a, ffe_t b)
{  unsigned dif = (unsigned)a - b;
   return (ffe_t)(dif + (dif >> FF_BITS));
}

/* Returns a * Exp(log_b) */
static ffe_t multiply_log(ffe_t a, ffe_t log_b)
{  if(a == 0) return 0;
   return exp_lut[add_mod(log_lut[a], log_b)];
}

static unsigned next_pow2(unsigned n)
{  unsigned p = 1;
   while(p < n) p <<= 1;
   return p;
}

static unsigned last_nonzero_bit(unsigned n)
{  unsigned i = 0;
   while(n >>= 1) i++;
   return i;
}

/***
 *** Fast Walsh-Hadamard transform (mod FF_MODULUS)
 ***/

static inline void fwht_2(ffe_t *a, ffe_t *b)
{  ffe_t sum = add_mod(*a, *b);
   ffe_t dif = sub_mod(*a, *b);
   *a = sum;
   *b = dif;
}

static inline void fwht_4(ffe_t *data, unsigned s)
{  unsigned s2 = s << 1;
   ffe_t t0 = data[0], t1 = data[s], t2 = data[s2], t3 = data[s2 + s];

   fwht_2(&t0, &t1);
   fwht_2(&t2, &t3);
   fwht_2(&t0, &t2);
   fwht_2(&t1, &t3);

   data[0] = t0; data[s] = t1; data[s2] = t2; data[s2 + s] = t3;
}

/* m_truncated: number of non-zero elements at the front of data */
static void fwht(ffe_t *data, unsigned m, unsigned m_truncated)
{  unsigned dist = 1, dist4 = 4;
   unsigned r, i;

   for(; dist4 <= m; dist = dist4, dist4 <<= 2)
     for(r = 0; r < m_truncated; r += dist4)
       for(i = r; i < r + dist; i++)
	 fwht_4(data + i, dist);

   if(dist < m)
     for(i = 0; i < dist; i++)
       fwht_2(data + i, data + i + dist);
}

/***
 *** Table initialization
 ***/

static void init_log_tables(void)
{  unsigned state = 1, i, j;

   /* LFSR table generation */

   for(i = 0; i < FF_MODULUS; i++)
   {  exp_lut[state] = (ffe_t)i;
      state <<= 1;
      if(state >= FF_ORDER)
	state ^= FF_POLYNOMIAL;
   }
   exp_lut[0] = FF_MODULUS;

   /* Conversion to Cantor basis */

   log_lut[0] = 0;
   for(i = 0; i < FF_BITS; i++)
   {  ffe_t basis = cantor_basis[i];
      unsigned width = 1U << i;

      for(j = 0; j < width; j++)
	log_lut[j + width] = log_lut[j] ^ basis;
   }

   for(i = 0; i < FF_ORDER; i++)
     log_lut[i] = exp_lut[log_lut[i]];

   for(i = 0; i < FF_ORDER; i++)
     exp_lut[log_lut[i]] = (ffe_t)i;

   exp_lut[FF_MODULUS] = exp_lut[0];

   for(i = 0; i < FF_BITS; i++)
     log_bits[i] = log_lut[1U << i];
}

static void init_fft_tables(void)
{  ffe_t temp[FF_BITS - 1];
   unsigned i, j, m;

   /* FFT skew vector */

   for(i = 1; i < FF_BITS; i++)
     temp[i - 1] = (ffe_t)(1U << i);

   for(m = 0; m < FF_BITS - 1; m++)
   {  unsigned step = 1U << (m + 1);

      fft_skew[(1U << m) - 1] = 0;

      for(i = m; i < FF_BITS - 1; i++)
      {  unsigned s = 1U << (i + 1);

	 for(j = (1U << m) - 1; j < s; j += step)
	   fft_skew[j + s] = fft_skew[j] ^ temp[i];
      }

      temp[m] = FF_MODULUS - log_lut[multiply_log(temp[m], log_lut[temp[m] ^ 1])];

      for(i = m + 1; i < FF_BITS - 1; i++)
      {  ffe_t sum = add_mod(log_lut[temp[i] ^ 1], temp[m]);
	 temp[i] = multiply_log(temp[i], sum);
      }
   }

   for(i = 0; i < FF_MODULUS; i++)
     fft_skew[i] = log_lut[fft_skew[i]];

   /* Precalculate FWHT(Log[i]) */

   for(i = 0; i < FF_ORDER; i++)
     log_walsh[i] = log_lut[i];
   log_walsh[0] = 0;

   fwht(log_walsh, FF_ORDER, FF_ORDER);
}

/***
 *** Data kernels.
 *** Each 64-byte block holds 32 symbols: low bytes in [0..31], high bytes in [32..63].
 ***/

/* Product table for multiplying by Exp(log_m), built per butterfly instead
   of Leopard's precomputed 8-16MB tables:
   - lo[16*n + v] / hi[16*n + v] are the low / high bytes of
     (v << 4n) * Exp(log_m), i.e. four 16 byte shuffle tables each
     (Leopard's Multiply128LUT_t), for the scalar and shuffle based kernels;
   - affine[] holds the multiplication as four 8x8 bit matrices for
     GF2P8AFFINEQB (low byte to low, high to low, low to high, high to high),
     only built for the GFNI kernels. */

typedef struct
{  guint8 lo[64], hi[64];
   guint64 affine[4];
} MulLut;

static const MulLut zero_lut;
static gboolean build_affine;   /* set together with the kernels */

/* Transposes an 8x8 bit matrix (bit j of byte i <-> bit i of byte j)
   and reverses the byte order, giving the GF2P8AFFINEQB matrix whose
   output bit i is the parity of (input & byte i of x). */
static guint64 affine_matrix(guint64 x)
{  guint64 t;

   t = (x ^ (x >> 7))  & 0x00aa00aa00aa00aaULL; x ^= t ^ (t << 7);
   t = (x ^ (x >> 14)) & 0x0000cccc0000ccccULL; x ^= t ^ (t << 14);
   t = (x ^ (x >> 28)) & 0x00000000f0f0f0f0ULL; x ^= t ^ (t << 28);

   return GUINT64_SWAP_LE_BE(x);
}

static void build_lut(MulLut *lut, ffe_t log_m)
{  const guint64 splat = 0x0101010101010101ULL;
   ffe_t p[FF_BITS];
   unsigned n;

   /* Multiplication is linear over GF(2): only the products with single
      bits need the log tables, everything else is xor combinations. */

   for(n = 0; n < FF_BITS; n++)
     p[n] = exp_lut[add_mod(log_bits[n], log_m)];

   if(build_affine)
   {  guint64 c[4] = { 0, 0, 0, 0 };

      /* byte j of c[]: low/high byte of the product with bit j of the
	 low/high input byte, i.e. column j of the matrix */

      for(n = 0; n < 8; n++)
      {  c[0] |= (guint64)(p[n] & 0xff) << (8 * n);
	 c[1] |= (guint64)(p[n + 8] & 0xff) << (8 * n);
	 c[2] |= (guint64)(p[n] >> 8) << (8 * n);
	 c[3] |= (guint64)(p[n + 8] >> 8) << (8 * n);
      }
      for(n = 0; n < 4; n++)
	lut->affine[n] = affine_matrix(c[n]);
      return;
   }

   /* The 16 entries of a nibble table are xor combinations of four
      products, computed eight bytes at a time; byte v of the masks
      below is 0xff if bit 0/1/2 of v is set. */

   for(n = 0; n < 4; n++)
   {  const ffe_t *pn = p + 4 * n;
      guint64 lo =   (0xff00ff00ff00ff00ULL & ((pn[0] & 0xff) * splat))
		   ^ (0xffff0000ffff0000ULL & ((pn[1] & 0xff) * splat))
		   ^ (0xffffffff00000000ULL & ((pn[2] & 0xff) * splat));
      guint64 hi =   (0xff00ff00ff00ff00ULL & ((pn[0] >> 8) * splat))
		   ^ (0xffff0000ffff0000ULL & ((pn[1] >> 8) * splat))
		   ^ (0xffffffff00000000ULL & ((pn[2] >> 8) * splat));
      guint64 v;

      v = GUINT64_TO_LE(lo);                          memcpy(lut->lo + 16 * n, &v, 8);
      v = GUINT64_TO_LE(lo ^ ((pn[3] & 0xff) * splat)); memcpy(lut->lo + 16 * n + 8, &v, 8);
      v = GUINT64_TO_LE(hi);                          memcpy(lut->hi + 16 * n, &v, 8);
      v = GUINT64_TO_LE(hi ^ ((pn[3] >> 8) * splat));   memcpy(lut->hi + 16 * n + 8, &v, 8);
   }
}

/* Returns NULL for log_m == FF_MODULUS (butterflies skip the multiply) */
static const MulLut *get_lut(MulLut *lut, ffe_t log_m)
{  if(log_m == FF_MODULUS)
     return NULL;
   build_lut(lut, log_m);
   return lut;
}

/* The data kernels; a NULL lut means "skip the multiply". */

typedef struct
{  const char *name;
   gboolean affine;   /* uses MulLut.affine[] instead of lo[] / hi[] */

   /* x[] ^= y[] */
   void (*xor_mem)(void *x, const void *y, guint64 bytes);

   /* x[] = y[] * lut */
   void (*mul_mem)(void *x, const void *y, const MulLut *lut, guint64 bytes);

   /* IFFT butterfly: y[] ^= x[]; x[] ^= y[] * lut
      FFT butterfly:  x[] ^= y[] * lut; y[] ^= x[] */
   void (*ifft_dit2)(void *x, void *y, const MulLut *lut, guint64 bytes);
   void (*fft_dit2)(void *x, void *y, const MulLut *lut, guint64 bytes);

   /* Two layers of butterflies on work[0], work[dist], work[2*dist], work[3*dist] */
   void (*ifft_dit4)(guint64 bytes, void **work, unsigned dist,
		     const MulLut *lut01, const MulLut *lut23, const MulLut *lut02);
   void (*fft_dit4)(guint64 bytes, void **work, unsigned dist,
		    const MulLut *lut01, const MulLut *lut23, const MulLut *lut02);
} Kernels;

/*
 * Scalar kernels
 */

static void xor_mem_scalar(void *x, const void *y, guint64 bytes)
{  guint8 *x1 = x;
   const guint8 *y1 = y;
   guint64 i;

   for(i = 0; i < bytes; i += 8)
   {  guint64 a, b;
      memcpy(&a, x1 + i, 8);
      memcpy(&b, y1 + i, 8);
      a ^= b;
      memcpy(x1 + i, &a, 8);
   }
}

static void muladd_mem_scalar(void *x, const void *y, const MulLut *l, guint64 bytes)
{  ffe_t lut[64];
   guint8 *x1 = x;
   const guint8 *y1 = y;
   unsigned j;

   for(j = 0; j < 64; j++)
     lut[j] = l->lo[j] | l->hi[j] << 8;

   for(; bytes; bytes -= 64, x1 += 64, y1 += 64)
   {  unsigned i;

      for(i = 0; i < 32; i++)
      {  unsigned lo = y1[i], hi = y1[i + 32];
	 ffe_t prod = lut[lo & 15] ^ lut[(lo >> 4) + 16] ^ lut[(hi & 15) + 32] ^ lut[(hi >> 4) + 48];

	 x1[i]      ^= (guint8)prod;
	 x1[i + 32] ^= (guint8)(prod >> 8);
      }
   }
}

static void mul_mem_scalar(void *x, const void *y, const MulLut *lut, guint64 bytes)
{  memset(x, 0, bytes);
   muladd_mem_scalar(x, y, lut, bytes);
}

static void ifft_dit2_scalar(void *x, void *y, const MulLut *lut, guint64 bytes)
{  xor_mem_scalar(y, x, bytes);
   if(lut)
     muladd_mem_scalar(x, y, lut, bytes);
}

static void fft_dit2_scalar(void *x, void *y, const MulLut *lut, guint64 bytes)
{  if(lut)
     muladd_mem_scalar(x, y, lut, bytes);
   xor_mem_scalar(y, x, bytes);
}

static void ifft_dit4_scalar(guint64 bytes, void **work, unsigned dist,
			     const MulLut *lut01, const MulLut *lut23, const MulLut *lut02)
{  /* First layer */
   ifft_dit2_scalar(work[0], work[dist], lut01, bytes);
   ifft_dit2_scalar(work[dist * 2], work[dist * 3], lut23, bytes);

   /* Second layer */
   ifft_dit2_scalar(work[0], work[dist * 2], lut02, bytes);
   ifft_dit2_scalar(work[dist], work[dist * 3], lut02, bytes);
}

static void fft_dit4_scalar(guint64 bytes, void **work, unsigned dist,
			    const MulLut *lut01, const MulLut *lut23, const MulLut *lut02)
{  /* First layer */
   fft_dit2_scalar(work[0], work[dist * 2], lut02, bytes);
   fft_dit2_scalar(work[dist], work[dist * 3], lut02, bytes);

   /* Second layer */
   fft_dit2_scalar(work[0], work[dist], lut01, bytes);
   fft_dit2_scalar(work[dist * 2], work[dist * 3], lut23, bytes);
}

static const Kernels kernels_scalar =
{  "scalar", FALSE,
   xor_mem_scalar, mul_mem_scalar,
   ifft_dit2_scalar, fft_dit2_scalar,
   ifft_dit4_scalar, fft_dit4_scalar
};

/*
 * SSSE3 and AVX2 kernels, selected at runtime; the rest of the binary
 * stays at the baseline instruction set.
 */

#if defined(HAVE_SSE2) && defined(__GNUC__)
#include <immintrin.h>

#define RS04_HAVE_X86_KERNELS

#define SIMD(name)    name##_ssse3
#define SIMD_NAME     "SSSE3"
#define TARGET        __attribute__((target("ssse3")))
#define V             __m128i
#define VW            16
#define V_LOAD(p)     _mm_loadu_si128((const __m128i*)(p))
#define V_STORE(p, v) _mm_storeu_si128((__m128i*)(p), v)
#define V_XOR(a, b)   _mm_xor_si128(a, b)
#define V_MASK        _mm_set1_epi8(0x0f)
#define V_TABLE(p)    _mm_loadu_si128((const __m128i*)(p))
#define V_SHUF(t, i)  _mm_shuffle_epi8(t, i)
#define V_LONIB(v, m) _mm_and_si128(v, m)
#define V_HINIB(v, m) _mm_and_si128(_mm_srli_epi64(v, 4), m)
#include "rs04-ff16-simd.h"
#undef V
#undef VW
#undef V_LOAD
#undef V_STORE
#undef V_XOR
#undef V_MASK
#undef V_TABLE
#undef V_SHUF
#undef V_LONIB
#undef V_HINIB

#define V             __m256i
#define VW            32
#define V_LOAD(p)     _mm256_loadu_si256((const __m256i*)(p))
#define V_STORE(p, v) _mm256_storeu_si256((__m256i*)(p), v)
#define V_XOR(a, b)   _mm256_xor_si256(a, b)
#define V_MASK        _mm256_set1_epi8(0x0f)
#define V_TABLE(p)    _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)(p)))
#define V_SHUF(t, i)  _mm256_shuffle_epi8(t, i)
#define V_LONIB(v, m) _mm256_and_si256(v, m)
#define V_HINIB(v, m) _mm256_and_si256(_mm256_srli_epi64(v, 4), m)

#define SIMD(name)    name##_avx2
#define SIMD_NAME     "AVX2"
#define TARGET        __attribute__((target("avx2")))
#include "rs04-ff16-simd.h"

/* GF2P8AFFINEQB multiplies 32 bytes by an 8x8 bit matrix at once
   (VEX encoded, no AVX-512 needed) */

#if defined(HAVE_CPUID) && ((defined(__clang__) && __clang_major__ >= 7) || (!defined(__clang__) && __GNUC__ >= 8))
#include <cpuid.h>

#define RS04_HAVE_GFNI_KERNELS

#define SIMD(name)    name##_gfni
#define SIMD_NAME     "GFNI"
#define TARGET        __attribute__((target("avx2,gfni")))
#define V_SPLAT64(x)  _mm256_set1_epi64x((long long)(x))
#define V_AFFINE(v, m) _mm256_gf2p8affine_epi64_epi8(v, m, 0)
#include "rs04-ff16-simd.h"
#undef V_SPLAT64

static gboolean cpu_has_gfni(void)
{  unsigned a, b, c, d;

   if(!__builtin_cpu_supports("avx2") || __get_cpuid_max(0, NULL) < 7)
     return FALSE;
   __cpuid_count(7, 0, a, b, c, d);
   return (c >> 8) & 1;
}
#endif
#endif

/*
 * NEON kernels (always available on aarch64)
 */

#ifdef __aarch64__
#include <arm_neon.h>

#define SIMD(name)    name##_neon
#define SIMD_NAME     "NEON"
#define TARGET
#define V             uint8x16_t
#define VW            16
#define V_LOAD(p)     vld1q_u8((const guint8*)(p))
#define V_STORE(p, v) vst1q_u8((guint8*)(p), v)
#define V_XOR(a, b)   veorq_u8(a, b)
#define V_MASK        vdupq_n_u8(0x0f)
#define V_TABLE(p)    vld1q_u8(p)
#define V_SHUF(t, i)  vqtbl1q_u8(t, i)
#define V_LONIB(v, m) vandq_u8(v, m)
#define V_HINIB(v, m) vshrq_n_u8(v, 4)
#include "rs04-ff16-simd.h"
#endif

/*
 * Kernel selection
 */

static const Kernels *kern;

int RS04SelectKernel(int kernel)
{  const Kernels *k = NULL;

   switch(kernel)
   {  case RS04_KERNEL_AUTO:
	return    RS04SelectKernel(RS04_KERNEL_GFNI)
	       || RS04SelectKernel(RS04_KERNEL_AVX2)
	       || RS04SelectKernel(RS04_KERNEL_SSSE3)
	       || RS04SelectKernel(RS04_KERNEL_NEON)
	       || RS04SelectKernel(RS04_KERNEL_SCALAR);
      case RS04_KERNEL_SCALAR:
	k = &kernels_scalar;
	break;
#ifdef RS04_HAVE_X86_KERNELS
      case RS04_KERNEL_SSSE3:
	if(__builtin_cpu_supports("ssse3"))
	  k = &kernels_ssse3;
	break;
      case RS04_KERNEL_AVX2:
	if(__builtin_cpu_supports("avx2"))
	  k = &kernels_avx2;
	break;
#endif
#ifdef RS04_HAVE_GFNI_KERNELS
      case RS04_KERNEL_GFNI:
	if(cpu_has_gfni())
	  k = &kernels_gfni;
	break;
#endif
#ifdef __aarch64__
      case RS04_KERNEL_NEON:
	k = &kernels_neon;
	break;
#endif
   }

   if(!k)
     return FALSE;

   kern = k;
   build_affine = k->affine;
   return TRUE;
}

const char *RS04KernelName(void)
{  RS04InitGF16();
   return kern->name;
}

void RS04InitGF16(void)
{  static gsize initialized = 0;

   if(g_once_init_enter(&initialized))
   {  init_log_tables();
      init_fft_tables();
      if(!kern)
	RS04SelectKernel(RS04_KERNEL_AUTO);
      g_once_init_leave(&initialized, 1);
   }
}

/* x[] = y[] * Exp(log_m); log_m == FF_MODULUS multiplies by 1 */
static void mul_mem(void *x, const void *y, ffe_t log_m, guint64 bytes)
{  MulLut lut;

   build_lut(&lut, log_m);
   kern->mul_mem(x, y, &lut, bytes);
}

/***
 *** Transforms
 ***/

/* In-place decimation-in-time IFFT of m buffers, of which only the
   first m_truncated are non-zero. skew points to Leopard's "skewLUT". */
static void ifft_dit(guint64 bytes, void **work, unsigned m_truncated, unsigned m,
		     const ffe_t *skew)
{  const Kernels *k = kern;
   MulLut l01, l23, l02;
   unsigned dist = 1, dist4 = 4;
   unsigned r, i;

   for(; dist4 <= m; dist = dist4, dist4 <<= 2)
   {  for(r = 0; r < m_truncated; r += dist4)
      {  unsigned i_end = r + dist;
	 const MulLut *lut01 = get_lut(&l01, skew[i_end]);
	 const MulLut *lut02 = get_lut(&l02, skew[i_end + dist]);
	 const MulLut *lut23 = get_lut(&l23, skew[i_end + dist * 2]);

	 for(i = r; i < i_end; i++)
	   k->ifft_dit4(bytes, work + i, dist, lut01, lut23, lut02);
      }
   }

   /* If there is one layer left (dist == m/2) */
   if(dist < m)
   {  const MulLut *lut = get_lut(&l01, skew[dist]);

      for(i = 0; i < dist; i++)
	k->ifft_dit2(work[i], work[i + dist], lut, bytes);
   }
}

/***
 *** Error bitfield: decides which final FFT operations are needed
 *** to reveal the erased originals (decoder speed-up only).
 ***/

#define EB_WORD_MIPS    5
#define EB_WORDS        (FF_ORDER / 64)
#define EB_BIG_MIPS     6
#define EB_BIG_WORDS    ((EB_WORDS + 63) / 64)
#define EB_BIGGEST_MIPS 4

typedef struct
{  guint64 words[EB_WORD_MIPS][EB_WORDS];
   guint64 bigWords[EB_BIG_MIPS][EB_BIG_WORDS];
   guint64 biggestWords[EB_BIGGEST_MIPS];
} ErrorBitfield;

static const guint64 hi_masks[5] =
{  0xAAAAAAAAAAAAAAAAULL,
   0xCCCCCCCCCCCCCCCCULL,
   0xF0F0F0F0F0F0F0F0ULL,
   0xFF00FF00FF00FF00ULL,
   0xFFFF0000FFFF0000ULL,
};

static void eb_set(ErrorBitfield *eb, unsigned i)
{  eb->words[0][i / 64] |= (guint64)1 << (i % 64);
}

static void eb_prepare(ErrorBitfield *eb)
{  guint64 w_i, bit;
   const guint64 *src;
   unsigned i, j, bits;

   /* First mip level is for final layer of FFT: pairs of data */
   for(i = 0; i < EB_WORDS; i++)
   {  w_i = eb->words[0][i];
      eb->words[0][i] = w_i = w_i | ((w_i & hi_masks[0]) >> 1) | ((w_i & (hi_masks[0] >> 1)) << 1);

      for(j = 1, bits = 2; j < EB_WORD_MIPS; j++, bits <<= 1)
	eb->words[j][i] = w_i = w_i | ((w_i & hi_masks[j]) >> bits) | ((w_i & (hi_masks[j] >> bits)) << bits);
   }

   for(i = 0; i < EB_BIG_WORDS; i++)
   {  w_i = 0;
      bit = 1;
      src = &eb->words[EB_WORD_MIPS - 1][i * 64];
      for(j = 0; j < 64; j++, bit <<= 1)
      {  guint64 w = src[j];
	 w_i |= (w | (w >> 32) | (w << 32)) & bit;
      }
      eb->bigWords[0][i] = w_i;

      for(j = 1, bits = 1; j < EB_BIG_MIPS; j++, bits <<= 1)
	eb->bigWords[j][i] = w_i = w_i | ((w_i & hi_masks[j - 1]) >> bits) | ((w_i & (hi_masks[j - 1] >> bits)) << bits);
   }

   w_i = 0;
   bit = 1;
   src = &eb->bigWords[EB_BIG_MIPS - 1][0];
   for(j = 0; j < EB_BIG_WORDS; j++, bit <<= 1)
   {  guint64 w = src[j];
      w_i |= (w | (w >> 32) | (w << 32)) & bit;
   }
   eb->biggestWords[0] = w_i;

   for(j = 1, bits = 1; j < EB_BIGGEST_MIPS; j++, bits <<= 1)
     eb->biggestWords[j] = w_i = w_i | ((w_i & hi_masks[j - 1]) >> bits) | ((w_i & (hi_masks[j - 1] >> bits)) << bits);
}

static gboolean eb_is_needed(const ErrorBitfield *eb, unsigned mip_level, unsigned bit)
{  if(!eb || mip_level >= 16)
     return TRUE;
   if(mip_level >= 12)
   {  bit /= 4096;
      return 0 != (eb->biggestWords[mip_level - 12] & ((guint64)1 << bit));
   }
   if(mip_level >= 6)
   {  bit /= 64;
      return 0 != (eb->bigWords[mip_level - 6][bit / 64] & ((guint64)1 << (bit % 64)));
   }
   return 0 != (eb->words[mip_level - 1][bit / 64] & ((guint64)1 << (bit % 64)));
}

/* In-place decimation-in-time FFT of m buffers; only the outputs
   [0..m_truncated-1] (further restricted by eb, if given) are valid. */
static void fft_dit(guint64 bytes, void **work, unsigned m_truncated, unsigned m,
		    const ffe_t *skew, const ErrorBitfield *eb)
{  const Kernels *k = kern;
   MulLut l01, l23, l02;
   unsigned mip_level = last_nonzero_bit(m);
   unsigned dist4 = m, dist = m >> 2;
   unsigned r, i;

   for(; dist != 0; dist4 = dist, dist >>= 2, mip_level -= 2)
   {  for(r = 0; r < m_truncated; r += dist4)
      {  unsigned i_end = r + dist;
	 const MulLut *lut01, *lut02, *lut23;

	 if(!eb_is_needed(eb, mip_level, r))
	   continue;

	 lut01 = get_lut(&l01, skew[i_end]);
	 lut02 = get_lut(&l02, skew[i_end + dist]);
	 lut23 = get_lut(&l23, skew[i_end + dist * 2]);

	 for(i = r; i < i_end; i++)
	   k->fft_dit4(bytes, work + i, dist, lut01, lut23, lut02);
      }
   }

   /* If there is one layer left */
   if(dist4 == 2)
   {  for(r = 0; r < m_truncated; r += 2)
      {  if(!eb_is_needed(eb, mip_level, r))
	   continue;

	 k->fft_dit2(work[r], work[r + 1], get_lut(&l01, skew[r + 1]), bytes);
      }
   }
}

/***
 *** Reed-Solomon encode / decode for m = NextPow2(recovery_count) >= 2
 ***/

/* work <- IFFT(data[0..m_truncated-1] padded with zeros to m), optionally
   xor'ed into xor_result[0..m-1] */
static void ifft_encoder_step(guint64 bytes, const void* const* data, unsigned m_truncated,
			      void **work, void **xor_result, unsigned m, const ffe_t *skew)
{  unsigned i;

   for(i = 0; i < m_truncated; i++)
     memcpy(work[i], data[i], bytes);
   for(; i < m; i++)
     memset(work[i], 0, bytes);

   ifft_dit(bytes, work, m_truncated, m, skew);

   if(xor_result)
     for(i = 0; i < m; i++)
       kern->xor_mem(xor_result[i], work[i], bytes);
}

static void rs_encode(guint64 bytes, unsigned original_count, unsigned recovery_count,
		      unsigned m, const void* const* data, void **work)
{  const ffe_t *skew = fft_skew + m - 1;
   unsigned i;

   /* work <- IFFT(data, m, m) */
   ifft_encoder_step(bytes, data, MIN(original_count, m), work, NULL, m, skew);

   /* work <- work xor IFFT(data + i, m, m + i), last set may be partial */
   for(i = m; i < original_count; i += m)
   {  data += m;
      skew += m;
      ifft_encoder_step(bytes, data, MIN(original_count - i, m), work + m, work, m, skew);
   }

   /* work <- FFT(work, m, 0) */
   fft_dit(bytes, work, recovery_count, m, fft_skew_m1, NULL);
}

static void rs_decode(guint64 bytes, unsigned original_count, unsigned recovery_count,
		      unsigned m, unsigned n, const void* const* original,
		      const void* const* recovery, void **work)
{  ffe_t *err_loc = g_malloc0(FF_ORDER * sizeof(ffe_t));
   ErrorBitfield *eb = g_malloc0(sizeof(ErrorBitfield));
   unsigned i;

   /* Fill in error locations */

   for(i = 0; i < recovery_count; i++)
     if(!recovery[i])
       err_loc[i] = 1;
   for(; i < m; i++)
     err_loc[i] = 1;
   for(i = 0; i < original_count; i++)
     if(!original[i])
     {  err_loc[i + m] = 1;
	eb_set(eb, i + m);
     }

   eb_prepare(eb);

   /* Evaluate error locator polynomial */

   fwht(err_loc, FF_ORDER, m + original_count);

   for(i = 0; i < FF_ORDER; i++)
     err_loc[i] = ((unsigned)err_loc[i] * (unsigned)log_walsh[i]) % FF_MODULUS;

   fwht(err_loc, FF_ORDER, FF_ORDER);

   /* work <- recovery data, original data, zero padding */

   for(i = 0; i < recovery_count; i++)
   {  if(recovery[i]) mul_mem(work[i], recovery[i], err_loc[i], bytes);
      else            memset(work[i], 0, bytes);
   }
   for(; i < m; i++)
     memset(work[i], 0, bytes);

   for(i = 0; i < original_count; i++)
   {  if(original[i]) mul_mem(work[m + i], original[i], err_loc[m + i], bytes);
      else            memset(work[m + i], 0, bytes);
   }
   for(i = m + original_count; i < n; i++)
     memset(work[i], 0, bytes);

   /* work <- IFFT(work, n, 0) */

   ifft_dit(bytes, work, m + original_count, n, fft_skew_m1);

   /* work <- FormalDerivative(work, n) */

   for(i = 1; i < n; i++)
   {  unsigned width = ((i ^ (i - 1)) + 1) >> 1;
      unsigned j;

      for(j = 0; j < width; j++)
	kern->xor_mem(work[i - width + j], work[i + j], bytes);
   }

   /* work <- FFT(work, n, 0) truncated to m + original_count */

   fft_dit(bytes, work, m + original_count, n, fft_skew_m1, eb);

   /* Reveal erasures */

   for(i = 0; i < original_count; i++)
     if(!original[i])
       mul_mem(work[i], work[i + m], FF_MODULUS - err_loc[i + m], bytes);

   g_free(eb);
   g_free(err_loc);
}

/***
 *** Public API (mirrors leopard.cpp)
 ***/

unsigned RS04EncodeWorkCount(unsigned original_count, unsigned recovery_count)
{  if(original_count == 1)
     return recovery_count;
   if(recovery_count == 1)
     return 1;
   return next_pow2(recovery_count) * 2;
}

unsigned RS04DecodeWorkCount(unsigned original_count, unsigned recovery_count)
{  if(original_count == 1 || recovery_count == 1)
     return original_count;
   return next_pow2(next_pow2(recovery_count) + original_count);
}

/* dest = xor of the count buffers in src[] (NULL entries are skipped) and of init */
static void xor_parity(guint64 bytes, unsigned count, const void* const* src,
		       const void *init, void *dest)
{  unsigned i;

   memcpy(dest, init, bytes);
   for(i = 0; i < count; i++)
     if(src[i])
       kern->xor_mem(dest, src[i], bytes);
}

int RS04Encode(guint64 buffer_bytes, unsigned original_count, unsigned recovery_count,
	       unsigned work_count, const void* const* original, void** work)
{  unsigned m;

   if(buffer_bytes == 0 || buffer_bytes % 64 != 0)
     return RS04_INVALID_SIZE;
   if(recovery_count == 0 || recovery_count > original_count)
     return RS04_INVALID_COUNTS;
   if(!original || !work)
     return RS04_INVALID_INPUT;

   RS04InitGF16();

   /* k = 1 (implies m = 1): copy */
   if(original_count == 1)
   {  memcpy(work[0], original[0], buffer_bytes);
      return RS04_SUCCESS;
   }

   /* m = 1: xor parity */
   if(recovery_count == 1)
   {  xor_parity(buffer_bytes, original_count - 1, original + 1, original[0], work[0]);
      return RS04_SUCCESS;
   }

   if(original_count > FF_ORDER)
     return RS04_TOO_MUCH_DATA;

   m = next_pow2(recovery_count);
   if(work_count != m * 2)
     return RS04_INVALID_COUNTS;
   if(m + original_count > FF_ORDER)
     return RS04_TOO_MUCH_DATA;

   rs_encode(buffer_bytes, original_count, recovery_count, m, original, work);
   return RS04_SUCCESS;
}

int RS04Decode(guint64 buffer_bytes, unsigned original_count, unsigned recovery_count,
	       unsigned work_count, const void* const* original,
	       const void* const* recovery, void** work)
{  unsigned original_loss_count = 0, original_loss_i = 0;
   unsigned recovery_got_count = 0, recovery_got_i = 0;
   unsigned i, m;

   if(buffer_bytes == 0 || buffer_bytes % 64 != 0)
     return RS04_INVALID_SIZE;
   if(recovery_count == 0 || recovery_count > original_count)
     return RS04_INVALID_COUNTS;
   if(!original || !recovery || !work)
     return RS04_INVALID_INPUT;

   RS04InitGF16();

   for(i = 0; i < original_count; i++)
     if(!original[i])
     {  original_loss_count++;
	original_loss_i = i;
     }
   for(i = 0; i < recovery_count; i++)
     if(recovery[i])
     {  recovery_got_count++;
	recovery_got_i = i;
     }
   if(recovery_got_count < original_loss_count)
     return RS04_NEED_MORE_DATA;

   /* k = 1. Leopard1 always copies recovery[] here (and crashes if it is
      missing); the result is the same data. */
   if(original_count == 1)
   {  memcpy(work[0], original[0] ? original[0] : recovery[recovery_got_i], buffer_bytes);
      return RS04_SUCCESS;
   }

   /* Nothing lost */
   if(original_loss_count == 0)
   {  for(i = 0; i < original_count; i++)
	memcpy(work[i], original[i], buffer_bytes);
      return RS04_SUCCESS;
   }

   /* m = 1: xor parity */
   if(recovery_count == 1)
   {  xor_parity(buffer_bytes, original_count, original, recovery[0], work[original_loss_i]);
      return RS04_SUCCESS;
   }

   if(original_count > FF_ORDER)
     return RS04_TOO_MUCH_DATA;

   m = next_pow2(recovery_count);
   if(work_count != next_pow2(m + original_count))
     return RS04_INVALID_COUNTS;
   if(work_count > FF_ORDER)
     return RS04_TOO_MUCH_DATA;

   rs_decode(buffer_bytes, original_count, recovery_count, m, work_count,
	     original, recovery, work);
   return RS04_SUCCESS;
}
