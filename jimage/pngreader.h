/*
 * Copyright (C) 2023, jpn
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef f29e6c7b_7a64_42d3_ba69_97933a01d8db
#define f29e6c7b_7a64_42d3_ba69_97933a01d8db

/*
 * pngreader.h
 * A small PNG image loader.
 *
 * ...
 */

#include "imageinfo.h"
#include <ctoolbox/memory.h>


/* Flag to toggle crc32 checksum check */
/* #define PNGR_CFG_DOCRC */


/* Error codes */
typedef enum {
	PNGR_OK               = 0,
	PNGR_EINCORRECTUSE    = 1,
	PNGR_EIOERROR         = 2,
	PNGR_EOOM             = 3,
	PNGR_EBADSTATE        = 4,
	PNGR_EINVALIDIMAGE    = 5,
	PNGR_ELIMIT           = 6,
	PNGR_EBADDATA         = 7,
	PNGR_EBADFILE         = 8,

	/* Specific errors */
	PNGR_EDEFLATE         = 10,
	PNGR_EBADCRC          = 11,
	PNGR_EMISSINGCHUNK    = 12,
	PNGR_EDUPLICATEDCHUNK = 13,
	PNGR_ECHUNKORDER      = 14,
} ePNGRError;


/* Flags */
typedef enum {
	PNGR_IGNOREICCP = 0x01,
	PNGR_NOCRCCHECK = 0x02
} ePNGRFlags;


/* State */
typedef enum {
	PNGR_ABORTED  = -3,
	PNGR_DECODING = -2,
	PNGR_READY    = -1,
	PNGR_NOTSET   =  0,
	PNGR_DECODED  =  1,
	PNGR_DECODEDWITHERROR = 2
} ePNGRState;


#define PNGR_BADSTATE 0xDEADBEEF


/* Public struct */
struct TPNGRPblc {
	/* state */
	uintxx state;
	uintxx flags;
	uintxx error;
	uintxx warnings;       /* non fatal errors */

	/* image size */
	uint32 sizex;
	uint32 sizey;

	uintxx colortype;
	uintxx depth;

	/* internal memory required for the decoder */
	uintxx requiredmemory;

	/* PNG file header */
	uint8 compression;
	uint8 filter;
	uint8 interlace;

	/* PNG image chunk map properties */
	uintxx properties;

	/* RGBA color palette */
	uintxx palettesize;
	uint8  palette[1024];

	/* transparency color key */
	uint16 alpha[3];       /* alpha key RGB or grayscale */

	/* background color */
	uint16 background[3];  /* RGB or grayscale */

	/* significant bits grayscale+alpha or RGB+alpha (in the same order) */
	uint8 sbits[4];

	/* color management */
	float32 gamma;

	/* primary chromaticities and white point */
	float32 wpointx;
	float32 wpointy;
	float32 chromax[3];    /* RGB */
	float32 chromay[3];    /* RGB */

	/* sRGB */
	uintxx srgbintent;

	/* ICC profile */
	uint8  iccpname[80];
	uint8* iccprofile;
	uintxx iccpsize;
	uint32 iccpchecksum;   /* adler32 checksum */

	/* physical dimensions */
	uint32 physx;
	uint32 physy;
	uint8  physunit;
};

typedef const struct TPNGRPblc TPNGReader;


/*
 * */
TPNGReader* pngr_create(ePNGRFlags flags, TAllocator* allctr);

/*
 * Destroys (and deallocates) the given PNG reader. */
void pngr_destroy(TPNGReader*);

/*
 * Resets the reader. */
void pngr_reset(TPNGReader*);

/*
 * Sets the input function. */
void pngr_setinputfn(TPNGReader*, TIMGInputFn fn, void* user);

/*
 * Init the decoder and determines the required internal memory nedeed
 * to decode the image. */
uintxx pngr_initdecoder(TPNGReader*, TImageInfo* info);

/*
 * Sets the target memory buffer for the decoded image and the index buffer
 * for indexed images, both (the pixel buffer and the index buffer) can be
 * NULL. */
void pngr_setbuffers(TPNGReader*, uint8* pixels, uint8* idxs);

/*
 * Decodes the next pass of a progressive image, returns the next pass or zero
 * is there are not more passes or in case of error. */
uintxx pngr_decodepass(TPNGReader*);

/*
 * Decodes the image to the image buffer (if set) or to the index buffer if
 * the index buffer is set and the image is indexed. */
uintxx pngr_decodeimg(TPNGReader*);

/*
 * */
CTB_INLINE bool pngr_isprogressive(TPNGReader*);

/*
 * */
CTB_INLINE bool pngr_isindexed(TPNGReader*);


/* chunks ids */
typedef enum {
	PNGR_TRNS = 0x01,
	PNGR_BKGD = 0x02,
	PNGR_SBIT = 0x04,
	PNGR_GAMA = 0x08,
	PNGR_SRGB = 0x10,
	PNGR_ICCP = 0x20,
	PNGR_CHRM = 0x40,
	PNGR_PHYS = 0x80
} eTPNGRChunk;

/*
 * */
CTB_INLINE bool pngr_haspropertyof(TPNGReader*, eTPNGRChunk chunks);


/* Non fatal errors */
typedef enum {
	PNGR_BADGAMA = 0x01,
	PNGR_BADSBIT = 0x02,
	PNGR_BADICCP = 0x04,
	PNGR_BADPHYS = 0x08,
	PNGR_BADSRGB = 0x10,
	PNGR_BADCHRM = 0x20
} ePNGRWarning;

/*
 * */
CTB_INLINE ePNGRState pngr_getstate(TPNGReader*, uintxx* error, uintxx* wrnns);


/*
 * Inlines */

CTB_INLINE bool
pngr_isprogressive(TPNGReader* pngr)
{
	CTB_ASSERT(pngr);

	return pngr->interlace;
}

CTB_INLINE bool
pngr_isindexed(TPNGReader* pngr)
{
	CTB_ASSERT(pngr);

	return pngr->colortype == 3;
}

CTB_INLINE bool
pngr_haspropertyof(TPNGReader* pngr, eTPNGRChunk chunks)
{
	CTB_ASSERT(pngr);

	return (pngr->properties & chunks) != 0;
}

CTB_INLINE ePNGRState
pngr_getstate(TPNGReader* pngr, uintxx* error, uintxx* wrnns)
{
	CTB_ASSERT(pngr);

	if (wrnns)
		wrnns[0] = pngr->warnings;
	if (error)
		error[0] = pngr->error;

	switch (pngr->state) {
		case 0: return PNGR_NOTSET;
		case 1:
		case 2: return PNGR_READY;
		case 3: return PNGR_DECODING;
		case 4: return PNGR_DECODED;
		case 5: return PNGR_DECODEDWITHERROR;
	}

	return PNGR_ABORTED;
}

#endif
