/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2017 Carsten Gnoerlich.
 *  Copyright (C) 2019-2026 The dvdisaster development team.
 *
 *  The RS04 SIMD kernels are a C port of the SSSE3/AVX2 code of Leopard-RS:
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

/*** src type: no GUI code ***/

/*
 * Template for the RS04 SIMD data kernels; included by rs04-ff16.c once per
 * instruction set after defining:
 *
 *   SIMD(name)       function name suffix, e.g. name##_avx2
 *   TARGET           function attribute selecting the instruction set
 *   V, VW            vector type and its width in bytes (16 or 32)
 *   V_LOAD(p)        unaligned load,  V_STORE(p, v) unaligned store
 *   V_XOR(a, b)      a ^ b
 *   V_MASK           all bytes 0x0f
 *   V_TABLE(p)       16 byte shuffle table at p, repeated to fill V
 *   V_SHUF(t, i)     t[i] per byte for indices 0..15 (pshufb / tbl)
 *   V_LONIB(v, m)    low nibbles of v,  V_HINIB(v, m) high nibbles of v
 *
 * The 64 byte blocks hold the low symbol bytes in [0..31] and the high bytes
 * in [32..63], so a VW wide slice at offset h (< 32) pairs with offset h+32.
 * Multiplication splits each symbol into four nibbles and looks up the
 * partial products of the low and high result bytes (Leopard's
 * LEO_MUL_128 / LEO_MUL_256).
 */

typedef struct { V lo[4], hi[4]; } SIMD(Tab);

#define FOR_SLICES(o, h, bytes) \
   for(o = 0; o < (bytes); o += 64) \
     for(h = o; h < o + 32; h += VW)

TARGET static inline void SIMD(load_tab)(SIMD(Tab) *t, const MulLut *lut)
{  unsigned n;

   for(n = 0; n < 4; n++)
   {  t->lo[n] = V_TABLE(lut->lo + 16 * n);
      t->hi[n] = V_TABLE(lut->hi + 16 * n);
   }
}

/* {x_lo, x_hi} ^= {y_lo, y_hi} * t */
TARGET static inline void SIMD(muladd)(V *x_lo, V *x_hi, V y_lo, V y_hi, const SIMD(Tab) *t, V mask)
{  V n0 = V_LONIB(y_lo, mask), n1 = V_HINIB(y_lo, mask);
   V n2 = V_LONIB(y_hi, mask), n3 = V_HINIB(y_hi, mask);
   V lo = V_XOR(V_XOR(V_SHUF(t->lo[0], n0), V_SHUF(t->lo[1], n1)),
		V_XOR(V_SHUF(t->lo[2], n2), V_SHUF(t->lo[3], n3)));
   V hi = V_XOR(V_XOR(V_SHUF(t->hi[0], n0), V_SHUF(t->hi[1], n1)),
		V_XOR(V_SHUF(t->hi[2], n2), V_SHUF(t->hi[3], n3)));

   *x_lo = V_XOR(*x_lo, lo);
   *x_hi = V_XOR(*x_hi, hi);
}

/* x[] ^= y[] */
TARGET static void SIMD(xor_mem)(void *x, const void *y, guint64 bytes)
{  guint8 *x1 = x;
   const guint8 *y1 = y;
   guint64 o, h;

   FOR_SLICES(o, h, bytes)
   {  V_STORE(x1 + h, V_XOR(V_LOAD(x1 + h), V_LOAD(y1 + h)));
      V_STORE(x1 + h + 32, V_XOR(V_LOAD(x1 + h + 32), V_LOAD(y1 + h + 32)));
   }
}

/* x[] = y[] * lut */
TARGET static void SIMD(mul_mem)(void *x, const void *y, const MulLut *lut, guint64 bytes)
{  guint8 *x1 = x;
   const guint8 *y1 = y;
   const V mask = V_MASK;
   SIMD(Tab) t;
   guint64 o, h;

   SIMD(load_tab)(&t, lut);
   FOR_SLICES(o, h, bytes)
   {  V lo = V_XOR(mask, mask), hi = lo;

      SIMD(muladd)(&lo, &hi, V_LOAD(y1 + h), V_LOAD(y1 + h + 32), &t, mask);
      V_STORE(x1 + h, lo);
      V_STORE(x1 + h + 32, hi);
   }
}

/* y[] ^= x[]; if(lut) x[] ^= y[] * lut */
TARGET static void SIMD(ifft_dit2)(void *x, void *y, const MulLut *lut, guint64 bytes)
{  guint8 *x1 = x, *y1 = y;
   const V mask = V_MASK;
   SIMD(Tab) t;
   guint64 o, h;

   if(!lut)
   {  SIMD(xor_mem)(y, x, bytes);
      return;
   }

   SIMD(load_tab)(&t, lut);
   FOR_SLICES(o, h, bytes)
   {  V x_lo = V_LOAD(x1 + h), x_hi = V_LOAD(x1 + h + 32);
      V y_lo = V_XOR(V_LOAD(y1 + h), x_lo), y_hi = V_XOR(V_LOAD(y1 + h + 32), x_hi);

      V_STORE(y1 + h, y_lo);
      V_STORE(y1 + h + 32, y_hi);
      SIMD(muladd)(&x_lo, &x_hi, y_lo, y_hi, &t, mask);
      V_STORE(x1 + h, x_lo);
      V_STORE(x1 + h + 32, x_hi);
   }
}

/* if(lut) x[] ^= y[] * lut; y[] ^= x[] */
TARGET static void SIMD(fft_dit2)(void *x, void *y, const MulLut *lut, guint64 bytes)
{  guint8 *x1 = x, *y1 = y;
   const V mask = V_MASK;
   SIMD(Tab) t;
   guint64 o, h;

   if(!lut)
   {  SIMD(xor_mem)(y, x, bytes);
      return;
   }

   SIMD(load_tab)(&t, lut);
   FOR_SLICES(o, h, bytes)
   {  V x_lo = V_LOAD(x1 + h), x_hi = V_LOAD(x1 + h + 32);
      V y_lo = V_LOAD(y1 + h), y_hi = V_LOAD(y1 + h + 32);

      SIMD(muladd)(&x_lo, &x_hi, y_lo, y_hi, &t, mask);
      V_STORE(x1 + h, x_lo);
      V_STORE(x1 + h + 32, x_hi);
      V_STORE(y1 + h, V_XOR(y_lo, x_lo));
      V_STORE(y1 + h + 32, V_XOR(y_hi, x_hi));
   }
}

/* Both layers of the 4-way butterflies are done in registers
   (Leopard: LEO_INTERLEAVE_BUTTERFLY4_OPT). A NULL lut skips its multiply. */

TARGET static void SIMD(ifft_dit4)(guint64 bytes, void **work, unsigned dist,
				   const MulLut *lut01, const MulLut *lut23, const MulLut *lut02)
{  guint8 *p0 = work[0], *p1 = work[dist], *p2 = work[dist * 2], *p3 = work[dist * 3];
   const V mask = V_MASK;
   SIMD(Tab) t01, t23, t02;
   guint64 o, h;

   SIMD(load_tab)(&t01, lut01 ? lut01 : &zero_lut);
   SIMD(load_tab)(&t23, lut23 ? lut23 : &zero_lut);
   SIMD(load_tab)(&t02, lut02 ? lut02 : &zero_lut);

   FOR_SLICES(o, h, bytes)
   {  V lo0 = V_LOAD(p0 + h), hi0 = V_LOAD(p0 + h + 32);
      V lo1 = V_LOAD(p1 + h), hi1 = V_LOAD(p1 + h + 32);
      V lo2 = V_LOAD(p2 + h), hi2 = V_LOAD(p2 + h + 32);
      V lo3 = V_LOAD(p3 + h), hi3 = V_LOAD(p3 + h + 32);

      /* First layer */
      lo1 = V_XOR(lo1, lo0); hi1 = V_XOR(hi1, hi0);
      if(lut01) SIMD(muladd)(&lo0, &hi0, lo1, hi1, &t01, mask);
      lo3 = V_XOR(lo3, lo2); hi3 = V_XOR(hi3, hi2);
      if(lut23) SIMD(muladd)(&lo2, &hi2, lo3, hi3, &t23, mask);

      /* Second layer */
      lo2 = V_XOR(lo2, lo0); hi2 = V_XOR(hi2, hi0);
      lo3 = V_XOR(lo3, lo1); hi3 = V_XOR(hi3, hi1);
      if(lut02)
      {  SIMD(muladd)(&lo0, &hi0, lo2, hi2, &t02, mask);
	 SIMD(muladd)(&lo1, &hi1, lo3, hi3, &t02, mask);
      }

      V_STORE(p0 + h, lo0); V_STORE(p0 + h + 32, hi0);
      V_STORE(p1 + h, lo1); V_STORE(p1 + h + 32, hi1);
      V_STORE(p2 + h, lo2); V_STORE(p2 + h + 32, hi2);
      V_STORE(p3 + h, lo3); V_STORE(p3 + h + 32, hi3);
   }
}

TARGET static void SIMD(fft_dit4)(guint64 bytes, void **work, unsigned dist,
				  const MulLut *lut01, const MulLut *lut23, const MulLut *lut02)
{  guint8 *p0 = work[0], *p1 = work[dist], *p2 = work[dist * 2], *p3 = work[dist * 3];
   const V mask = V_MASK;
   SIMD(Tab) t01, t23, t02;
   guint64 o, h;

   SIMD(load_tab)(&t01, lut01 ? lut01 : &zero_lut);
   SIMD(load_tab)(&t23, lut23 ? lut23 : &zero_lut);
   SIMD(load_tab)(&t02, lut02 ? lut02 : &zero_lut);

   FOR_SLICES(o, h, bytes)
   {  V lo0 = V_LOAD(p0 + h), hi0 = V_LOAD(p0 + h + 32);
      V lo1 = V_LOAD(p1 + h), hi1 = V_LOAD(p1 + h + 32);
      V lo2 = V_LOAD(p2 + h), hi2 = V_LOAD(p2 + h + 32);
      V lo3 = V_LOAD(p3 + h), hi3 = V_LOAD(p3 + h + 32);

      /* First layer */
      if(lut02)
      {  SIMD(muladd)(&lo0, &hi0, lo2, hi2, &t02, mask);
	 SIMD(muladd)(&lo1, &hi1, lo3, hi3, &t02, mask);
      }
      lo2 = V_XOR(lo2, lo0); hi2 = V_XOR(hi2, hi0);
      lo3 = V_XOR(lo3, lo1); hi3 = V_XOR(hi3, hi1);

      /* Second layer */
      if(lut01) SIMD(muladd)(&lo0, &hi0, lo1, hi1, &t01, mask);
      lo1 = V_XOR(lo1, lo0); hi1 = V_XOR(hi1, hi0);
      if(lut23) SIMD(muladd)(&lo2, &hi2, lo3, hi3, &t23, mask);
      lo3 = V_XOR(lo3, lo2); hi3 = V_XOR(hi3, hi2);

      V_STORE(p0 + h, lo0); V_STORE(p0 + h + 32, hi0);
      V_STORE(p1 + h, lo1); V_STORE(p1 + h + 32, hi1);
      V_STORE(p2 + h, lo2); V_STORE(p2 + h + 32, hi2);
      V_STORE(p3 + h, lo3); V_STORE(p3 + h + 32, hi3);
   }
}

static const Kernels SIMD(kernels) =
{  SIMD_NAME,
   SIMD(xor_mem), SIMD(mul_mem),
   SIMD(ifft_dit2), SIMD(fft_dit2),
   SIMD(ifft_dit4), SIMD(fft_dit4)
};

#undef FOR_SLICES
#undef SIMD
#undef SIMD_NAME
#undef TARGET
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
