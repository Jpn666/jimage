/*
 * Copyright (C) 2021, jpn 
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


/* Progressive pass count limit */
#define JPGR_MAXPASSES 100


/* Error codes */
typedef enum {
	JPGR_OK             = 0,
	JPGR_EBADSTATE      = 1,
	JPGR_EBADFILE       = 2,
	JPGR_EINVALIDIMAGE  = 3,
	JPGR_ENOSUPPORTED   = 4,
	JPGR_EBADDATA       = 5,
	JPGR_EIOERROR       = 6,
	JPGR_EBADHMTABLE    = 7,
	JPGR_ETABLEID       = 8,
	JPGR_ENOHMTABLE     = 9,    /* missing huffman table */
	JPGR_ENOQTTABLE     = 10,   /* missing quantization table */
	JPGR_EBADCODE       = 11,
	JPGR_EINVALIDPASS   = 12,
	JPGR_ESEGMENTORDER  = 13,
	JPGR_ENOSEGMENT     = 14,   /* missing segment */
	JPGR_EBADUSE        = 15,
	JPGR_ELIMIT         = 16,
	JPGR_EPASSLIMIT     = 17,
	JPGR_EOOM           = 18
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
struct TJPGRPblc {
	uintxx state;
	uintxx error;
	uintxx warnings;       /* non fatal errors */
	uintxx flags;
	
	/* image size */
	uint16 sizex;
	uint16 sizey;
	
	/* properties */
	uintxx cmode;
	uintxx isprogressive;
	
	/* */
	uintxx mayorversion;
	uintxx minorversion;
	uintxx xdensity;
	uintxx ydensity;
	uintxx unit;
	
	/* ICC profile */
	uint8* iccprofile;
	uintxx iccpsize;
};

typedef const struct TJPGRPblc TJPGReader;


/*
 * */
TJPGReader* jpgr_create(eJPGRFlags flags);

/*
 * Destroys (and deallocates) the given JPG reader. */
void jpgr_destroy(TJPGReader*);

/*
 * Resets the reader. */
void jpgr_reset(TJPGReader*);

/*
 * Sets the input function used to read the image data. */
void jpgr_setinputfn(TJPGReader*, TIMGInputFn fn, void* user);

/* 
 * Init the decoder and determines the required internal memory nedeed
 * to decode the image. */
bool jpgr_initdecoder(TJPGReader*, TImageInfo* info);

/*
 * Sets the internal working memory and the target memory buffer for the
 * decoded image (the complete image). */
void jpgr_setbuffers(TJPGReader*, uint8* memory, uint8* pixels);

/*
 * Decodes the image to the image buffer (if set). */
uintxx jpgr_decodeimg(TJPGReader*);

/*
 * */
uintxx jpgr_decodepass(TJPGReader*, bool update);

/*
 * */
void jpgr_updateimg(TJPGReader*);

/*
 * */
CTB_INLINE bool jpgr_isprogressive(TJPGReader*);


/* Non fatal errors */
typedef enum {
	JPGR_BADSIGNATURE = 0x01,
	JPGR_BADVERSION   = 0x02,
	JPGR_BADICCP      = 0x04,
	JPGR_SEGMENTORDER = 0x08
} eJPGRWarning;

/*
 * */
CTB_INLINE eJPGRState jpgr_getstate(TJPGReader*, uintxx* error, uintxx* wrnns);


/*
 * Inlines */

CTB_INLINE bool
jpgr_isprogressive(TJPGReader* jpgr)
{
	ASSERT(jpgr);

	return (bool) jpgr->isprogressive;
}

CTB_INLINE eJPGRState
jpgr_getstate(TJPGReader* jpgr, uintxx* error, uintxx* wrnns)
{
	ASSERT(jpgr);
	
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
