/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native-family boundaries, also runnable under address/undefined sanitizers. */
#include <glib.h>
#include <string.h>
#include "../src/rs04-ff16.h"

static unsigned pow2(unsigned x)
{  unsigned result=1;
   while(result<x) result*=2;
   return result;
}

static void round_trip(unsigned k, unsigned reserve)
{  unsigned ew=reserve*2, dw=pow2(k+reserve), i, j;
   unsigned char *data=g_malloc((gsize)k*64);
   unsigned char *encoded=g_malloc((gsize)ew*64);
   unsigned char *decoded=g_malloc((gsize)dw*64);
   const void **original=g_new(const void*,k);
   const void **parity=g_new0(const void*,reserve);
   void **work=g_new(void*,MAX(ew,dw));

   for(i=0;i<k;i++)
   {  original[i]=data+(gsize)i*64;
      for(j=0;j<64;j++) data[(gsize)i*64+j]=(unsigned char)(i*37+j*19);
   }
   for(i=0;i<ew;i++) work[i]=encoded+(gsize)i*64;
   g_assert_cmpint(RS04EncodeFamily(64,k,reserve,ew,original,work),==,RS04_SUCCESS);
   parity[reserve-1]=encoded+(gsize)(reserve-1)*64;
   original[k-1]=NULL;
   for(i=0;i<dw;i++) work[i]=decoded+(gsize)i*64;
   g_assert_cmpint(RS04DecodeFamily(64,k,reserve,dw,original,parity,work),==,RS04_SUCCESS);
   g_assert(memcmp(work[k-1],data+(gsize)(k-1)*64,64)==0);
   parity[reserve-1]=NULL;
   g_assert_cmpint(RS04DecodeFamily(64,k,reserve,dw,original,parity,work),==,RS04_NEED_MORE_DATA);
   g_free(work); g_free(parity); g_free(original);
   g_free(decoded); g_free(encoded); g_free(data);
}

int main(void)
{  unsigned char region[128]={0};
   const void *original[1]={region};
   void *work[2]={region,region+64};
   const void *parity[1]={NULL};
   g_assert_cmpint(RS04EncodeFamily(63,1,1,2,original,work),==,RS04_INVALID_SIZE);
   g_assert_cmpint(RS04EncodeFamily(64,0,1,2,original,work),==,RS04_INVALID_COUNTS);
   g_assert_cmpint(RS04EncodeFamily(64,1,3,2,original,work),==,RS04_INVALID_COUNTS);
   g_assert_cmpint(RS04EncodeFamily(64,65536,1,2,original,work),==,RS04_TOO_MUCH_DATA);
   g_assert_cmpint(RS04EncodeFamily(64,1,1,0,original,work),==,RS04_INVALID_INPUT);
   g_assert_cmpint(RS04DecodeFamily(64,1,1,2,NULL,parity,work),==,RS04_INVALID_INPUT);
   region[0]=1;
   g_assert_cmpint(RS04MulAddFamily(64,0xACCA,region,region+64),==,RS04_SUCCESS);
   g_assert_cmpint(region[64],==,0xCA);
   g_assert_cmpint(region[96],==,0xAC);
   g_assert_cmpint(RS04MulAddFamily(64,1,region,region),==,RS04_SUCCESS);
   g_assert_cmpint(region[0],==,0);
   g_assert_cmpint(RS04MulAddFamily(63,1,region,region),==,RS04_INVALID_SIZE);
   round_trip(1,1);
   round_trip(1,4);
   round_trip(5,4);
   round_trip(2,8);
   round_trip(65535,1);
   round_trip(32768,32768);
   g_print("RS04 native family API: boundaries and maximum field-size round trips passed\n");
   return 0;
}
