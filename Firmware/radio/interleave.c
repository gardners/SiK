// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2013 Paul Gardner-Stephen, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

///
/// @file	interleave.c
///
/// interleave and de-interleave packets before golay encoding to
/// improve performance of golay error detection/correction in the
/// face of burst errors.
///
/// As the golay coder can correct 3 in every 12 bits, a byte-level
/// interleaver is probably sufficient for now. 

#ifdef STANDALONE
#include <stdio.h>
#define __pdata
#define __xdata
#define uint8_t unsigned char
#define uint16_t unsigned short
#else
#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#endif

#define MAXBITS 256*8
#define BITSTEP 24 /* 3bytes*8bits */

__code const uint16_t steps[86]={
0, // n=0 bytes, no two bits are closer than 1 bits apart, total distance metric = 0 bits
7, // n=3 bytes, no two bits are closer than 7 bits apart, total distance metric = 168 bits
9, // n=6 bytes, no two bits are closer than 9 bits apart, total distance metric = 433 bits
28, // n=9 bytes, no two bits are closer than 11 bits apart, total distance metric = 794 bits
13, // n=12 bytes, no two bits are closer than 13 bits apart, total distance metric = 1249 bits
68, // n=15 bytes, no two bits are closer than 15 bits apart, total distance metric = 1801 bits
17, // n=18 bytes, no two bits are closer than 17 bits apart, total distance metric = 2448 bits
20, // n=21 bytes, no two bits are closer than 17 bits apart, total distance metric = 2858 bits
57, // n=24 bytes, no two bits are closer than 19 bits apart, total distance metric = 3649 bits
47, // n=27 bytes, no two bits are closer than 20 bits apart, total distance metric = 4321 bits
21, // n=30 bytes, no two bits are closer than 21 bits apart, total distance metric = 5041 bits
23, // n=33 bytes, no two bits are closer than 23 bits apart, total distance metric = 6072 bits
104, // n=36 bytes, no two bits are closer than 23 bits apart, total distance metric = 6626 bits
25, // n=39 bytes, no two bits are closer than 25 bits apart, total distance metric = 7800 bits
25, // n=42 bytes, no two bits are closer than 25 bits apart, total distance metric = 8401 bits
233, // n=45 bytes, no two bits are closer than 26 bits apart, total distance metric = 9361 bits
31, // n=48 bytes, no two bits are closer than 25 bits apart, total distance metric = 9605 bits
110, // n=51 bytes, no two bits are closer than 28 bits apart, total distance metric = 11425 bits
32, // n=54 bytes, no two bits are closer than 29 bits apart, total distance metric = 12528 bits
60, // n=57 bytes, no two bits are closer than 29 bits apart, total distance metric = 13226 bits
31, // n=60 bytes, no two bits are closer than 31 bits apart, total distance metric = 14880 bits
107, // n=63 bytes, no two bits are closer than 31 bits apart, total distance metric = 15625 bits
281, // n=66 bytes, no two bits are closer than 32 bits apart, total distance metric = 16897 bits
66, // n=69 bytes, no two bits are closer than 33 bits apart, total distance metric = 18216 bits
33, // n=72 bytes, no two bits are closer than 33 bits apart, total distance metric = 19009 bits
159, // n=75 bytes, no two bits are closer than 34 bits apart, total distance metric = 20401 bits
146, // n=78 bytes, no two bits are closer than 35 bits apart, total distance metric = 21840 bits
228, // n=81 bytes, no two bits are closer than 35 bits apart, total distance metric = 22682 bits
188, // n=84 bytes, no two bits are closer than 36 bits apart, total distance metric = 24193 bits
111, // n=87 bytes, no two bits are closer than 37 bits apart, total distance metric = 25752 bits
37, // n=90 bytes, no two bits are closer than 37 bits apart, total distance metric = 26641 bits
354, // n=93 bytes, no two bits are closer than 37 bits apart, total distance metric = 27531 bits
82, // n=96 bytes, no two bits are closer than 39 bits apart, total distance metric = 29952 bits
118, // n=99 bytes, no two bits are closer than 39 bits apart, total distance metric = 30889 bits
239, // n=102 bytes, no two bits are closer than 39 bits apart, total distance metric = 31826 bits
41, // n=105 bytes, no two bits are closer than 41 bits apart, total distance metric = 34440 bits
253, // n=108 bytes, no two bits are closer than 41 bits apart, total distance metric = 35424 bits
186, // n=111 bytes, no two bits are closer than 41 bits apart, total distance metric = 36410 bits
191, // n=114 bytes, no two bits are closer than 42 bits apart, total distance metric = 38305 bits
196, // n=117 bytes, no two bits are closer than 43 bits apart, total distance metric = 40248 bits
143, // n=120 bytes, no two bits are closer than 43 bits apart, total distance metric = 41281 bits
402, // n=123 bytes, no two bits are closer than 43 bits apart, total distance metric = 42314 bits
95, // n=126 bytes, no two bits are closer than 44 bits apart, total distance metric = 44353 bits
48, // n=129 bytes, no two bits are closer than 45 bits apart, total distance metric = 46440 bits
45, // n=132 bytes, no two bits are closer than 45 bits apart, total distance metric = 47521 bits
93, // n=135 bytes, no two bits are closer than 46 bits apart, total distance metric = 49681 bits
47, // n=138 bytes, no two bits are closer than 47 bits apart, total distance metric = 51888 bits
588, // n=141 bytes, no two bits are closer than 47 bits apart, total distance metric = 53017 bits
400, // n=144 bytes, no two bits are closer than 47 bits apart, total distance metric = 54146 bits
613, // n=147 bytes, no two bits are closer than 48 bits apart, total distance metric = 56449 bits
49, // n=150 bytes, no two bits are closer than 49 bits apart, total distance metric = 58800 bits
99, // n=153 bytes, no two bits are closer than 49 bits apart, total distance metric = 59977 bits
49, // n=156 bytes, no two bits are closer than 49 bits apart, total distance metric = 61153 bits
52, // n=159 bytes, no two bits are closer than 49 bits apart, total distance metric = 62330 bits
168, // n=162 bytes, no two bits are closer than 50 bits apart, total distance metric = 64801 bits
255, // n=165 bytes, no two bits are closer than 51 bits apart, total distance metric = 67320 bits
511, // n=168 bytes, no two bits are closer than 50 bits apart, total distance metric = 67203 bits
157, // n=171 bytes, no two bits are closer than 51 bits apart, total distance metric = 69770 bits
241, // n=174 bytes, no two bits are closer than 52 bits apart, total distance metric = 72385 bits
294, // n=177 bytes, no two bits are closer than 53 bits apart, total distance metric = 75048 bits
299, // n=180 bytes, no two bits are closer than 53 bits apart, total distance metric = 76321 bits
168, // n=183 bytes, no two bits are closer than 53 bits apart, total distance metric = 77594 bits
905, // n=186 bytes, no two bits are closer than 54 bits apart, total distance metric = 80353 bits
55, // n=189 bytes, no two bits are closer than 55 bits apart, total distance metric = 83160 bits
447, // n=192 bytes, no two bits are closer than 55 bits apart, total distance metric = 84480 bits
179, // n=195 bytes, no two bits are closer than 55 bits apart, total distance metric = 85801 bits
306, // n=198 bytes, no two bits are closer than 55 bits apart, total distance metric = 87122 bits
221, // n=201 bytes, no two bits are closer than 56 bits apart, total distance metric = 90049 bits
176, // n=204 bytes, no two bits are closer than 57 bits apart, total distance metric = 93024 bits
399, // n=207 bytes, no two bits are closer than 57 bits apart, total distance metric = 94393 bits
57, // n=210 bytes, no two bits are closer than 57 bits apart, total distance metric = 95761 bits
293, // n=213 bytes, no two bits are closer than 57 bits apart, total distance metric = 97130 bits
255, // n=216 bytes, no two bits are closer than 58 bits apart, total distance metric = 100225 bits
410, // n=219 bytes, no two bits are closer than 59 bits apart, total distance metric = 103368 bits
430, // n=222 bytes, no two bits are closer than 59 bits apart, total distance metric = 104785 bits
620, // n=225 bytes, no two bits are closer than 59 bits apart, total distance metric = 106202 bits
172, // n=228 bytes, no two bits are closer than 59 bits apart, total distance metric = 107618 bits
178, // n=231 bytes, no two bits are closer than 60 bits apart, total distance metric = 110881 bits
436, // n=234 bytes, no two bits are closer than 61 bits apart, total distance metric = 114192 bits
305, // n=237 bytes, no two bits are closer than 61 bits apart, total distance metric = 115657 bits
61, // n=240 bytes, no two bits are closer than 61 bits apart, total distance metric = 117121 bits
252, // n=243 bytes, no two bits are closer than 61 bits apart, total distance metric = 118586 bits
1047, // n=246 bytes, no two bits are closer than 62 bits apart, total distance metric = 122017 bits
435, // n=249 bytes, no two bits are closer than 63 bits apart, total distance metric = 125496 bits
1040, // n=252 bytes, no two bits are closer than 63 bits apart, total distance metric = 127009 bits
394 // n=255 bytes, no two bits are closer than 63 bits apart, total distance metric = 128521 bits
};

// These are macros to save RAM, even though inline functions probably shouldn't
// use RAM just by existing.
#define interleave_getbit(n,in,bit) ((in[(((bit)*steps[n/3])%n)>>3]>>((((bit)*steps[n/3])%n)&7))?1:0)

#define interleave_setbit(n,in,bit, value) \
  { \
    if ((value)==0) in[(((bit)*steps[n/3])%n)>>3]&=~1<<((((bit)*steps[n/3])%n)&7); \
    else in[(((bit)*steps[n/3])%n)>>3]|=1<<((((bit)*steps[n/3])%n)&7); \
  }

uint8_t interleave_getbyte(__pdata uint8_t n, __xdata uint8_t * __pdata in,
			   __pdata uint8_t index)
{
  register uint8_t v=0;
  v|=interleave_getbit(n,in,index*8+0);
  v|=interleave_getbit(n,in,index*8+1)<<1;
  v|=interleave_getbit(n,in,index*8+2)<<2;
  v|=interleave_getbit(n,in,index*8+3)<<3;
  v|=interleave_getbit(n,in,index*8+4)<<4;
  v|=interleave_getbit(n,in,index*8+5)<<5;
  v|=interleave_getbit(n,in,index*8+6)<<6;
  v|=interleave_getbit(n,in,index*8+7)<<7;
  return v;
}

void interleave_setbyte(__pdata uint8_t n, __xdata uint8_t * __pdata in,
			       __pdata uint8_t index, uint8_t __pdata value)
{
  interleave_setbit(n,in,(index<<3)+0,(value>>0)&1);
  interleave_setbit(n,in,(index<<3)+1,(value>>1)&1);
  interleave_setbit(n,in,(index<<3)+2,(value>>2)&1);
  interleave_setbit(n,in,(index<<3)+3,(value>>3)&1);
  interleave_setbit(n,in,(index<<3)+4,(value>>4)&1);
  interleave_setbit(n,in,(index<<3)+5,(value>>5)&1);
  interleave_setbit(n,in,(index<<3)+6,(value>>6)&1);
  interleave_setbit(n,in,(index<<3)+7,(value>>7)&1);
}

#ifdef STANDALONE

int prefill(int *b)
{
  int i;
  for(i=0;i<MAXBITS;i++) b[i]=i;
  return 0;
}

int show(int n,int *b)
{
  int i;
  for(i=0;i<n;i++) {
    if (!(i&0xf)) printf("\n%02x:",i);
    printf(" %02x",b[i]);
  }
  printf("\n");
  return 0;
}

int main(int argc,char **argv)
{
  int b[MAXBITS];
  int c[MAXBITS];
  int n,i,j;
  unsigned short step=0;
  printf("uint16_t steps[%d]={\n",MAXBITS/BITSTEP+1);
  for(n=1;n<MAXBITS;n+=BITSTEP) {
    int bestmindiff=-1;
    int besttotaldiff=-1;
    int beststep=-1;
    for(step=0;step<MAXBITS;step++) {
      prefill(b);
      bzero(&c[0],MAXBITS*sizeof(int));
      for(i=0;i<n;i++) {
	b[i]=(i*step)%n;
	c[(i*step)%n]++;
      }
      for(i=0;i<n;i++) if (c[i]!=1) {
	  break;
	}
      if (i==n) {
	int mindiff=n;
	int totaldiff=0;
	for(i=0;i<n-1;i++) {
	  int diff=MAXBITS;
	  for(j=0;j<i;j++) {
	    int bdiff=b[i]-b[j]; if (bdiff<0) bdiff=-bdiff;
	    bdiff+=(i-j);	    
	    if (bdiff<diff) diff=bdiff;
	  }
	  for(j=i+1;j<n;j++) {
	    int bdiff=b[i]-b[j]; if (bdiff<0) bdiff=-bdiff;
	    bdiff+=(j-i);	    
	    if (bdiff<diff) diff=bdiff;
	  }

	  if (diff<0) diff=-diff;
	  if (diff<mindiff) mindiff=diff;
	  if (mindiff<bestmindiff) break;
	  totaldiff+=diff;
	}
	if (mindiff>bestmindiff
	    ||(mindiff==bestmindiff&&totaldiff>besttotaldiff)) {
	  beststep=step;
	  bestmindiff=mindiff;
	  besttotaldiff=totaldiff;
	  //	  printf("n=%d, step=%d, mindiff=%d, totaldiff=%d\n",
	  //	 n,step,mindiff,totaldiff);
	}
      }
    }
    printf("%d%s // n=%d bytes, no two bits are closer than %d bits apart, total distance metric = %d bits\n",
	   beststep,(n!=MAXBITS/BITSTEP-1)?",":"",
	   n/8,bestmindiff,besttotaldiff);
  }
  printf("};\n");
}

#endif