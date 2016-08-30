/*
    Copyright (C) 2000 Paul Davis 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#define _ISOC9X_SOURCE  1
#define _ISOC99_SOURCE  1

#define __USE_ISOC9X    1
#define __USE_ISOC99    1

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <memory.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <endian.h>

#include "memops.h"

/* Notes about these *_SCALING values.

   the MAX_<N>BIT values are floating point. when multiplied by
   a full-scale normalized floating point sample value (-1.0..+1.0)
   they should give the maxium value representable with an integer
   sample type of N bits. Note that this is asymmetric. Sample ranges 
   for signed integer, 2's complement values are -(2^(N-1) to +(2^(N-1)-1)

   Complications
   -------------
   If we use +2^(N-1) for the scaling factors, we run into a problem:

   if we start with a normalized float value of -1.0, scaling
   to 24 bits would give -8388608 (-2^23), which is ideal.
   But with +1.0, we get +8388608, which is technically out of range.

   We never multiply a full range normalized value by this constant,
   but we could multiply it by a positive value that is close enough to +1.0
   to produce a value > +(2^(N-1)-1.

   There is no way around this paradox without wasting CPU cycles to determine
   which scaling factor to use (i.e. determine if its negative or not,
   use the right factor).

   So, for now (October 2008) we use 2^(N-1)-1 as the scaling factor.
*/

#define SAMPLE_24BIT_SCALING  8388607.0f
#define SAMPLE_16BIT_SCALING  32767.0f

/* these are just values to use if the floating point value was out of range
   
   advice from Fons Adriaensen: make the limits symmetrical
 */

#define SAMPLE_24BIT_MAX  8388607  
#define SAMPLE_24BIT_MIN  -8388607 
#define SAMPLE_24BIT_MAX_F  8388607.0f  
#define SAMPLE_24BIT_MIN_F  -8388607.0f 

#define SAMPLE_16BIT_MAX  32767
#define SAMPLE_16BIT_MIN  -32767
#define SAMPLE_16BIT_MAX_F  32767.0f
#define SAMPLE_16BIT_MIN_F  -32767.0f

/* these mark the outer edges of the range considered "within" range
   for a floating point sample value. values outside (and on the boundaries) 
   of this range will be clipped before conversion; values within this 
   range will be scaled to appropriate values for the target sample
   type.
*/

#define NORMALIZED_FLOAT_MIN -1.0f
#define NORMALIZED_FLOAT_MAX  1.0f

/* define this in case we end up on a platform that is missing
   the real lrintf functions
*/

#define f_round(f) lrintf(f)

#define float_16(s, d)\
	if ((s) <= NORMALIZED_FLOAT_MIN) {\
		(d) = SAMPLE_16BIT_MIN;\
	} else if ((s) >= NORMALIZED_FLOAT_MAX) {\
		(d) = SAMPLE_16BIT_MAX;\
	} else {\
		(d) = f_round ((s) * SAMPLE_16BIT_SCALING);\
	}

/* call this when "s" has already been scaled (e.g. when dithering)
 */

#define float_16_scaled(s, d)\
        if ((s) <= SAMPLE_16BIT_MIN_F) {\
		(d) = SAMPLE_16BIT_MIN_F;\
	} else if ((s) >= SAMPLE_16BIT_MAX_F) {	\
		(d) = SAMPLE_16BIT_MAX;\
	} else {\
	        (d) = f_round ((s));\
	}

#define float_24u32(s, d) \
	if ((s) <= NORMALIZED_FLOAT_MIN) {\
		(d) = SAMPLE_24BIT_MIN << 8;\
	} else if ((s) >= NORMALIZED_FLOAT_MAX) {\
		(d) = SAMPLE_24BIT_MAX << 8;\
	} else {\
		(d) = f_round ((s) * SAMPLE_24BIT_SCALING) << 8;\
	}

/* call this when "s" has already been scaled (e.g. when dithering)
 */

#define float_24u32_scaled(s, d)\
        if ((s) <= SAMPLE_24BIT_MIN_F) {\
		(d) = SAMPLE_24BIT_MIN << 8;\
	} else if ((s) >= SAMPLE_24BIT_MAX_F) {	\
		(d) = SAMPLE_24BIT_MAX << 8;		\
	} else {\
		(d) = f_round ((s)) << 8; \
	}

#define float_24(s, d) \
	if ((s) <= NORMALIZED_FLOAT_MIN) {\
		(d) = SAMPLE_24BIT_MIN;\
	} else if ((s) >= NORMALIZED_FLOAT_MAX) {\
		(d) = SAMPLE_24BIT_MAX;\
	} else {\
		(d) = f_round ((s) * SAMPLE_24BIT_SCALING);\
	}

/* call this when "s" has already been scaled (e.g. when dithering)
 */

#define float_24_scaled(s, d)\
        if ((s) <= SAMPLE_24BIT_MIN_F) {\
		(d) = SAMPLE_24BIT_MIN;\
	} else if ((s) >= SAMPLE_24BIT_MAX_F) {	\
		(d) = SAMPLE_24BIT_MAX;		\
	} else {\
		(d) = f_round ((s)); \
	}


/* Linear Congruential noise generator. From the music-dsp list
 * less random than rand(), but good enough and 10x faster 
 */

inline unsigned int fast_rand() {
	static unsigned int seed = 22222;
	seed = (seed * 96314165) + 907633515;

	return seed;
}


/* functions for native float sample data */

void sample_move_floatLE_sSs (jack_default_audio_sample_t *dst, char *src, unsigned long nsamples, unsigned long src_skip) {
	while (nsamples--) {
		*dst = *((float *) src);
		dst++;
		src += src_skip;
	}
}

void sample_move_dS_floatLE (char *dst, jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state) {
	while (nsamples--) {
		*((float *) dst) = *src;
		dst += dst_skip;
		src++;
	}
}

/* NOTES on function naming:

   foo_bar_d<TYPE>_s<TYPE>

   the "d<TYPE>" component defines the destination type for the operation
   the "s<TYPE>" component defines the source type for the operation

   TYPE can be one of:
   
   S      - sample is a jack_default_audio_sample_t, currently (October 2008) a 32 bit floating point value
   Ss     - like S but reverse endian from the host CPU
   32u24  - sample is an signed 32 bit integer value, but data is in upper 24 bits only
   32u24s - like 32u24 but reverse endian from the host CPU
   24     - sample is an signed 24 bit integer value
   24s    - like 24 but reverse endian from the host CPU
   16     - sample is an signed 16 bit integer value
   16s    - like 16 but reverse endian from the host CPU

   For obvious reasons, the reverse endian versions only show as source types.

   This covers all known sample formats at 16 bits or larger.
*/   

/* functions for native integer sample data */

void sample_move_d32u24_sSs (char *dst, jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)
{
	int32_t z;

	while (nsamples--) {

		float_24u32 (*src, z);


#if __BYTE_ORDER == __LITTLE_ENDIAN
		dst[0]=(char)(z>>24);
		dst[1]=(char)(z>>16);
		dst[2]=(char)(z>>8);
		dst[3]=(char)(z);
#elif __BYTE_ORDER == __BIG_ENDIAN
		dst[0]=(char)(z);
		dst[1]=(char)(z>>8);
		dst[2]=(char)(z>>16);
		dst[3]=(char)(z>>24);
#endif
		dst += dst_skip;
		src++;
	}
}	

void sample_move_d32u24_sS (char *dst, jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)
{
	while (nsamples--) {
		float_24u32 (*src, *((int32_t*) dst));
		dst += dst_skip;
		src++;
	}
}	

void sample_move_dS_s32u24s (jack_default_audio_sample_t *dst, char *src, unsigned long nsamples, unsigned long src_skip)
{
	/* ALERT: signed sign-extension portability !!! */

	while (nsamples--) {
		int x;
#if __BYTE_ORDER == __LITTLE_ENDIAN
		x = (unsigned char)(src[0]);
		x <<= 8;
		x |= (unsigned char)(src[1]);
		x <<= 8;
		x |= (unsigned char)(src[2]);
		x <<= 8;
		x |= (unsigned char)(src[3]);
#elif __BYTE_ORDER == __BIG_ENDIAN
		x = (unsigned char)(src[3]);
		x <<= 8;
		x |= (unsigned char)(src[2]);
		x <<= 8;
		x |= (unsigned char)(src[1]);
		x <<= 8;
		x |= (unsigned char)(src[0]);
#endif
		*dst = (x >> 8) / SAMPLE_24BIT_SCALING;
		dst++;
		src += src_skip;
	}
}	

void sample_move_dS_s32u24 (jack_default_audio_sample_t *dst, char *src, unsigned long nsamples, unsigned long src_skip)
{
	/* ALERT: signed sign-extension portability !!! */

	while (nsamples--) {
		*dst = (*((int *) src) >> 8) / SAMPLE_24BIT_SCALING;
		dst++;
		src += src_skip;
	}
}	

void sample_move_d24_sSs (char *dst, jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)
{
	int32_t z;

	while (nsamples--) {
		float_24 (*src, z);
#if __BYTE_ORDER == __LITTLE_ENDIAN
		dst[0]=(char)(z>>16);
		dst[1]=(char)(z>>8);
		dst[2]=(char)(z);
#elif __BYTE_ORDER == __BIG_ENDIAN
		dst[0]=(char)(z);
		dst[1]=(char)(z>>8);
		dst[2]=(char)(z>>16);
#endif
		dst += dst_skip;
		src++;
	}
}	

void sample_move_d24_sS (char *dst, jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)
{
        int32_t z;
	
	while (nsamples--) {
		float_24 (*src, z);
#if __BYTE_ORDER == __LITTLE_ENDIAN
		memcpy (dst, &z, 3);
#elif __BYTE_ORDER == __BIG_ENDIAN
		memcpy (dst, (char *)&z + 1, 3);
#endif
		dst += dst_skip;
		src++;
	}
}	

void sample_move_dS_s24s (jack_default_audio_sample_t *dst, char *src, unsigned long nsamples, unsigned long src_skip)
{
	/* ALERT: signed sign-extension portability !!! */

	while (nsamples--) {
		int x;
#if __BYTE_ORDER == __LITTLE_ENDIAN
		x = (unsigned char)(src[0]);
		x <<= 8;
		x |= (unsigned char)(src[1]);
		x <<= 8;
		x |= (unsigned char)(src[2]);
		/* correct sign bit and the rest of the top byte */
		if (src[0] & 0x80) {
			x |= 0xff << 24;
		}
#elif __BYTE_ORDER == __BIG_ENDIAN
		x = (unsigned char)(src[2]);
		x <<= 8;
		x |= (unsigned char)(src[1]);
		x <<= 8;
		x |= (unsigned char)(src[0]);
		/* correct sign bit and the rest of the top byte */
		if (src[2] & 0x80) {
			x |= 0xff << 24;
		}
#endif
		*dst = x / SAMPLE_24BIT_SCALING;
		dst++;
		src += src_skip;
	}
}	

void sample_move_dS_s24 (jack_default_audio_sample_t *dst, char *src, unsigned long nsamples, unsigned long src_skip)
{
	/* ALERT: signed sign-extension portability !!! */

	while (nsamples--) {
		int x;
#if __BYTE_ORDER == __LITTLE_ENDIAN
		memcpy((char*)&x + 1, src, 3);
#elif __BYTE_ORDER == __BIG_ENDIAN
		memcpy(&x, src, 3);
#endif
		x >>= 8;
		*dst = x / SAMPLE_24BIT_SCALING;
		dst++;
		src += src_skip;
	}
}	


void sample_move_d16_sSs (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	int16_t tmp;

	while (nsamples--) {
		// float_16 (*src, tmp);

		if (*src <= NORMALIZED_FLOAT_MIN) {
			tmp = SAMPLE_16BIT_MIN;
		} else if (*src >= NORMALIZED_FLOAT_MAX) {
			tmp = SAMPLE_16BIT_MAX;
		} else {
			tmp = (int16_t) f_round (*src * SAMPLE_16BIT_SCALING);
		}

#if __BYTE_ORDER == __LITTLE_ENDIAN
		dst[0]=(char)(tmp>>8);
		dst[1]=(char)(tmp);
#elif __BYTE_ORDER == __BIG_ENDIAN
		dst[0]=(char)(tmp);
		dst[1]=(char)(tmp>>8);
#endif
		dst += dst_skip;
		src++;
	}
}

void sample_move_d16_sS (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	while (nsamples--) {
		float_16 (*src, *((int16_t*) dst));
		dst += dst_skip;
		src++;
	}
}

void sample_move_dither_rect_d16_sSs (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	jack_default_audio_sample_t val;
	int16_t      tmp;

	while (nsamples--) {
		val = (*src * SAMPLE_16BIT_SCALING) + fast_rand() / (float) UINT_MAX - 0.5f;
		float_16_scaled (val, tmp);
#if __BYTE_ORDER == __LITTLE_ENDIAN
		dst[0]=(char)(tmp>>8);
		dst[1]=(char)(tmp);
#elif __BYTE_ORDER == __BIG_ENDIAN
		dst[0]=(char)(tmp);
		dst[1]=(char)(tmp>>8);
#endif
		dst += dst_skip;
		src++;
	}
}

void sample_move_dither_rect_d16_sS (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	jack_default_audio_sample_t val;

	while (nsamples--) {
		val = (*src * SAMPLE_16BIT_SCALING) + fast_rand() / (float)UINT_MAX - 0.5f;
		float_16_scaled (val, *((int16_t*) dst));
		dst += dst_skip;
		src++;
	}
}

void sample_move_dither_tri_d16_sSs (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	jack_default_audio_sample_t val;
	int16_t      tmp;

	while (nsamples--) {
		val = (*src * SAMPLE_16BIT_SCALING) + ((float)fast_rand() + (float)fast_rand()) / (float)UINT_MAX - 1.0f;
		float_16_scaled (val, tmp);

#if __BYTE_ORDER == __LITTLE_ENDIAN
		dst[0]=(char)(tmp>>8);
		dst[1]=(char)(tmp);
#elif __BYTE_ORDER == __BIG_ENDIAN
		dst[0]=(char)(tmp);
		dst[1]=(char)(tmp>>8);
#endif
		dst += dst_skip;
		src++;
	}
}

void sample_move_dither_tri_d16_sS (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	jack_default_audio_sample_t val;

	while (nsamples--) {
		val = (*src * SAMPLE_16BIT_SCALING) + ((float)fast_rand() + (float)fast_rand()) / (float)UINT_MAX - 1.0f;
		float_16_scaled (val, *((int16_t*) dst));
		dst += dst_skip;
		src++;
	}
}

void sample_move_dither_shaped_d16_sSs (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	jack_default_audio_sample_t     x;
	jack_default_audio_sample_t     xe; /* the innput sample - filtered error */
	jack_default_audio_sample_t     xp; /* x' */
	float        r;
	float        rm1 = state->rm1;
	unsigned int idx = state->idx;
	int16_t      tmp;

	while (nsamples--) {
		x = *src * SAMPLE_16BIT_SCALING;
		r = ((float)fast_rand() + (float)fast_rand())  / (float)UINT_MAX - 1.0f;
		/* Filter the error with Lipshitz's minimally audible FIR:
		   [2.033 -2.165 1.959 -1.590 0.6149] */
		xe = x
		     - state->e[idx] * 2.033f
		     + state->e[(idx - 1) & DITHER_BUF_MASK] * 2.165f
		     - state->e[(idx - 2) & DITHER_BUF_MASK] * 1.959f
		     + state->e[(idx - 3) & DITHER_BUF_MASK] * 1.590f
		     - state->e[(idx - 4) & DITHER_BUF_MASK] * 0.6149f;
		xp = xe + r - rm1;
		rm1 = r;

		float_16_scaled (xp, tmp);

		/* Intrinsic z^-1 delay */
		idx = (idx + 1) & DITHER_BUF_MASK;
		state->e[idx] = xp - xe;

#if __BYTE_ORDER == __LITTLE_ENDIAN
		dst[0]=(char)(tmp>>8);
		dst[1]=(char)(tmp);
#elif __BYTE_ORDER == __BIG_ENDIAN
		dst[0]=(char)(tmp);
		dst[1]=(char)(tmp>>8);
#endif
		dst += dst_skip;
		src++;
	}
	state->rm1 = rm1;
	state->idx = idx;
}

void sample_move_dither_shaped_d16_sS (char *dst,  jack_default_audio_sample_t *src, unsigned long nsamples, unsigned long dst_skip, dither_state_t *state)	
{
	jack_default_audio_sample_t     x;
	jack_default_audio_sample_t     xe; /* the innput sample - filtered error */
	jack_default_audio_sample_t     xp; /* x' */
	float        r;
	float        rm1 = state->rm1;
	unsigned int idx = state->idx;

	while (nsamples--) {
		x = *src * SAMPLE_16BIT_SCALING;
		r = ((float)fast_rand() + (float)fast_rand()) / (float)UINT_MAX - 1.0f;
		/* Filter the error with Lipshitz's minimally audible FIR:
		   [2.033 -2.165 1.959 -1.590 0.6149] */
		xe = x
		     - state->e[idx] * 2.033f
		     + state->e[(idx - 1) & DITHER_BUF_MASK] * 2.165f
		     - state->e[(idx - 2) & DITHER_BUF_MASK] * 1.959f
		     + state->e[(idx - 3) & DITHER_BUF_MASK] * 1.590f
		     - state->e[(idx - 4) & DITHER_BUF_MASK] * 0.6149f;
		xp = xe + r - rm1;
		rm1 = r;

		float_16_scaled (xp, *((int16_t*) dst));

		/* Intrinsic z^-1 delay */
		idx = (idx + 1) & DITHER_BUF_MASK;
		state->e[idx] = *((int16_t*) dst) - xe;

		dst += dst_skip;
		src++;
	}
	state->rm1 = rm1;
	state->idx = idx;
}

void sample_move_dS_s16s (jack_default_audio_sample_t *dst, char *src, unsigned long nsamples, unsigned long src_skip) 	
{
	short z;

	/* ALERT: signed sign-extension portability !!! */
	while (nsamples--) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
		z = (unsigned char)(src[0]);
		z <<= 8;
		z |= (unsigned char)(src[1]);
#elif __BYTE_ORDER == __BIG_ENDIAN
		z = (unsigned char)(src[1]);
		z <<= 8;
		z |= (unsigned char)(src[0]);
#endif
		*dst = z / SAMPLE_16BIT_SCALING;
		dst++;
		src += src_skip;
	}
}	

void sample_move_dS_s16 (jack_default_audio_sample_t *dst, char *src, unsigned long nsamples, unsigned long src_skip) 
	
{
	/* ALERT: signed sign-extension portability !!! */
	while (nsamples--) {
		*dst = (*((short *) src)) / SAMPLE_16BIT_SCALING;
		dst++;
		src += src_skip;
	}
}	

void memset_interleave (char *dst, char val, unsigned long bytes, 
			unsigned long unit_bytes, 
			unsigned long skip_bytes) 
{
	switch (unit_bytes) {
	case 1:
		while (bytes--) {
			*dst = val;
			dst += skip_bytes;
		}
		break;
	case 2:
		while (bytes) {
			*((short *) dst) = (short) val;
			dst += skip_bytes;
			bytes -= 2;
		}
		break;
	case 4:		    
		while (bytes) {
			*((int *) dst) = (int) val;
			dst += skip_bytes;
			bytes -= 4;
		}
		break;
	default:
		while (bytes) {
			memset(dst, val, unit_bytes);
			dst += skip_bytes;
			bytes -= unit_bytes;
		}
		break;
	}
}

/* COPY FUNCTIONS: used to move data from an input channel to an
   output channel. Note that we assume that the skip distance
   is the same for both channels. This is completely fine
   unless the input and output were on different audio interfaces that
   were interleaved differently. We don't try to handle that.
*/

void 
memcpy_fake (char *dst, char *src, unsigned long src_bytes, unsigned long foo, unsigned long bar)
{
	memcpy (dst, src, src_bytes);
}

void 
memcpy_interleave_d16_s16 (char *dst, char *src, unsigned long src_bytes,
			   unsigned long dst_skip_bytes, unsigned long src_skip_bytes)
{
	while (src_bytes) {
		*((short *) dst) = *((short *) src);
		dst += dst_skip_bytes;
		src += src_skip_bytes;
		src_bytes -= 2;
	}
}

void 
memcpy_interleave_d24_s24 (char *dst, char *src, unsigned long src_bytes,
			   unsigned long dst_skip_bytes, unsigned long src_skip_bytes)

{
	while (src_bytes) {
		memcpy(dst, src, 3);
		dst += dst_skip_bytes;
		src += src_skip_bytes;
		src_bytes -= 3;
	}
}

void 
memcpy_interleave_d32_s32 (char *dst, char *src, unsigned long src_bytes,
			   unsigned long dst_skip_bytes, unsigned long src_skip_bytes)

{
	while (src_bytes) {
		*((int *) dst) = *((int *) src);
		dst += dst_skip_bytes;
		src += src_skip_bytes;
		src_bytes -= 4;
	}
}

