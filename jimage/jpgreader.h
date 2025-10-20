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

#ifndef dca5f1fa_265a_4ba4_94c0_e1c65deb7603
#define dca5f1fa_265a_4ba4_94c0_e1c65deb7603

/*
 * jpgreader.h
 * A JPEG file loader.
 */

#include "imageinfo.h"
#include <ctoolbox/memory.h>


/* Progressive pass count limit */
#define JPGR_MAXPASSES 100


/* Error codes */
typedef enum {
	JPGR_OK             = 0,
	JPGR_EINCORRECTUSE  = 1,
	JPGR_EIOERROR       = 2,
	JPGR_EOOM           = 3,
	JPGR_EBADSTATE      = 4,
	JPGR_EINVALIDIMAGE  = 5,
	JPGR_ELIMIT         = 6,
	JPGR_EBADDATA       = 7,
	JPGR_EBADFILE       = 8,

	/* Specific errors */
	JPGR_ENOSUPPORTED   = 10,
	JPGR_EBADHMTABLE    = 11,
	JPGR_ETABLEID       = 12,
	JPGR_ENOHMTABLE     = 13,   /* missing huffman table */
	JPGR_ENOQTTABLE     = 14,   /* missing quantization table */
	JPGR_EBADCODE       = 15,
	JPGR_EINVALIDPASS   = 16,
	JPGR_ESEGMENTORDER  = 17,
	JPGR_ENOSEGMENT     = 18,   /* missing segment */
	JPGR_EPASSLIMIT     = 19,
} eJPGRError;


/* Flags */
typedef enum {
	JPGR_IGNOREICCP = 0x01,
	JPGR_KEEPYCBCR  = 0x02
} eJPGRFlags;


/* State */
typedef enum {
	JPGR_ABORTED  = -3,
	JPGR_DECODING = -2,
	JPGR_READY    = -1,
	JPGR_NOTSET   =  0,
	JPGR_DECODED  =  1,
	JPGR_DECODEDWITHERROR = 2
} eJPGRState;


#define JPGR_BADSTATE 0xDEADBEEF


/* Public struct */
struct TJPGReader {
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

	/* properties */
	uintxx isprogressive;

	/* jpeg version and density */
	uintxx majorversion;
	uintxx minorversion;
	uintxx xdensity;
	uintxx ydensity;
	uintxx unit;

	/* image component sampling */
	uint8 vsampling[4];
	uint8 hsampling[4];

	/* ICC profile */
	uint8* iccprofile;
	uintxx iccpsize;
};

typedef struct TJPGReader TJPGReader;


/*
 * */
JIMAGE_API
const TJPGReader* jpgr_create(eJPGRFlags flags, const TAllocator*);

/*
 * Destroys (and deallocates) the given JPG reader. */
JIMAGE_API
void jpgr_destroy(const TJPGReader*);

/*
 * Resets the reader. */
JIMAGE_API
void jpgr_reset(const TJPGReader*);

/*
 * Sets the input function used to read the image data. */
JIMAGE_API
void jpgr_setinputfn(const TJPGReader*, TIMGInputFn fn, void* user);

/*
 * Init the decoder and determines the required internal memory nedeed
 * to decode the image. */
JIMAGE_API
bool jpgr_initdecoder(const TJPGReader*, TImageInfo* info);

/*
 * Sets the target memory buffer for the decoded image (the complete image). */
JIMAGE_API
void jpgr_setbuffers(const TJPGReader*, uint8* pixels);

/*
 * Decodes the image to the image buffer (if set). */
JIMAGE_API
uintxx jpgr_decodeimg(const TJPGReader*);

/*
 * */
JIMAGE_API
uintxx jpgr_decodepass(const TJPGReader*, bool update);

/*
 * */
JIMAGE_API
void jpgr_updateimg(const TJPGReader*);

/*
 * */
CTB_INLINE
bool jpgr_isprogressive(const TJPGReader*);


/* Non fatal errors */
typedef enum {
	JPGR_BADSIGNATURE = 0x01,
	JPGR_BADVERSION   = 0x02,
	JPGR_BADICCP      = 0x04,
	JPGR_SEGMENTORDER = 0x08
} eJPGRWarning;

/*
 * */
CTB_INLINE
eJPGRState jpgr_getstate(const TJPGReader*, uintxx* error, uintxx* wrnns);


/*
 * Inlines */

CTB_INLINE bool
jpgr_isprogressive(const TJPGReader* jpgr)
{
	CTB_ASSERT(jpgr);

	return (bool) jpgr->isprogressive;
}

CTB_INLINE eJPGRState
jpgr_getstate(const TJPGReader* jpgr, uintxx* error, uintxx* wrnns)
{
	CTB_ASSERT(jpgr);

	if (wrnns)
		wrnns[0] = jpgr->warnings;
	if (error)
		error[0] = jpgr->error;

	switch (jpgr->state) {
		case 0: return JPGR_NOTSET;
		case 1:
		case 2: return JPGR_READY;
		case 3: return JPGR_DECODING;
		case 4: return JPGR_DECODED;
		case 5: return JPGR_DECODEDWITHERROR;
	}

	return JPGR_ABORTED;
}

#endif
