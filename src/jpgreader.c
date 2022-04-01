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

/* Code based on IJG JPEG library, turbo-JPEG library and STB Image (by
 * Sean Barrett). The book Compressed File Formats by John Miano was used as
 * reference. */

#include "../jpgreader.h"


/* segment markers */
#define SOI  0xffd8
#define EOI  0xffd9

#define APP0 0xffe0
#define APP1 0xffe1
#define APP2 0xffe2
#define APP3 0xffe3
#define APP4 0xffe4
#define APP5 0xffe5
#define APP6 0xffe6
#define APP7 0xffe7
#define APP8 0xffe8
#define APP9 0xffe9
#define APPA 0xffea
#define APPB 0xffeb
#define APPC 0xffec
#define APPD 0xffed
#define APPE 0xffee
#define APPF 0xffef

#define DQT  0xffdb
#define DHT  0xffc4

#define SOF0 0xffc0
#define SOF1 0xffc1
#define SOF2 0xffc2
#define SOF3 0xffc3
#define SOF5 0xffc5
#define SOF6 0xffc6
#define SOF7 0xffc7
#define SOF9 0xffc9
#define SOFA 0xffca
#define SOFB 0xffcb
#define SOFD 0xffcd
#define SOFE 0xffce
#define SOFF 0xffcf

#define DRI  0xffdd
#define SOS  0xffda

#define DNL  0xffdc

/* */
#define RST0 0xffd0
#define RST1 0xffd1
#define RST2 0xffd2
#define RST3 0xffd3
#define RST4 0xffd4
#define RST5 0xffd5
#define RST6 0xffd6
#define RST7 0xffd7


/* 255 posible chunks of 65519 bytes */
#define MAXICCPSIZE 0xfeef11


/* direct decoding table size */
#define ROOTBITS 9

#if ROOTBITS ==  8
	#define ENOUGH_DC 384  /*  16 8 */
	#define ENOUGH_AC 630  /* 256 8 */
#endif
#if ROOTBITS ==  9
	#define ENOUGH_DC 576  /*  16 9 */
	#define ENOUGH_AC 822  /* 256 9 */
#endif


/* NOTE: we use diferents structs for huffman decoding because the size of
 * the tables is diferent, for DC tables we have 16 symbols and 256 for AC
 * tables. */

/* Dummy table (only used as pointer), layout must be the same
 * for all the tables. */
struct TJPGHmTable {
	uintxx defined;
	
	/* symbol or offset (11 bits) | length (5 bits) */
	uint16 symbols[1];
};


/* DC huffman table */
struct TJPGDCHmTable {
	uintxx defined;
	uint16 symbols[ENOUGH_DC];
};


/* AC huffman table */
struct TJPGACHmTable {
	uintxx defined;
	uint16 symbols[ENOUGH_AC];

	/* combined table contaning extended values and the length in bits of the
	 * symbol + symbol bits */
	int16 sextent[1 << ROOTBITS];
};


/* Quantization table */
struct TJPGQnTable {
	uintxx defined;
	
	/* quantization values */
	int16* values;
	uint8 storage[64 * sizeof(int16) + 16]; /* for alignment to 16 */
};


/* bit prefecth buffer size */
#if defined(CTB_ENV64)
#	define BPREFETCHBZ 32
#else
#	define BPREFETCHBZ 16
#endif

/* bit buffer type */
#if defined(CTB_ENV64)
	#define BBTYPE uint64
#else
	#define BBTYPE uint32
#endif


/* Private stuff */
struct TJPGRPrvt {
	/* public fields */
	struct TJPGRPblc hidden;
	
	uintxx ysampling;
	uintxx xsampling;
	uint32 issubsampled;
	
	/* scan size in number of MCU and the total number of units */
	uintxx nrows;
	uintxx ncols;

	uintxx nunits;
	
	/* component in the scan (if it's a single scan) and number of components
	 * in the scan */
	uintxx nscancomponents;
	uintxx scancomponent;
	
	/* component order */
	uintxx corder[3];

	/* color transform flags */
	uint32 isrgb;
	uint32 keepyuv;

	/* number of components in the image */
	uint32 ncomponents;
	
	/* to ensure segment order */
	struct TJPGRSegmentMap {
		uintxx APP0s: 1;
		uintxx SOFXs: 1;
		uintxx  SOSs: 1;
	}
	segmentmap;
	
	/* */
	uint32 isinterleaved;
	
	/* progressive parameters */
	uint32 al;  /* sucessive approximation */
	uint32 ah;
	
	uint32 ss;  /* spectral selection start and end */
	uint32 se;
	intxx  eobrun;
	
	/* current pass in a progressive image */
	uint32 npass;
	
	/* restart interval */
	uint32 rinterval;
	
	/* to map MCU to the final image */
	uint8 originy[16];
	uint8 originx[16];
	
	/* decoded image data */
	uint8* pixels;
	
	/* allocated memory for the ICC profile (if any) */
	uint8* iccpmemory;
	uint8* iccpappend;
	uintxx iccpsize;

	/* used to read the profile */
	uintxx iccpmode;
	uintxx iccptotal;
	uint8  iccps1;
	uint8  iccps2;

	/* input callback */
	TIMGInputFn inputfn;
	
	/* input callback parameter */
	void* payload;
	
	/* bit buffer */
	BBTYPE bbuffer;
	uintxx bbcount;
	intxx  bbcread;
	uintxx bend;
	
	/* bit prefecth buffer */
	uint16 bb[BPREFETCHBZ];
	uint32 bindex;
	
	/* flag used to indicate the end of the input */
	uint32 endofinput;
	
	/* input handling */
	uint8* bgn;
	uint8* end;
	uint8* sourceend;
	
	/* input buffer */
	uint8 source[4096];

	/* Components */
	struct TJPGComponent {
		uint32 ysampling;
		uint32 xsampling;
		
		/* component ID */
		uint32 id;

		/* size without padding (for non-interleaved decoding) */
		uintxx nrows;
		uintxx ncols;
		
		/* size with padding */
		uintxx irows;
		uintxx icols;
		
		/* upsample map */
		const uint8* umap;
		
		/* block index and upsample offset for each block in the component */
		uint8 iblock[16];
		uint8 offset[16];

		/* row upsample mode
		 * x1=0
		 * x2=1 x2+4=2
		 * x4=3 x4+2=4 x4+4=5 x4+6=6 */
		uint8 rumode[16];

		/* huffman tables */
		struct TJPGDCHmTable* dctable;
		struct TJPGACHmTable* actable;
		
		/* quantization table */
		struct TJPGQnTable* qtable;
		
		intxx cofficient;
		
		/* complete component scan units in a non-interleaved or progressive
		 * image */
		int16* scan;
		
		/* scan units */
		int16* units[8];

		/* used for upsampling */
		int16* srow;
		
		/* number of units in the scan or component */
		uintxx ucount;
	}
	components[3];
	
	/* huffman tables */
	struct TJPGDCHmTable dctables[4];
	struct TJPGACHmTable actables[4];
	
	/* quantization tables */
	struct TJPGQnTable qtables[4];
};


/* private and public cast, we only need to use PBLC to set values, only in the
 * public functions */
#define PBLC ((struct TJPGRPblc*) jpgr)
#define PRVT ((struct TJPGRPrvt*) jpgr)


TJPGReader*
jpgr_create(eJPGRFlags flags)
{
	uintxx i;
	struct TJPGRPblc* jpgr;
	
	jpgr = CTB_MALLOC(sizeof(struct TJPGRPrvt));
	if (jpgr == NULL) {
		return NULL;
	}
	
	/* align the quantization tables to 16 (we need this to use SIMD) */
	for (i = 0; i < 4; i++) {
		struct TJPGQnTable* qtable;
		qtable = PRVT->qtables + i;
		
		qtable->values = (void*) ((((uintxx) qtable->storage) | 15) + 1);
	}
	
	PRVT->iccpmemory = NULL;
	jpgr_reset(jpgr);
	
	PBLC->flags = flags;
	return jpgr;
}


#define BUFFERSIZE (sizeof(((struct TJPGRPrvt*) NULL)->source))

void
jpgr_reset(TJPGReader* jpgr)
{
	uintxx i;
	struct TJPGComponent* c;
	ASSERT(jpgr);
	
	/* public fields */
	PBLC->state = 0;
	PBLC->error = 0;
	
	PBLC->sizex = 0;
	PBLC->sizey = 0;
	
	PBLC->colortype = 0;
	PBLC->depth     = 0;
	PBLC->requiredmemory = 0;
	PBLC->isprogressive  = 0;

	PBLC->mayorversion = 0;
	PBLC->minorversion = 0;
	PBLC->xdensity = 0;
	PBLC->ydensity = 0;
	PBLC->unit = 0;

	PBLC->iccprofile = NULL;
	PBLC->iccpsize   = 0;
	
	/* private fields */
	PRVT->ncomponents   = 0;
	PRVT->isinterleaved = 0;
	PRVT->issubsampled  = 0;

	if (PRVT->iccpmemory == NULL) {
		PRVT->iccpsize = 0;
	}
	PRVT->iccpappend = NULL;
	PRVT->iccpmode  = 0;
	PRVT->iccptotal = 0;
	PRVT->iccps1 = 0;
	PRVT->iccps2 = 0;

	PRVT->ysampling = 0;
	PRVT->xsampling = 0;
	PRVT->nrows  = 0;
	PRVT->ncols  = 0;
	PRVT->nunits = 0;
	
	PRVT->pixels = NULL;
	
	PRVT->al = 0;
	PRVT->ah = 0;
	PRVT->ss = 0;
	PRVT->se = 0;
	PRVT->eobrun = 0;
	PRVT->npass  = 0;
	PRVT->rinterval   = 0;
		
	PRVT->inputfn = NULL;
	PRVT->payload = NULL;

	PRVT->isrgb   = 0;
	PRVT->keepyuv = 0;
	for (i = 0; i < 3; i++) {
		c = PRVT->components + i;
		
		c->dctable = NULL;
		c->actable = NULL;
		c->qtable  = NULL;
		
		c->ysampling = 0;
		c->xsampling = 0;
		
		c->ncols = 0;
		c->nrows = 0;
		c->cofficient = 0;
		c->id = -1;
	}
	
	for (i = 0; i < 4; i++) {
		PRVT->qtables[i].defined = 0;

		PRVT->dctables[i].defined = 0;
		PRVT->actables[i].defined = 0;
	}

	PRVT->segmentmap = (struct TJPGRSegmentMap) {0};
	
	PRVT->sourceend = PRVT->source + BUFFERSIZE;
	PRVT->bgn = PRVT->source;
	PRVT->end = PRVT->source;
	PRVT->endofinput = 0;
}

void
jpgr_destroy(TJPGReader* jpgr)
{
	if (jpgr) {
		if (PRVT->iccpmemory != NULL) {
			CTB_FREE(PRVT->iccpmemory);
		}
		CTB_FREE(PBLC);
	}
}

#define SETERROR(ERROR) (PBLC->error = (ERROR))
#define SETSTATE(STATE) (PBLC->state = (STATE))

void
jpgr_setinputfn(TJPGReader* jpgr, TIMGInputFn fn, void* user)
{
	ASSERT(jpgr);
	
	if (jpgr->state != 0) {
		SETERROR(JPGR_EBADUSE);
		SETSTATE(JPGR_EBADSTATE);
		return;
	}
	PRVT->inputfn = fn;
	PRVT->payload = user;
}


/*
 * Input handling functions */

CTB_INLINE uintxx
readmore(struct TJPGRPblc* jpgr, uintxx avaible, uintxx amount)
{
	uintxx remaining;
	intxx r;
	
	remaining = (uintxx) (PRVT->sourceend - PRVT->end);
	if (LIKELY(remaining + avaible < amount)) {
		if (avaible) {
			memmove(PRVT->source, PRVT->bgn, avaible);
		}
		
		PRVT->bgn = PRVT->source;
		PRVT->end = PRVT->source + avaible;
			
		remaining = BUFFERSIZE - avaible;
	}
	
	if (PRVT->endofinput) {
		return avaible;
	}
	
	r = PRVT->inputfn(PRVT->end, remaining, PRVT->payload);
	if (LIKELY(r > 0)) {
		avaible   += r;
		PRVT->end += r;
	}
	else {
		PRVT->endofinput = 1;
		if (r != 0) {
			SETERROR(JPGR_EIOERROR);
			return 0;
		}
	}
	
	return avaible;
}

CTB_INLINE bool
ensurebytes(struct TJPGRPblc* jpgr, uintxx amount)
{
	uintxx avaible;
	
	avaible = (uintxx) (PRVT->end - PRVT->bgn);
	if (UNLIKELY(avaible < amount)) {
		avaible = readmore(jpgr, avaible, amount);
	}
	
	if (avaible >= amount) {
		return 1;
	}
	return 0;
}

CTB_INLINE void
consumebytes(struct TJPGRPblc* jpgr, uintxx amount)
{
	PRVT->bgn += amount;
}

CTB_INLINE void
skipbytes(struct TJPGRPblc* jpgr, uintxx amount)
{
	uintxx r;
	
	while (amount) {
		r = amount;
		if (r > 256)
			r = 256;
		
		if (ensurebytes(jpgr, r) == 0) {
			break;
		}
		consumebytes(jpgr, r);
		amount -= r;
	}
}

CTB_INLINE uint8*
readinput(struct TJPGRPblc* jpgr, uintxx amount)
{
	uint8* s;
	
	if (LIKELY(ensurebytes(jpgr, amount))) {
		s = PRVT->bgn;
		consumebytes(jpgr, amount);
		return s;
	}
	return NULL;
}


#define TOI32(A, B, C, D) ((A << 0x18) | (B << 0x10) | (C << 0x08) | (D))
#define TOI16(A, B)       ((A << 0x08) | (B))

CTB_INLINE uint16
read16(struct TJPGRPblc* jpgr)
{
	uint8* s;
	
	s = readinput(jpgr, 2);
	if (UNLIKELY(s == NULL)) {
		return 0;
	}
	return TOI16(s[0], s[1]);
}

CTB_INLINE uint16
readmarker(struct TJPGRPblc* jpgr)
{
	uint8* s;
	
	if ((s = readinput(jpgr, 1)) == NULL) {
		return 0;
	}
	
	if (s[0] == 0xff) {
		do {
			s = readinput(jpgr, 1);
			if (UNLIKELY(s == NULL)) {
				return 0;
			}
		} while (s[0] == 0xff);
		
		return TOI16(0xff, s[0]);
	}
	return s[0];
}


static uintxx parseAPP0(struct TJPGRPblc* jpgr);
static uintxx parseAPP2(struct TJPGRPblc* jpgr);
static uintxx parseSOF0(struct TJPGRPblc* jpgr, uintxx progressive);

static uintxx parseSOS(struct TJPGRPblc* jpgr);
static uintxx parseDQT(struct TJPGRPblc* jpgr);
static uintxx parseDHT(struct TJPGRPblc* jpgr);
static uintxx parseDRI(struct TJPGRPblc* jpgr);

static uintxx
parsesegments(struct TJPGRPblc* jpgr)
{
	uint16 m;
	
	for (;;) {
		m = readmarker(jpgr);
		if (m == EOI) {
			if (jpgr->state != 3) {
				/* premature end of file */
				SETERROR(JPGR_EBADDATA);
				SETSTATE(JPGR_BADSTATE);
				return 0;
			}

			SETSTATE(4);
			break;
		}
		
		switch (m) {
			case APP0:
				if (parseAPP0(jpgr) == 0) {
					return 0;
				}
				continue;
			
			/* ICCP */
			case APP2:
				if ((jpgr->flags & JPGR_IGNOREICCP) == 0) {
					if (parseAPP2(jpgr) == 0) {
						return 0;
					}
					continue;
				}
				break;

			case DQT:
				if (parseDQT(jpgr) == 0) {
					return 0;
				}
				continue;
			
			case SOF2:
			case SOF1:
			case SOF0:
				if (parseSOF0(jpgr, (m == SOF2)) == 0) {
					return 0;
				}
				continue;
			
			/* not supported markers */
			case SOF3:
			case SOF5:
			case SOF6:
			case SOF7:
				SETERROR(JPGR_ENOSUPPORTED);
				return 0;
			
			case DHT:
				if (parseDHT(jpgr) == 0) {
					return 0;
				}
				continue;
			
			case DRI:
				if (parseDRI(jpgr) == 0) {
					return 0;
				}
				continue;
			
			case SOS:
				if (parseSOS(jpgr) == 0) {
					return 0;
				}
				return 1;
		}
		
		if (((m >= 0xffd0 && m <= 0xffd9) || m == 0xff01) == 0) {
			uint16 r;
			
			r = read16(jpgr);
			if (r < 2) {
				return 0;
			}
			r -= 2;
			
			skipbytes(jpgr, r);
			if (jpgr->error) {
				return 0;
			}
		}
		
		/* bad marker value */
		if (((m >> 8) ^ 0xff) != 0) {
			return 0;
		}
	}
	
	return 1;
}


#define ADDWARNING(W) (jpgr->warnings |= (W))


#define JFIFID 0x4a464946
#define JFXXID 0x4a465858

static uintxx
parseAPP0(struct TJPGRPblc* jpgr)
{
	uint16 r;
	uint32 signature;
	uint8* s;
	
	r = read16(jpgr);
	if (r < 1) {
		return 0;
	}
	r -= 2;

	if (PRVT->segmentmap.APP0s == 1) {
		ADDWARNING(JPGR_SEGMENTORDER);
		goto L_SKIP;
	}
	PRVT->segmentmap.APP0s = 1;
	
	if (PRVT->segmentmap.SOFXs == 1) {
		ADDWARNING(JPGR_SEGMENTORDER);
	}

	if (r < 5 || (s = readinput(jpgr, 5)) == NULL ) {
		return 0;
	}
	r -= 5;
	signature = TOI32(s[0], s[1], s[2], s[3]);
	if (signature != JFIFID && signature != JFXXID) {
		ADDWARNING(JPGR_BADSIGNATURE);
		goto L_SKIP;
	}
	
	if (r < 7 || (s = readinput(jpgr, 7)) == NULL ) {
		return 0;
	}
	r -= 7;
	jpgr->mayorversion = s[0];
	jpgr->minorversion = s[1];
	if (jpgr->mayorversion != 1) {
		ADDWARNING(JPGR_BADVERSION);
		goto L_SKIP;
	}
	
	/* density units */
	s += 2;
	jpgr->unit = s[0];
	jpgr->ydensity = TOI16(s[1], s[2]);
	jpgr->xdensity = TOI16(s[3], s[4]);
	
L_SKIP:
	if (r) {
		skipbytes(jpgr, r);
		if (jpgr->error) {
			return 0;
		}
	}
	return 1;
}

#undef JFIFID
#undef JFXXID
#undef OCADID


CTB_INLINE uintxx
checkiccheader(struct TJPGRPblc* jpgr, uint8* s)
{
	uintxx size;
	
	/* offset zero is the profile size (uint32), signature is at
	 * offset 36 (0x61637370) "acsp" */
	size = TOI32(s[0], s[1], s[2], s[3]);

	s += 36;
	if (s[0] != 'a' &&
		s[1] != 'c' &&
		s[2] != 's' && s[3] != 'p') {
		return 0;
	}

	if (size > MAXICCPSIZE || size < 0x80) {
		SETERROR(JPGR_ELIMIT);
		return 0;
	}
	return size;
}

CTB_INLINE uintxx
readiccp(struct TJPGRPblc* jpgr, uintxx remaining)
{
	uintxx r;
	uintxx total;
	uint8* bgn;
	uint8* end;
	uint8* s;

	end = PRVT->iccpmemory + PRVT->iccptotal;
	bgn = PRVT->iccpappend;

	r = remaining;
	for (r = remaining; r; r -= total) {
		uintxx v;

		v = (uintxx) (end - bgn);
		if (v > r)
			v = r;

		total = v;
		if (total > 256)
			total = 256;
		
		if (total == 0) {
			break;
		}
		s = readinput(jpgr, total);
		if (s == NULL) {
			return 0;
		}

		memcpy(bgn, s, total);
		bgn += total;
	}
	return r;
}

CTB_INLINE bool
checkiccpsignature(struct TJPGRPblc* jpgr, uintxx r)
{
	uintxx i;
	static const uint8 signature[] = "ICC_PROFILE";

	if (r < 14) {
		/* not an ICCP segment */
		return 0;
	}

	if (ensurebytes(jpgr, 12) == 0) {
		SETERROR(JPGR_EBADDATA);
		return 0;
	}
	for (i = 0; i < 12; i++) {
		if (signature[i] != PRVT->bgn[i]) {
			return 0;
		}
	}
	return 1;
}

CTB_INLINE bool
primeiccpchunk(struct TJPGRPblc* jpgr, uintxx r)
{
	uintxx total;
	uint8* buffer;
	uint8* s;

	if (r < 0x80) {
		/* not an ICCP header */
		skipbytes(jpgr, r);
		return 0;
	}

	s = readinput(jpgr, 0x80);
	total = checkiccheader(jpgr, s);
	if (total == 0) {
		skipbytes(jpgr, r);

		PRVT->iccpmode = 2;
		return 0;
	}

	if (total > PRVT->iccpsize) {
		buffer = CTB_REALLOC(PRVT->iccpmemory, total);
		if (buffer == NULL) {
			SETERROR(JPGR_EOOM);
			return 0;
		}
		PRVT->iccpmemory = buffer;
		PRVT->iccpappend = buffer;
		PRVT->iccpsize = total;
	}
	
	PRVT->iccptotal  = total;
	PRVT->iccpappend = PRVT->iccpmemory;

	/* copy the header to the profile memory */
	memcpy(PRVT->iccpappend, s, 0x80);
	PRVT->iccpappend += 0x80;
	return 1;
}

static uintxx
parseAPP2(struct TJPGRPblc* jpgr)
{
	uintxx r;
	uint8* s;
	uint8 s1;
	uint8 s2;

	r = read16(jpgr);
	if (r < 1) {
		if (jpgr->error == 0)
			SETERROR(JPGR_EBADDATA);
		return 0;
	}
	r -= 2;

	if (checkiccpsignature(jpgr, r) == 0) {
		if (jpgr->error) {
			return 0;
		}

		skipbytes(jpgr, r);
		if (jpgr->error) {
			return 0;
		}
		return 1;
	}
	consumebytes(jpgr, 12);
	r -= 12;

	if (PRVT->iccpmode == 2) {
		skipbytes(jpgr, r);
		if (jpgr->error) {
			return 0;
		}
		return 1;
	}

	s = readinput(jpgr, 2);
	if (s == NULL) {
		return 0;
	}

	s1 = s[0];  /* sequence number */
	s2 = s[1];  /* total */
	r -= 2;

	if (PRVT->iccpmode == 0) {
		if (primeiccpchunk(jpgr, r) == 0) {
			if (jpgr->error) {
				return 0;
			}
			if (PRVT->iccpmode == 2) {
				ADDWARNING(JPGR_BADICCP);
			}
			return 1;
		}
		r -= 0x80;
		PRVT->iccps1 = s1;
		PRVT->iccps2 = s2;

		PRVT->iccpmode = 1;
	}

	/* bad sequence */
	if (s2 != PRVT->iccps2 || s1 != PRVT->iccps1) {
		skipbytes(jpgr, r);
		if (jpgr->error) {
			return 0;
		}

		PRVT->iccpmode = 2;
		ADDWARNING(JPGR_BADICCP);
		return 1;
	}
	PRVT->iccps1++;

	if ((r = readiccp(jpgr, r)) == 0) {
		if (jpgr->error) {
			return 0;
		}
	}

	/* last sequence */
	if (s1 == s2) {
		jpgr->iccprofile = PRVT->iccpmemory;
		jpgr->iccpsize   = PRVT->iccptotal;
		PRVT->iccpmode = 2;
	}

	/* ignore trailing bytes */
	if (r) {
		skipbytes(jpgr, r);
		if (jpgr->error) {
			return 0;
		}
	}
	return 1;
}

static uintxx
parseDRI(struct TJPGRPblc* jpgr)
{
	uint16 r;
	uint8* s;

	r = read16(jpgr);
	if (r <= 2) {
		return 0;
	}
	
	if (r != 4 || (s = readinput(jpgr, 2)) == NULL) {
		return 0;
	}
	PRVT->rinterval = TOI16(s[0], s[1]);
	return 1;
}


/* dezigzag table, result will be in colum-mayor order but it's transposed
 * in the inverse DCT */
static const uint8 zzorder[] = {
	 0,  8,  1,  2,  9, 16, 24, 17,
	10,  3,  4, 11, 18, 25, 32, 40,
	33, 26, 19, 12,  5,  6, 13, 20,
	27, 34, 41, 48, 56, 49, 42, 35,
	28, 21, 14,  7, 15, 22, 29, 36,
	43, 50, 57, 58, 51, 44, 37, 30,
	23, 31, 38, 45, 52, 59, 60, 53,
	46, 39, 47, 54, 61, 62, 55, 63,
	
	/* extra values to prevent overflow during decoding */
	63, 63, 63, 63, 63, 63, 63, 63,
	63, 63, 63, 63, 63, 63, 63, 63,
};

static uintxx
parseDQT(struct TJPGRPblc* jpgr)
{
	uint16 r;
	uint8* s;
	uint8 total;
	uintxx tablemap;
	
	r = read16(jpgr);
	if (r <= 1) {
		return 0;
	}
	r -= 2;
	
	tablemap = 0;
	while (r) {
		uint8 id;
		uint8 precision;
		uintxx i;
		struct TJPGQnTable* table;
		
		if ((s = readinput(jpgr, 1)) == NULL) {
			return 0;
		}
		r--;
		
		id        = (s[0] >> 0) & 0x0f;
		precision = (s[0] >> 4) & 0x0f;
		
		i = ((uintxx) 1) << id;
		if (tablemap & i || id > 3) {
			SETERROR(JPGR_ETABLEID);
			return 0;
		}
		tablemap |= 1;
		
		total = 64;
		if (precision)
			total = 128;
		
		if (r < total) {
			return 0;
		}
		
		s = readinput(jpgr, total);
		if (s == NULL) {
			return 0;
		}
		r -= total;
		
		table = PRVT->qtables + id;
		for (i = 0; i < 64; i++) {
			intxx v;
			
			if (precision == 0) {
				v = s[i];
			}
			else {
				v = (s[i << 1] << 0x08) | s[(i << 1) + 1];
			}
			table->values[zzorder[i]] = (int16) v;
		}
		table->defined = 1;
	};
	
	return 1;
}


static const uint8 upscalemap[][64] = {
    {  /* 1 1 */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f
    },
    {  /* 1 2 */
        0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03,
        0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b,
        0x10, 0x10, 0x11, 0x11, 0x12, 0x12, 0x13, 0x13,
        0x18, 0x18, 0x19, 0x19, 0x1a, 0x1a, 0x1b, 0x1b,
        0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23,
        0x28, 0x28, 0x29, 0x29, 0x2a, 0x2a, 0x2b, 0x2b,
        0x30, 0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33,
        0x38, 0x38, 0x39, 0x39, 0x3a, 0x3a, 0x3b, 0x3b
    },
    {  /* 1 4 */
        0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01,
        0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09,
        0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11,
        0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19,
        0x20, 0x20, 0x20, 0x20, 0x21, 0x21, 0x21, 0x21,
        0x28, 0x28, 0x28, 0x28, 0x29, 0x29, 0x29, 0x29,
        0x30, 0x30, 0x30, 0x30, 0x31, 0x31, 0x31, 0x31,
        0x38, 0x38, 0x38, 0x38, 0x39, 0x39, 0x39, 0x39
    },
    {  /* 2 1 */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    },
    {  /* 2 2 */
        0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03,
        0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03,
        0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b,
        0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b,
        0x10, 0x10, 0x11, 0x11, 0x12, 0x12, 0x13, 0x13,
        0x10, 0x10, 0x11, 0x11, 0x12, 0x12, 0x13, 0x13,
        0x18, 0x18, 0x19, 0x19, 0x1a, 0x1a, 0x1b, 0x1b,
        0x18, 0x18, 0x19, 0x19, 0x1a, 0x1a, 0x1b, 0x1b
    },
    {  /* 2 4 */
        0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01,
        0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09,
        0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09,
        0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11,
        0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11,
        0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19,
        0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19
    },
    {  /* 4 1 */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    },
    {  /* 4 2 */
        0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03,
        0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03,
        0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03,
        0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03,
        0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b,
        0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b,
        0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b,
        0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b
    }
};


CTB_INLINE void
initcomponents(struct TJPGRPblc* jpgr, uintxx ys, uintxx xs)
{
	uintxx i;
	uintxx y;
	uintxx x;
	uintxx sizey;
	uintxx sizex;
	struct TJPGComponent* component;
	const uintxx f[] = {
		0x00, 0x03, 0x04, 0x00, 0x05
	};
	
	/* MCU dimensions */
	PRVT->nrows = (jpgr->sizey + ((ys << 3) - 1)) >> f[ys];
	PRVT->ncols = (jpgr->sizex + ((xs << 3) - 1)) >> f[xs];
	
	sizey = PRVT->nrows * (ys << 3);
	sizex = PRVT->ncols * (xs << 3);
	for (i = 0; i < PRVT->ncomponents; i++) {
		uintxx totaly;
		uintxx totalx;
		uintxx bsizey;
		uintxx bsizex;
		uintxx n;
		uintxx j;
		uintxx rumode;
		const uintxx s[] = {
			0x00, 0x00, 0x01, 0x00, 0x02
		};
		
		component = PRVT->components + i;
		
		/* component size in number of units including padding */
		bsizey = sizey >> f[PRVT->ysampling >> s[component->ysampling]];
		bsizex = sizex >> f[PRVT->xsampling >> s[component->xsampling]];
		component->irows = bsizey;
		component->icols = bsizex;
		if (PRVT->isinterleaved) {
			component->ucount = component->ysampling * component->xsampling;
		}
		else {
			component->ucount = bsizey * bsizex;			
		}
		
		bsizey = ys >> s[component->ysampling];
		bsizex = xs >> s[component->xsampling];
		component->nrows = (jpgr->sizey + (bsizey << 3) - 1) >> f[bsizey];
		component->ncols = (jpgr->sizex + (bsizex << 3) - 1) >> f[bsizex];
		
		/* 1 = 0; 2 = 1; 4 = 3 */
		rumode = bsizex - 1;

		/* set the lookup-upscale values */
		totaly = 0x40 >> s[bsizey];
		totalx = 0x08 >> s[bsizex];
		j = 0;
		n = 0;
		for (y = 0; y < ys; y++) {
			uintxx ay;
			uintxx ax;
			uintxx um;

			um = rumode;
			ax = 0;
			ay = (y >> s[bsizey]) * component->xsampling;
			for (x = 0; x < xs; x++) {
				uintxx m;
				
				m = n + ax;
				component->iblock[j] = (uint8) (ay + (x >> s[bsizex]));
				component->offset[j] = (uint8) (m & 0x3f);
				
				ax += totalx;
				if (ax >= 8)
					ax -= 8;

				component->rumode[j] = (uint8) um;
				if (bsizex != 1)
					um++;
				j++;				
			}
			n += totaly;
		}
		n = ((s[bsizey] << 1) + s[bsizey]) + s[bsizex];
		component->umap = upscalemap[n];
	}
	
	/* pixel origin for each block */
	i = 0;
	for (y = 0; y < ys; y++) {
		for (x = 0; x < xs; x++) {
			PRVT->originy[i] = (uint8) (y << 3);
			PRVT->originx[i] = (uint8) (x << 3);
			i++;
		}
	}
	PRVT->nunits = i;
	
	if (PRVT->ysampling != 1 || PRVT->xsampling != 1) {
		PRVT->issubsampled = 1;
	}
}


/* image size limit 4GB on 64bit or 2GB on 32bit platform */
#if defined(CTB_ENV64)
	#define MAXSAFESIZE1 0x100000000LL
	#define MAXSAFESIZE3 0x055555555LL
#else
	#define MAXSAFESIZE1 0x080000000LL
	#define MAXSAFESIZE3 0x02aaaaaaaLL
#endif

/* this is not accurate but for progressive (or non-interleaved) images we
 * check the limits later */
CTB_INLINE uintxx
checksize(struct TJPGRPblc* jpgr)
{
	uintxx s;

	s = jpgr->sizey * jpgr->sizex;
	if (PRVT->ncomponents == 3) {
		if (s > MAXSAFESIZE3) {
			return  0;
		}
	}
	else {
		if (s > MAXSAFESIZE1) {
			return 0;
		}
	}

	return 1;
}

static uintxx
parseSOF0(struct TJPGRPblc* jpgr, uintxx progressive)
{
	uint16 r;
	uint8* s;
    uintxx i;
    uintxx total;
    uintxx ysampling;
    uintxx xsampling;

    if (PRVT->segmentmap.SOFXs == 1) {
    	/* multi frame image */
    	SETERROR(JPGR_ENOSUPPORTED);
    	return 0;
    }
    PRVT->segmentmap.SOFXs = 1;

	r = read16(jpgr);
	if (r < 8) {
		return 0;
	}
	r -= 8;
	
	s = readinput(jpgr, 6);
	if (s == NULL) {
		return 0;
	}
	
	/* sampling (we only support 8 bit sampling) */
	if (s[0] != 8) {
		SETERROR(JPGR_ENOSUPPORTED);
		return 0;
	}
	s++;
	
	/* image size */
	jpgr->sizey = TOI16(s[0], s[1]); s += 2;
	jpgr->sizex = TOI16(s[0], s[1]); s += 2;
	if (jpgr->sizey == 0 || jpgr->sizex == 0) {
		return 0;
	}
		
	/* number of components (strict jfif (no 4 components)) */
	if (s[0] ^ 1 && s[0] ^ 3) {
		SETERROR(JPGR_ENOSUPPORTED);
		return 0;
	}
	PRVT->ncomponents = s[0];
	
	if (checksize(jpgr) == 0) {
		SETERROR(JPGR_ELIMIT);
		return 0;
	}

	total = PRVT->ncomponents * 3;
	if (r < total) {
		return 0;
	}
	s = readinput(jpgr, total);
	if (s == NULL) {
		return 0;
	}
	
	total = 0;
	xsampling = 0;
	ysampling = 0;
	for (i = 0; i < PRVT->ncomponents; i++) {
		uint8 id;
		uint8 xs;
		uint8 ys;
		struct TJPGComponent* component;
		
		id = s[0];
		component = PRVT->components + i;
		if (component->id != (uint32) -1) {
			SETERROR(JPGR_EBADDATA);
			return 0;
		}
		component->id = id;

		ys = (s[1] >> 0) & 0x0f;
		xs = (s[1] >> 4) & 0x0f;
		
		/* subsampling */
		if (ys != 1 && ys != 2 && ys != 4) {
			SETERROR(JPGR_ENOSUPPORTED);
			return 0;
		}
		if (xs != 1 && xs != 2 && xs != 4) {
			SETERROR(JPGR_ENOSUPPORTED);
			return 0;
		}
		
		if (ys > ysampling)
			ysampling = ys;
		if (xs > xsampling)
			xsampling = xs;
		component->ysampling = ys;
		component->xsampling = xs;
		
		total += ys * xs;

		/* quantization table */
		if (s[2] > 3) {
			return 0;
		}
		component->qtable = PRVT->qtables + s[2];
		s += 3;
	}
	
	if (PRVT->ncomponents == 3) {
		struct TJPGComponent* c;

		c = PRVT->components;
		/* RGB or rgb */
		if ((c[0].id | 0x20) == 'r' &&
			(c[1].id | 0x20) == 'g' &&
			(c[2].id | 0x20) == 'b') {
			PRVT->isrgb = 1;
		}
		else {
			if (jpgr->flags & JPGR_KEEPYCBCR) {
				/* don't do color transform */
				PRVT->keepyuv = 1;
			}
		}
	}

	/* MCU size limit */
	if (total > 10) {
		SETERROR(JPGR_EINVALIDIMAGE);
		return 0;
	}
	PRVT->ysampling = ysampling;
	PRVT->xsampling = xsampling;
	
	jpgr->isprogressive = progressive;
	return 1;
}


static uintxx buildtable(struct TJPGHmTable*, uintxx, uint8*, uint8*);

static uintxx
parseDHT(struct TJPGRPblc* jpgr)
{
	uintxx r;
	uint8* s;
	uintxx i;	
	uintxx total;
	uintxx tablemap;
	uintxx mode;

	r = read16(jpgr);
	if (r <= 1) {
		return 0;
	}
	r -= 2;
	
	tablemap = 0;
	while (r) {
		uint8 type;
		uint8 id;
		uint8 lns[16];
		struct TJPGHmTable* table;

		if ((s = readinput(jpgr, 1)) == NULL) {
			return 0;
		}
		r--;
		
		id   = (s[0] >> 0) & 0x0f;
		type = (s[0] >> 4) & 0x0f;
		if (type > 1 || id > 3) {
			SETERROR(JPGR_ETABLEID);
			return 0;
		}

		/* check if the table is redefined */
		i = ((uintxx) 1) << ((type << 1) + id);
		if (tablemap & i) {
			SETERROR(JPGR_ETABLEID);
			return 0;
		}
		tablemap |= i;
		
		/* counts for each lenght */
		total = 16;
		if (r < total || (s = readinput(jpgr, total)) == NULL) {
			return 0;
		}
		r -= 16;
		
		for (i = total = 0; i < 16; i++) {
			total += (lns[i] = s[i]);
		}
		
		/* symbols */
		if (256 < total) {
			return 0;
		}
		if (r < total || (s = readinput(jpgr, total)) == NULL) {
			return 0;
		}
		r -= total;
		
		table = (void*) (PRVT->dctables + id);
		if (type == 1) {
			table = (void*) (PRVT->actables + id);
		}
		
		mode = type;
		if (jpgr->isprogressive == 0) {
			if (mode == 1) {
				mode = mode | (1 << 2);
			}
		}
		if (buildtable(table, mode, lns, s) == 0) {
			SETERROR(JPGR_EBADHMTABLE);
			return 0;
		}
		table->defined = 1;
	};
	
	return 1;
}

CTB_INLINE uintxx
readpassinfo(struct TJPGRPblc* jpgr, uint8* s)
{
	uint8 ss;
	uint8 se;
	uint8 al;
	uint8 ah;
	
	ss = s[0];
	se = s[1];
	ah = 0x0f & (s[2] >> 4);
	al = 0x0f & (s[2] >> 0);
	
	if (ss > 63 || se > 63 || ss > se) {
		return 0;
	}
	
	if (ah > 13 || al > 13) {
		return 0;
	}
	
	PRVT->ss = ss;
	PRVT->se = se;
	PRVT->al = al;
	PRVT->ah = ah;
	return 1;
}

CTB_INLINE uintxx
setrequiredmemory(struct TJPGRPblc* jpgr)
{
	uintxx i;
	uint64 total;
	struct TJPGComponent* c;
	
	total = 0;
	if (PRVT->isinterleaved) {
		for (i = 0; i < PRVT->ncomponents; i++) {
			c = PRVT->components + i;
			total += c->ucount;
		}
	}
	else {
		for (i = 0; i < PRVT->ncomponents; i++) {
			c = PRVT->components + i;
			total += c->ucount + (c->ysampling * c->xsampling);
		}	
	}
	total = (total * 64) * sizeof(c->units[0][0]) + 16;

	if (PRVT->issubsampled) {
		total += PRVT->ncomponents * (8 * sizeof(c->srow[0]));
	}
	
	/* check memory limits */
	if (total > MAXSAFESIZE1) {
		return 0;
	}
	jpgr->requiredmemory = (uintxx) total;
	return 1;
}

CTB_INLINE uintxx
findcomponent(struct TJPGRPblc* jpgr, uintxx id)
{
	uintxx i;

	for (i = 0; i < PRVT->ncomponents; i++) {
		if (PRVT->components[i].id == id) {
			return i;
		}
	}
	return -1;
}

static uintxx
parseSOS(struct TJPGRPblc* jpgr)
{
	uint16 r;
	uint8* s;
	uintxx i;
	uintxx j;
	uintxx total;
	struct TJPGComponent* c;
	uintxx index;
	uint8 ac;
	uint8 dc;
	
	if (PRVT->segmentmap.SOFXs == 0) {
		SETERROR(JPGR_SEGMENTORDER);
		return 0;
	}
	PRVT->segmentmap.SOSs = 1;

	r = read16(jpgr);
	if (r <= 2) {
		return 0;
	}
	r -= 2;
	
	if ((s = readinput(jpgr, 1)) == NULL) {
		return 0;
	}
	
	j = s[0];
	if (j != 1 && j != 3) {
		SETERROR(JPGR_ENOSUPPORTED);
		return 0;
	}
	if (j == 3 && PRVT->ncomponents == 1) {
		return 0;
	}
	PRVT->nscancomponents = j;
	
	/* component tables, spectral selection and progressive aproximation */
	total = (j * 2) + 3;
	if (r < total || (s = readinput(jpgr, total)) == NULL) {
		return 0;
	}

	for (i = 0; i < j; i++) {
		index = findcomponent(jpgr, s[0]);
		if (index == (uintxx) -1) {
			SETERROR(JPGR_EBADDATA);
			return 0;
		}

		c = PRVT->components + index;
		PRVT->corder[i] = index;

		ac = (s[1] >> 0) & 0x0f;
		dc = (s[1] >> 4) & 0x0f;
		if (ac > 3 || dc > 3) {
			SETERROR(JPGR_ETABLEID);
			return 0;
		}
		c->dctable = PRVT->dctables + dc;
		c->actable = PRVT->actables + ac;
		s += 2;
	}
	PRVT->scancomponent = index;

	if (jpgr->isprogressive) {
		if (readpassinfo(jpgr, s) == 0) {
			SETERROR(JPGR_EINVALIDPASS);
			return 0;
		}
	}
	else {
		if (jpgr->state == 0) {
			if (PRVT->ncomponents == j) {
				PRVT->isinterleaved = 1;
			}
			else {
				if (PRVT->ncomponents < j) {
					SETERROR(JPGR_EBADDATA);
					return 0;
				}
			}
		}
		else {
			if (j != 1) {
				SETERROR(JPGR_ENOSUPPORTED);
				return 0;
			}
		}

		if (PRVT->isinterleaved) {
			for (i = 0; i < j; i++) {
				c = PRVT->components + i;
				if (c->actable->defined == 0 || c->dctable->defined == 0) {
					SETERROR(JPGR_ENOHMTABLE);
					return 0;
				}

				if (c->qtable->defined == 0) {
					SETERROR(JPGR_ENOQTTABLE);
					return 0;
				}
			}
		}
		else {
			c = PRVT->components + PRVT->scancomponent;
			if (c->actable->defined == 0 || c->dctable->defined == 0) {
				SETERROR(JPGR_ENOHMTABLE);
				return 0;
			}
			
			if (c->qtable->defined == 0) {
				SETERROR(JPGR_ENOQTTABLE);
				return 0;
			}
		}
	}

	if (jpgr->state == 0) {
		initcomponents(jpgr, PRVT->ysampling, PRVT->xsampling);
		if (setrequiredmemory(jpgr) == 0) {
			SETERROR(JPGR_ELIMIT);
			return 0;
		}
	}
	return 1;
}

bool
jpgr_initdecoder(TJPGReader* jpgr, TImageInfo* info)
{
	uint16 m;
	ASSERT(jpgr && info);
	
	if (jpgr->state) {
		goto L_ERROR;
	}
	
	/* at this point we need an input function */
	if (PRVT->inputfn == NULL) {
		SETERROR(JPGR_EIOERROR);
		goto L_ERROR;
	}
	
	m = read16(PBLC);
	if (m == SOI) {
		uintxx mode;
		
		if (parsesegments(PBLC) == 0) {
			goto L_ERROR;
		}
		
		/* color mode */
		mode = IMAGE_GRAY;
		if (PRVT->ncomponents == 3) {
			mode = IMAGE_YCBCR;
			if (PRVT->isrgb)
				mode = IMAGE_RGB;
		}
		PBLC->colortype = mode;

		/* set values */
		if (PRVT->ncomponents == 3) {
			if (mode == IMAGE_YCBCR && PRVT->keepyuv == 0) {
				mode = IMAGE_RGB;
			}
		}
		
		info->sizey = jpgr->sizey;
		info->sizex = jpgr->sizex;
		info->colortype = mode;
		info->depth = 8;
		info->imgsize = jpgr->sizex * jpgr->sizey * PRVT->ncomponents;

		SETSTATE(1);
		return 1;
	}

	/* not a jpeg file */
	if (jpgr->error == 0)
		SETERROR(JPGR_EBADFILE);

L_ERROR:
	if (jpgr->error == 0) {
		SETERROR(JPGR_EBADDATA);
	}
	
	SETSTATE(JPGR_BADSTATE);
	return 0;
}

void
jpgr_setbuffers(TJPGReader* jpgr, uint8* memory, uint8* pixels)
{
	uintxx i;
	uintxx j;
	struct TJPGComponent* c;
	ASSERT(jpgr && memory);
	
	if (jpgr->state ^ 1) {
		SETSTATE(JPGR_BADSTATE);
		if (jpgr->error == 0) {
			SETERROR(JPGR_EBADUSE);
		}
		return;
	}
	
	memory = (uint8*) ((((uintxx) memory) | 15) + 1);

	/* scan memory */
	if (PRVT->isinterleaved == 0) {
		for (i = 0; i < PRVT->ncomponents; i++) {
			c = PRVT->components + i;
			
			c->scan = (void*) memory;
			memory += (c->ucount * 64) * (sizeof(c->scan[0]));
		}
	}
	
	/* memory for each unit */
	for (i = 0; i < PRVT->ncomponents; i++) {
		uintxx n;
			
		c = PRVT->components + i;
		
		n = c->ucount;
		if (PRVT->isinterleaved == 0) {
			n = c->ysampling * c->xsampling;
		}
		
		for (j = 0; j < n; j++) {
			c->units[j] = (void*) memory;
			memory += (64 * sizeof(c->units[0][0]));
		}
	}

	if (PRVT->issubsampled) {
		for (i = 0; i < PRVT->ncomponents; i++) {
			c = PRVT->components + i;

			c->srow = (void*) memory;
			memory += (8 * sizeof(c->srow[0]));
		}
	}
	
	PRVT->pixels = pixels;
	if (jpgr->isprogressive && pixels) {
		memset(pixels, 0, jpgr->sizey * jpgr->sizex * PRVT->ncomponents);
	}
	SETSTATE(2);
	
}

/*
 * Image decoder */

#define LENGTHBITS 5
#define LENGTHMASK ((1u << LENGTHBITS) - 1)

#define GETLENGTH(S) ((S) &  LENGTHMASK)
#define GETSYMBOL(S) ((S) >> LENGTHBITS)


CTB_INLINE intxx
extend(intxx m, intxx a)
{
#if defined(CTB_ENV64)
	return a + (((a - ((1LL) << (m - 1))) >> 31) & ((((uintxx) -1) << m) + 1));
#else
	return a + (((a - (( 1L) << (m - 1))) >> 31) & ((((uintxx) -1) << m) + 1));
#endif
}

static void
buildextenttable(struct TJPGACHmTable* table)
{
	uintxx i;
	uint16 s;
	intxx rs;
	intxx length;
	intxx rrrr;
	intxx ssss;
	
	/* The idea for this optimization comes from stb_image by Sean Barret */
	for (i = 0; i < (1u << ROOTBITS); i++) {
		intxx v;
		intxx a;
		
		table->sextent[i] = 0;
		s = table->symbols[i];
		if ((int16) s < 0) {
			/* subtable */
			continue;
		}
		rs     = GETSYMBOL(s);
		length = GETLENGTH(s);
		
		rrrr = (rs >> 4);
		ssss = (rs >> 0) & 0x0f;
		if (ssss == 0 || ((length + ssss) > ROOTBITS)) {
			continue;
		}
		
		/* additional bits */
		a = ((i << length) & ((1l << ROOTBITS) - 1)) >> (ROOTBITS - ssss);
		
		v = extend(ssss, a);
		if (v < -128 || v > 127) {
			/* value is too large fo fit in 8 bits */
			continue;
		}
		rs = (int16) ((v << 8) | (rrrr << 4) | (length + ssss));
		table->sextent[i] = (int16) rs;
	}
}

static uintxx
buildtable(struct TJPGHmTable* table, uintxx mode, uint8* lns, uint8* symbols)
{
	intxx i;
	intxx j;
	intxx m;
	intxx r;
	intxx v;
	intxx k;
	intxx count;
	intxx offset;
	uint16 c;
	uint16 codes[16];

	j = 1;
	c = 0;
	for (i = m = 0; i < 16; i++) {
		j = (j << 1) - lns[i];
		if (j < 0) {
			
			return 0;
		}
		m += lns[i];

		codes[i] = c;
		c = (c + lns[i]) << 1;
	}
	
	/* check symbols range 0-15 for DC tables */
	if ((mode & 0x01) == 0) {
		for (i = 0; i < m; i++) {
			if (symbols[i] > 15) {
				return 0;
			}
		}
		v = ENOUGH_DC;
	}
	else {
		v = ENOUGH_AC;
	}
	
	/* reset the main entries in the table */
	for (i = 0; i < 1u << ROOTBITS; i++) {
		table->symbols[i] = 0;
	}
	
	/* mark secondary tables as secondary and set the offsets */	
	offset = 1u << ROOTBITS;
	for (i = 16; i > ROOTBITS; i--) {
		count = lns[i - 1];
		if (count == 0) {
			continue;
		}
		
		r = i - ROOTBITS;
		c = codes[i - 1] >> r;
		
		j = count >> r;
		if (count & ((1u << r) - 1))
			j++;
		
		for (m = 0; m < j; m++) {
			uintxx entry;
			k = c + m;
			
			if (table->symbols[k]) {
				continue;
			}
			
			/* Store the shift needed to get the lower bits of the code
			 * in a 16bit word, for subtables we need to mask the top bits and
			 * shift rigth using this value.
			 * The top bit is used to indicate the symbol is a
			 * subtable offset. */
			entry = (1u << 15) | (offset << LENGTHBITS) | (16 - i);

			table->symbols[k] = (uint16) entry;
			offset += ((uintxx) 1) << r;
		}
	}
	
	if (offset > v) {
		/* should not happen */
		return 0;
	}
	
	/* populate the table */
	m = 0;
	for (j = 0; j < 16; j++) {
		uint16 e;
		
		count = lns[j];
		if (count == 0) {
			continue;
		}
		
		for (v = 0; v < count; v++) {
			e = (uint16) ((((uint16) (symbols[m++])) << LENGTHBITS) | (j + 1));
			
			k = 0;
			if ((j + 1) > ROOTBITS) {
				uint16 entry;
				
				r = (j + 1) - ROOTBITS;
				c = codes[j] >> r;
				
				entry = table->symbols[c];
				c = codes[j] & ((1u << r) - 1);
				
				r = ((16 - ROOTBITS) - GETLENGTH(entry)) - r;
				c = c << r;
				
				/* offset */
				k = GETSYMBOL(entry & ((1u << 15) - 1));
			}
			else {
				r = ROOTBITS - (j + 1);
				c = codes[j] << r;
			}
			
			for (i = (1u << r) - 1; i >= 0; i--) {
				table->symbols[k + (c | i)] = e;
			}
			codes[j]++;
		}
	}
	
	if ((mode >> 1) != 0) {
		buildextenttable((void*) table);
	}
	return 1;
}


/* 
 * BIT reading functions */

#define BUFFERBYTES (BPREFETCHBZ * sizeof(PRVT->bb[0]))

static void
fecthbits(struct TJPGRPblc* jpgr)
{
	uintxx j;
	uintxx s;
	uintxx r;
	uintxx index;
	uintxx buffer;
	
	PRVT->bindex = 0;
	if (UNLIKELY(PRVT->bend)) {
		if (PRVT->bend == 1) {
			for (index = 0; index < BPREFETCHBZ; index++) {
				PRVT->bb[index] = 0;
			}
			PRVT->bend++;
		}
		return;
	}
	
	index = 0;
	if (LIKELY(((uintxx) (PRVT->end - PRVT->bgn)) >= BUFFERBYTES)) {
		for (; index < BPREFETCHBZ; PRVT->bgn += 2) {
			if (UNLIKELY(PRVT->bgn[0] == 0xff)) {
				PRVT->bbcread += (index << 1) << 3;
				goto L_SLOW;
			}
			if (UNLIKELY(PRVT->bgn[1] == 0xff)) {
				PRVT->bbcread += (index << 1) << 3;
				goto L_SLOW;
			}
			
			PRVT->bb[index++] = (PRVT->bgn[0] << 8) | PRVT->bgn[1];
		}
		
		PRVT->bbcread += BUFFERBYTES << 3;
		return;
	}
		
L_SLOW:
	j = 0;
	s = 0;
	r = 0;
	buffer = 0;
	for (; index < BPREFETCHBZ;) {
		uintxx m;
		uintxx v;
		
		v = PRVT->end - PRVT->bgn;
		if (LIKELY(v > 1)) {
			m = PRVT->bgn[0];
		}
		else {
			if (UNLIKELY(PRVT->endofinput)) {
				m = 0;
				s = 1;
				if (v) {
					m = PRVT->bgn[0];
					if (m == 0xff) {
						m = 0;
					}
					else {
						s = 0;
					}
				}
			}
			else {
				readmore(jpgr, v, 4 * BUFFERBYTES);
				continue;
			}
		}
		
		if (UNLIKELY(m == 0xff)) {
			if (PRVT->bgn[1]) {
				m = 0;
				s = 1;
				PRVT->bend = 1;
			}
			if (s == 0) {
				PRVT->bgn++;
				PRVT->bgn++;
				r++;
			}
		}
		else {
			if (s == 0) {
				PRVT->bgn++;
				r++;
			}
		}
		
		buffer = m | (buffer << 8);
		j += 8;
		if (j == 16) {
			PRVT->bb[index++] = (uint16) buffer;
			j = buffer = 0;
		}
	}
	
	PRVT->bbcread += r << 3;
}

#undef BUFFERBYTES


CTB_INLINE void
initbitmode(struct TJPGRPblc* jpgr)
{
	PRVT->bbuffer = 0;
	PRVT->bbcount = 0;
	
	PRVT->bbcread = 0;
	PRVT->bend = 0;
	fecthbits(jpgr);
}

CTB_INLINE void
ensurebits(struct TJPGRPblc* jpgr, uintxx n)
{
	if (LIKELY(PRVT->bbcount < n)) {
		if (UNLIKELY(PRVT->bindex >= BPREFETCHBZ)) {
			fecthbits(jpgr);
		}
		PRVT->bbuffer  = (PRVT->bbuffer << 16) | PRVT->bb[PRVT->bindex++];
		PRVT->bbcount += 16;
	}
}

CTB_INLINE uintxx
getbits(struct TJPGRPblc* jpgr, uintxx n)
{
	return PRVT->bbuffer >> (PRVT->bbcount - n);
}

CTB_INLINE void
dropbits(struct TJPGRPblc* jpgr, uintxx n)
{
	PRVT->bbcread -= n;
	PRVT->bbcount -= n;
	PRVT->bbuffer = PRVT->bbuffer & ~(((uintxx) -1) << PRVT->bbcount);
}

CTB_INLINE bool
overread(struct TJPGRPblc* jpgr)
{
	if (PRVT->bbcread < 0) {
		return 1;
	}
	return 0;
}


/*
 * Decoder */

#define ROOTMASK (~(((1u << ROOTBITS) - 1) << (16 - ROOTBITS)))

CTB_INLINE uint16
decodesymbol(struct TJPGACHmTable* table, uintxx bits)
{
	int16 s;

	s = (uint16) table->symbols[bits >> (16 - ROOTBITS)];
	
	if (UNLIKELY((int16) s < 0)) {
		uintxx offset;
		uintxx extra;
		
		offset = GETSYMBOL(s & ((1u << 15) - 1));
		extra  = GETLENGTH(s);
		s = table->symbols[offset + ((bits & ROOTMASK) >> extra)];
	}
	return s;
}

#undef ROOTMASK


CTB_INLINE BBTYPE
fillbbuffer(struct TJPGRPblc* jpgr, BBTYPE bb)
{
	/* keep 16 bits */
#if defined(CTB_ENV64)
	if (UNLIKELY(PRVT->bindex >= BPREFETCHBZ))
		fecthbits(jpgr);
	bb = (bb << 16) | PRVT->bb[PRVT->bindex++];
	
	if (UNLIKELY(PRVT->bindex >= BPREFETCHBZ))
		fecthbits(jpgr);
	bb = (bb << 16) | PRVT->bb[PRVT->bindex++];
#endif
	
	if (UNLIKELY(PRVT->bindex >= BPREFETCHBZ))
		fecthbits(jpgr);
	bb = (bb << 16) | PRVT->bb[PRVT->bindex++];
	
	return bb;
}


#define GETBITS(BB, BC, N) ((BB) >> ((BC) - (N)))

#define DROPBITS(BB, BC, N) ((BB) &= ~(((BBTYPE) -1l) << ((BC) -= (N))))


#if defined(CTB_ENV64)
#	define BBFILLBITS 48
#else
#	define BBFILLBITS 16
#endif

static bool
decodeblock(struct TJPGRPblc* jpgr, struct TJPGComponent* c, int16* block)
{
	uintxx i;
	uintxx symbol;
	uintxx length;
	uintxx r;
	BBTYPE bb;
	uintxx bc;
	struct TJPGDCHmTable* dc;
	struct TJPGACHmTable* ac;
	int16 s;

	dc = c->dctable;
	ac = c->actable;

	bb = PRVT->bbuffer;
	bc = PRVT->bbcount;
	r = 0;

	/* sets the cofficients to zero */
	memset(block, 0, 64 * sizeof(block[0]));

	/* DC cofficient decoding */
	if (bc < 16) {
		bb = fillbbuffer(jpgr, bb);
		bc += BBFILLBITS;
	}
	s = decodesymbol((void*) dc, GETBITS(bb, bc, 16));
	if (UNLIKELY(s == 0)) {
		/* invalid code */
		SETERROR(JPGR_EBADCODE);
		return 0;
	}
	length = GETLENGTH(s);
	DROPBITS(bb, bc, length);
	r += length;

	symbol = GETSYMBOL(s);
	if (bc < 16) {
		bb = fillbbuffer(jpgr, bb);
		bc += BBFILLBITS;
	}
	c->cofficient += extend(symbol, GETBITS(bb, bc, symbol));	
	block[0] = (int16) c->cofficient;

	DROPBITS(bb, bc, symbol);
	r += symbol;
	
	/* AC cofficients decoding */
	for (i = 1; i < 64; i++) {
		if (bc < 16) {
			bb = fillbbuffer(jpgr, bb);
			bc += BBFILLBITS;
		}
		
		/* fast decoding for run-length + extended value */
		s = ac->sextent[GETBITS(bb, bc, ROOTBITS)];
		if (LIKELY(s != 0)) {
			/* this can't be out of range now, that is why we extended
			 * zzorder by 16 */
			i += (s >> 4) & 0x0f;
			block[zzorder[i]] = s >> 8;

			length = s & 0x0f;
			DROPBITS(bb, bc, length);
			r += length;
			continue;
		}
		
		s = decodesymbol(ac, GETBITS(bb, bc, 16));
		if (UNLIKELY(s == 0)) {
			/* invalid code */
			SETERROR(JPGR_EBADCODE);
			return 0;
		}
		length = GETLENGTH(s);

		DROPBITS(bb, bc, length);
		r += length;

		symbol = GETSYMBOL(s);
		if (symbol == 0) {
			break;
		}
		
		if (symbol > 15) {
			/* zero run-length */
			i += symbol >> 4;
			symbol = symbol & 0x0f;
			
			if (UNLIKELY(i >= 64)) {
				if (bc < 16) {
					bb = fillbbuffer(jpgr, bb);
					bc += BBFILLBITS;
				}

				DROPBITS(bb, bc, symbol);
				r += symbol;
				break;
			}
		}
		
		if (bc < 16) {
			bb = fillbbuffer(jpgr, bb);
			bc += BBFILLBITS;
		}
		block[zzorder[i]] = (int16) extend(symbol, GETBITS(bb, bc, symbol));
		
		DROPBITS(bb, bc, symbol);
		r += symbol;
	}
	
	/* restore the state and check for bit overread */
	PRVT->bbuffer = bb;
	PRVT->bbcount = bc;
	PRVT->bbcread = PRVT->bbcread - r;
	if (UNLIKELY(overread(jpgr))) {
		return 0;
	}
	return 1;
}


#if 0

/* keep as reference */
#define C1    0.980785280f /* cos(1 * pi / 16) */
#define C3    0.831469612f /* cos(3 * pi / 16) */
#define C5    0.555570233f /* cos(5 * pi / 16) */
#define C6    0.382683432f /* cos(6 * pi / 16) */
#define C7    0.195090322f /* cos(7 * pi / 16) */
#define S1    0.195090322f /* sin(1 * pi / 16) */
#define S3    0.555570233f /* sin(3 * pi / 16) */
#define S6    0.923879532f /* sin(6 * pi / 16) */
#define SQRT2 1.414213562f /* sqrt(2) */

#define C6xSQRT2 (C6 * SQRT2)
#define S6xSQRT2 (S6 * SQRT2)

#define A ((-C1 + C3 + C5 - C7) * SQRT2)
#define B (( C1 + C3 - C5 + C7) * SQRT2)
#define C (( C1 + C3 + C5 - C7) * SQRT2)
#define D (( C1 + C3 - C5 - C7) * SQRT2)
#define E ((-C3 + C7) * SQRT2)
#define F ((-C1 - C3) * SQRT2)
#define G ((-C3 - C5) * SQRT2)
#define H ((-C3 + C5) * SQRT2)
#define I (( C3) * SQRT2)

#endif

#if defined(JPGR_CFG_EXTERNALASM)

extern void jpgr_inverseDCT(int16*, int16*, int16*);

#else

/* constant values scaled to (2**13) */
#define C6xSQRT2  4433
#define S6xSQRT2 10703

#define A   2446
#define B  16819
#define C  25172
#define D  12299
#define E  -7373
#define F -20995
#define G -16069
#define H  -3196
#define I   9633


/*
* Based on the paper (same algorithm used in IJG jpeg library and turbo-jpeg):
* Practical fast 1-D DCT algorithms with 11 multiplications
* by Christoph Loeffler, Adriaan Lieenberg and George S. Moschytz. */

static void
inverseDCT(int16* sblock, int16* rblock, int16* qtable)
{
	int32 l0;
	int32 l1;
	int32 l2;
	int32 l3;
	int32 l4;
	int32 l5;
	int32 l6;
	int32 l7;
	int32 z0, z1, z2, z3, z4, z5;
	int32 r[64];
	int32* rr;
	uintxx i;
	
#define y7 l4
#define y5 l5
#define y3 l6
#define y1 l7
	
	rr = r;
	for (i = 0; i < 8; i++) {
	    l0 = sblock[0 * 8];
	    l1 = sblock[4 * 8];
	    l2 = sblock[2 * 8];
	    l3 = sblock[6 * 8];
	    l4 = sblock[7 * 8];  /* y7 */
	    l5 = sblock[5 * 8];  /* y5 */
	    l6 = sblock[3 * 8];  /* y3 */
	    l7 = sblock[1 * 8];  /* y1 */
	    
	    if ((l1 | l2 | l3 | l4 | l5 | l6 | l7) == 0) {
	    	l0 = (l0 * qtable[0]) << 1;
			rr[0] = l0;
			rr[1] = l0;
			rr[2] = l0;
			rr[3] = l0;
			rr[4] = l0;
			rr[5] = l0;
			rr[6] = l0;
			rr[7] = l0;
		    qtable++;
		    sblock++;
		    rr += 8;
		    continue;
	    }
	    
	    /* dequantize */
	    l0 *= qtable[0 * 8];
	    l1 *= qtable[4 * 8];
	    l2 *= qtable[2 * 8];
	    l3 *= qtable[6 * 8];
	    l4 *= qtable[7 * 8];
	    l5 *= qtable[5 * 8];
	    l6 *= qtable[3 * 8];
	    l7 *= qtable[1 * 8];
	    
	    /* 
	     * even part */
	    
	    /* stage 3 */
	    z0 = (l0 + l1) << 13;
	    z1 = (l0 - l1) << 13;
	    
	    l0 = z0;
	    l1 = z1;
	    
	    /*
	    first rotation:
	    same as:
	    z2 = l2 * -(C6xSQRT2) + l3 * (S6xSQRT2);
	    z3 = l2 *  (S6xSQRT2) + l3 * (C6xSQRT2); */
	    z5 = S6xSQRT2 * (l2 + l3);
	    z2 = l2 * -(S6xSQRT2 + C6xSQRT2) + z5;
	    z3 = l3 *  (C6xSQRT2 - S6xSQRT2) + z5;
	    
	    /* stage 2 */
	    l0 = z3 + z0;
	    l1 = z1 - z2;
	    l2 = z2 + z1;
	    l3 = z0 - z3;
	    
	    /*
	     * odd part */
	    
	    /* alternative implementation for the odd part, using one
	     * multiplication per path as descrived in figure 8 */
	    z1 = y7 + y1;
	    z2 = y5 + y3;
	    z3 = y7 + y3;
	    z4 = y5 + y1;
	    z5 = z3 + z4;
	    
	    y7 *= A;
	    y5 *= B;
	    y3 *= C;
	    y1 *= D;
	    z1 *= E;
	    z2 *= F;
	    z3 *= G;
	    z4 *= H;
	    z5 *= I;
	        
	    z4 += z5;
	    z3 += z5;
	        
	    y1 += z1 + z4;
	    y3 += z2 + z3;
	    y5 += z2 + z4;
	    y7 += z1 + z3;
	    
	    /* last stage */
	    /* keep 1 bit of precision, plus 3 (scaled by 8) */
	    rr[0] = ((l0 + l7) + 2048) >> 12;
	    rr[7] = ((l0 - l7) + 2048) >> 12;
	    rr[1] = ((l1 + l6) + 2048) >> 12;
	    rr[6] = ((l1 - l6) + 2048) >> 12;
	    rr[2] = ((l2 + l5) + 2048) >> 12;
	    rr[5] = ((l2 - l5) + 2048) >> 12;
	    rr[3] = ((l3 + l4) + 2048) >> 12;
	    rr[4] = ((l3 - l4) + 2048) >> 12;
	    qtable++;
	    sblock++;
	    rr += 8;
	}
	
	rr = r;
	for (i = 0; i < 8; i++) {
	    /* even part */
	    l0 = rr[0 * 8];
	    l1 = rr[4 * 8];
	    l2 = rr[2 * 8];
	    l3 = rr[6 * 8];
	    l4 = rr[7 * 8]; /* y7 */
	    l5 = rr[5 * 8]; /* y5 */
	    l6 = rr[3 * 8]; /* y3 */
	    l7 = rr[1 * 8]; /* y1 */
	    
	    /* stage 3 */
	    z0 = (l0 + l1) << 13;
	    z1 = (l0 - l1) << 13;
	    
	    z5 = S6xSQRT2 * (l2 + l3);
	    z2 = l2 * -(S6xSQRT2 + C6xSQRT2) + z5;
	    z3 = l3 *  (C6xSQRT2 - S6xSQRT2) + z5;
	    
	    /* stage 2 */
	    l0 = z3 + z0;
	    l1 = z1 - z2;
	    l2 = z2 + z1;
	    l3 = z0 - z3;
	    
	    /* odd part */
	    z1 = y7 + y1;
	    z2 = y5 + y3;
	    z3 = y7 + y3;
	    z4 = y5 + y1;
	    z5 = z3 + z4;
	        
	    y7 *= A;
	    y5 *= B;
	    y3 *= C;
	    y1 *= D;
	    z1 *= E;
	    z2 *= F;
	    z3 *= G;
	    z4 *= H;
	    z5 *= I;
	        
	    z4 += z5;
	    z3 += z5;
	    
	    y1 += z1 + z4;
	    y3 += z2 + z3;
	    y5 += z2 + z4;
	    y7 += z1 + z3;
	    
	    /* last stage */
	    /* 13 bits +
	     *  4 bits (1 bit preview pass + 3 current pass) */
	    rblock[0 * 8] = ((l0 + l7) + 65536) >> 17;
	    rblock[7 * 8] = ((l0 - l7) + 65536) >> 17;
	    rblock[1 * 8] = ((l1 + l6) + 65536) >> 17;
	    rblock[6 * 8] = ((l1 - l6) + 65536) >> 17;
	    rblock[2 * 8] = ((l2 + l5) + 65536) >> 17;
	    rblock[5 * 8] = ((l2 - l5) + 65536) >> 17;
	    rblock[3 * 8] = ((l3 + l4) + 65536) >> 17;
	    rblock[4 * 8] = ((l3 - l4) + 65536) >> 17;
	    rr++;
	    rblock++;
	}
	
#undef y7
#undef y5
#undef y3
#undef y1
}

#endif

/*
 * We use the formula from the specification (CCIR 601 (256 levels)).
 * 
 * r = y + 1.402   * (cr - 128)
 * g = y - 0.34414 * (cb - 128) - 0.71414 * (cr - 128)
 * b = y + 1.772   * (cb - 128) */

/* */
struct TJPGRGB {
	uint8 r;
	uint8 g;
	uint8 b;
};


/* values scaled to 2**12 */
#define FIXED_1_402 5743
#define FIXED_0_344 1410
#define FIXED_0_714 2925
#define FIXED_1_772 7258

CTB_INLINE struct TJPGRGB
toRGB(int16 y, int16 cb, int16 cr, uintxx transform)
{
	int32 r;
	int32 g;
	int32 b;
	if (transform) {
		int32 m;
		r = cr *  FIXED_1_402;
		g = cb * -FIXED_0_344 + cr * -FIXED_0_714;
		b = cb *  FIXED_1_772;
		
		/* + 0.5 + 128 */		
		m = (y << 12) + 2048 + 524228;
		r = (m + r) >> 12;
		g = (m + g) >> 12;
		b = (m + b) >> 12;
	}
	else {
		r = y  + 128;
		g = cb + 128;
		b = cr + 128;
	}
	if ((uint32) r > 255) {
		if (r > 255)
			r = 255;
		else
			r = 0;
	}
	if ((uint32) g > 255) {
		if (g > 255)
			g = 255;
		else
			g = 0;
	}
	if ((uint32) b > 255) {
		if (b > 255)
			b = 255;
		else
			b = 0;
	}

	return (struct TJPGRGB) {r, g, b};
}

CTB_INLINE uint8
tograyscale(int16 v)
{
	int16 r;

	r = v + 128;
	if ((uint16) r > 255) {
		if (r > 255)
			r = 255;
		else
			r = 0;
	}
	return (uint8) r;
}


#if defined(JPGR_CFG_EXTERNALASM)

extern void jpgr_setrow3(int16*, int16*, int16*, uint8*, uintxx);
extern void jpgr_setrow1(int16* r1, uint8* row);

#else

CTB_INLINE void
setrow3(int16* r1, int16* r2, int16* r3, uint8* row, uintxx transform)
{
	int32 r;
	int32 g;
	int32 b;
	intxx i;
	
	if (LIKELY(transform)) {
		for (i = 0; i < 8; i++, row += 3) {
			int32 m;
			r = r3[i] *  FIXED_1_402;
			g = r2[i] * -FIXED_0_344 + r3[i] * -FIXED_0_714;
			b = r2[i] *  FIXED_1_772;
			
			/* + 0.5 + 128 */		
			m = (r1[i] << 12) + 2048 + 524228;
			r = (m + r) >> 12;
			g = (m + g) >> 12;
			b = (m + b) >> 12;
			
			if ((uint32) r > 255) {
				if (r > 255)
					r = 255;
				else
					r = 0;
			}
			if ((uint32) g > 255) {
				if (g > 255)
					g = 255;
				else
					g = 0;
			}
			if ((uint32) b > 255) {
				if (b > 255)
					b = 255;
				else
					b = 0;
			}
			row[0] = r;
			row[1] = g;
			row[2] = b;
		}
		return;
	}
	
	for (i = 0; i < 8; i++, row += 3) {
		r = r1[i] + 128;
		g = r2[i] + 128;
		b = r3[i] + 128;
	
		if ((uint32) r > 255) {
			if (r > 255)
				r = 255;
			else
				r = 0;
		}
		if ((uint32) g > 255) {
			if (g > 255)
				g = 255;
			else
				g = 0;
		}
		if ((uint32) b > 255) {
			if (b > 255)
				b = 255;
			else
				b = 0;
		}
		row[0] = r;
		row[1] = g;
		row[2] = b;	
	}
}

CTB_INLINE void
setrow1(int16* r1, uint8* row)
{
	uintxx i;

	for (i = 0; i < 8; i++, row++) {
		int16 r;

		r = r1[i] + 128;
		if ((uint16) r > 255) {
			if (r > 255)
				r = 255;
			else
				r = 0;
		}
		row[0] = (uint8) r;
	}
}

#endif


#if defined(JPGR_CFG_EXTERNALASM)

#define inverseDCT jpgr_inverseDCT
#define setrow1 jpgr_setrow1
#define setrow3 jpgr_setrow3

#endif

static void
setpixels1(struct TJPGRPblc* jpgr, uintxx y, uintxx x)
{
	uintxx i;
	uintxx s;
	uintxx stepx;
	int16* r1;
	int16* u1;
	struct TJPGComponent* c1;
	uintxx sy;
	uintxx sx;
	const uintxx svalue[] = {
		0, 3, 4, 0, 5
	};
	
	c1 = PRVT->components + 0;
	r1 = c1->srow;
	
	sy = svalue[PRVT->ysampling];
	sx = svalue[PRVT->xsampling];
	for (i = 0; i < PRVT->nunits; i++) {
		uintxx row;
		uintxx col;
		uintxx d1;
		uintxx i1;
		
		d1 = c1->offset[i];
		i1 = c1->iblock[i];
		u1 = c1->units[i1];
		
		row = (y << sy) + PRVT->originy[i];
		for (s = 0; s < 64; s += 8) {
			uintxx o;

			if (UNLIKELY(row >= jpgr->sizey)) {
				break;
			}
				
			col = (x << sx) + PRVT->originx[i];
			if (col + 8 <= jpgr->sizex) {
				o = (row * jpgr->sizex) + col;

				if (PRVT->issubsampled) {
					r1[0] = u1[c1->umap[s++] + d1];
					r1[1] = u1[c1->umap[s++] + d1];
					r1[2] = u1[c1->umap[s++] + d1];
					r1[3] = u1[c1->umap[s++] + d1];
					r1[4] = u1[c1->umap[s++] + d1];
					r1[5] = u1[c1->umap[s++] + d1];
					r1[6] = u1[c1->umap[s++] + d1];
					r1[7] = u1[c1->umap[s++] + d1];

					setrow1(r1, PRVT->pixels + o);
					row++;
					continue;
				}
					
				setrow1(u1 + s, PRVT->pixels + o);
				row++;
				continue;
			}
			
			for (stepx = 0; stepx < 8; stepx++) {
				int16 a1;
				uint8 r;
					
				if (col >= jpgr->sizex) {
					break;
				}
				
				if (PRVT->issubsampled) {
					a1 = u1[c1->umap[s++] + d1];
				}
				else {
					a1 = u1[s++];
				}
				r = tograyscale(a1);

				o = (row * jpgr->sizex) + col;
				PRVT->pixels[o] = r;
				col++;
			}
			row++;
		}
	}
}

/* non subsampled components */
static void
setpixels3ns(struct TJPGRPblc* jpgr, uintxx y, uintxx x)
{
	uintxx i;
	uintxx s;
	uintxx stepx;
	int16* u1;
	int16* u2;
	int16* u3;
	struct TJPGComponent* c1;
	struct TJPGComponent* c2;
	struct TJPGComponent* c3;
	uintxx torgb;
	uintxx limit;
	uintxx y8;
	uintxx x8;
	const uintxx svalue[] = {
		0, 3, 4, 0, 5
	};
	
	torgb = 1;
	if (PRVT->isrgb == 1 || PRVT->keepyuv == 1)
		torgb = 0;
	limit = jpgr->sizey * jpgr->sizex;

	c1 = PRVT->components + 0;
	c2 = PRVT->components + 1;
	c3 = PRVT->components + 2;
	
	y8 = svalue[PRVT->ysampling];
	x8 = svalue[PRVT->xsampling];
	u1 = c1->units[0];
	u2 = c2->units[0];
	u3 = c3->units[0];
	for (i = 0; i < PRVT->nunits; i++) {
		uintxx row;
		uintxx col;
		
		row = ((y << y8) + PRVT->originy[i]) * jpgr->sizex;
		for (s = 0; s < 64; s += 8) {
			uintxx o;
			
			if (UNLIKELY(row >= limit)) {
				break;
			}
			
			col = (x << x8) + PRVT->originx[i];
			if (LIKELY(col + 8 < jpgr->sizex)) {
				o = (row + col) * 3;
				setrow3(u1 + s, u2 + s, u3 + s, PRVT->pixels + o, torgb);
				row += jpgr->sizex;
				continue;
			}
			
			o = (row + col) * 3;
			for (stepx = 0; stepx < 8; stepx++) {
				int16 a1;
				int16 a2;
				int16 a3;
				struct TJPGRGB r;
				
				if (UNLIKELY(col >= jpgr->sizex)) {
					break;
				}
				a1 = u1[s + stepx];
				a2 = u2[s + stepx];
				a3 = u3[s + stepx];
				r = toRGB(a1, a2, a3, torgb);
				
				PRVT->pixels[o + 0] = r.r;
				PRVT->pixels[o + 1] = r.g;
				PRVT->pixels[o + 2] = r.b;
				o += 3;
				
				col++;
			}
			
			row += jpgr->sizex;
		}
	}
}

/* image containing subsampled components */
static void
setpixels3ss(struct TJPGRPblc* jpgr, uintxx y, uintxx x)
{
	uintxx i;
	uintxx s;
	uintxx stepx;
	int16* r1;
	int16* r2;
	int16* r3;
	int16* u1;
	int16* u2;
	int16* u3;
	struct TJPGComponent* c1;
	struct TJPGComponent* c2;
	struct TJPGComponent* c3;
	uintxx torgb;

	torgb = 1;
	if (PRVT->isrgb == 1 || PRVT->keepyuv == 1)
		torgb = 0;

	c1 = PRVT->components + 0;
	c2 = PRVT->components + 1;
	c3 = PRVT->components + 2;
	r1 = c1->srow;
	r2 = c2->srow;
	r3 = c3->srow;
	for (i = 0; i < PRVT->nunits; i++) {
		uintxx row;
		uintxx col;
		uintxx d1, d2, d3;
		uintxx i1, i2, i3;

		d1 = c1->offset[i];
		d2 = c2->offset[i];
		d3 = c3->offset[i];
		i1 = c1->iblock[i];
		i2 = c2->iblock[i];
		i3 = c3->iblock[i];
		u1 = c1->units[i1];
		u2 = c2->units[i2];
		u3 = c3->units[i3];

		row = y * (PRVT->ysampling * 8) + PRVT->originy[i];
		for (s = 0; s < 64; s += 8) {
			uintxx o;

			if (UNLIKELY(row >= jpgr->sizey)) {
				break;
			}

			col = x * (PRVT->xsampling * 8) + PRVT->originx[i];
			if (col + 8 <= jpgr->sizex) {
				int16* row1;
				int16* row2;
				int16* row3;

				o = ((row * jpgr->sizex) + col) * 3;

				/* this may be slow, but not significantly slower for most
				 * images */
				if (c1->rumode[i]) {
					r1[0] = u1[c1->umap[s + 0] + d1];
					r1[1] = u1[c1->umap[s + 1] + d1];
					r1[2] = u1[c1->umap[s + 2] + d1];
					r1[3] = u1[c1->umap[s + 3] + d1];
					r1[4] = u1[c1->umap[s + 4] + d1];
					r1[5] = u1[c1->umap[s + 5] + d1];
					r1[6] = u1[c1->umap[s + 6] + d1];
					r1[7] = u1[c1->umap[s + 7] + d1];
					row1 = r1;
				}
				else {
					row1 = u1 + c1->umap[s] + d1;
				}
				
				if (c2->rumode[i]) {
					r2[0] = u2[c2->umap[s + 0] + d2];
					r2[1] = u2[c2->umap[s + 1] + d2];
					r2[2] = u2[c2->umap[s + 2] + d2];
					r2[3] = u2[c2->umap[s + 3] + d2];
					r2[4] = u2[c2->umap[s + 4] + d2];
					r2[5] = u2[c2->umap[s + 5] + d2];
					r2[6] = u2[c2->umap[s + 6] + d2];
					r2[7] = u2[c2->umap[s + 7] + d2];
					row2 = r2;
				}
				else {
					row2 = u2 + c2->umap[s] + d2;
				}

				if (c3->rumode[i]) {
					r3[0] = u3[c3->umap[s + 0] + d3];
					r3[1] = u3[c3->umap[s + 1] + d3];
					r3[2] = u3[c3->umap[s + 2] + d3];
					r3[3] = u3[c3->umap[s + 3] + d3];
					r3[4] = u3[c3->umap[s + 4] + d3];
					r3[5] = u3[c3->umap[s + 5] + d3];
					r3[6] = u3[c3->umap[s + 6] + d3];
					r3[7] = u3[c3->umap[s + 7] + d3];
					row3 = r3;
				}
				else {
					row3 = u3 + c3->umap[s] + d3;
				}
				
				setrow3(row1, row2, row3, PRVT->pixels + o, torgb);
				row++;
				continue;
			}
			
			for (stepx = 0; stepx < 8; stepx++) {
				int16 a1;
				int16 a2;
				int16 a3;
				struct TJPGRGB r;
				
				if (UNLIKELY(col >= jpgr->sizex)) {
					break;
				}
				a1 = u1[c1->umap[s + stepx] + d1];
				a2 = u2[c2->umap[s + stepx] + d2];
				a3 = u3[c3->umap[s + stepx] + d3];
				r = toRGB(a1, a2, a3, torgb);
				
				o = (row * jpgr->sizex) + col;
				PRVT->pixels[(o * 3) + 0] = r.r;
				PRVT->pixels[(o * 3) + 1] = r.g;
				PRVT->pixels[(o * 3) + 2] = r.b;
				col++;
			}
			row++;
		}
	}
}

CTB_INLINE uintxx
checkinterval(struct TJPGRPblc* jpgr)
{
	uintxx i;
	uint16 m;
	
	if (overread(jpgr) == 1) {
		return 0;
	}
	m = readmarker(jpgr);
	if (m >> 8 != 0xff)  {
		SETERROR(JPGR_EBADDATA);
		return 0;
	}
	for (i = 0; i < PRVT->ncomponents; i++) {
		struct TJPGComponent* c;
		
		c = PRVT->components + i;
		c->cofficient = 0;
	}
	return 1;
}

static uintxx
decodebaseline(struct TJPGRPblc* jpgr)
{
	uintxx y;
	uintxx x;
	uintxx i;
	uintxx interval;
	void (*setpixels)(struct TJPGRPblc*, uintxx, uintxx);

	initbitmode(jpgr);
	interval = PRVT->rinterval;

	if (PRVT->isinterleaved == 0) {
		struct TJPGComponent* c;
		int16* block;

		c = PRVT->components + PRVT->scancomponent;
		for (y = 0; y < c->nrows; y++) {
			for (x = 0; x < c->ncols; x++) {
				if (PRVT->rinterval) {
					if (UNLIKELY(interval == 0)) {
						if (checkinterval(jpgr) == 0) {
							return 0;
						}
						initbitmode(jpgr);
						interval = PRVT->rinterval;
					}
					interval -= 1;
				}

				block = c->scan + (((y * c->icols) + x) << 6);
				if (UNLIKELY(decodeblock(jpgr, c, block) == 0)) {
					return 0;
				}
			}
		}
		return 1;
	}
	
	setpixels = setpixels1;
	if (PRVT->ncomponents == 3) {
		setpixels = setpixels3ss;
		if (PRVT->issubsampled == 0)
			setpixels = setpixels3ns;
	}
	
	if (setpixels == setpixels3ns) {
		struct TJPGComponent* c1;
		struct TJPGComponent* c2;
		struct TJPGComponent* c3;
		
		c1 = PRVT->components + PRVT->corder[0];
		c2 = PRVT->components + PRVT->corder[1];
		c3 = PRVT->components + PRVT->corder[2];
		for (y = 0; y < PRVT->nrows; y++) {
			for (x = 0; x < PRVT->ncols; x++) {
				if (PRVT->rinterval) {
					if (UNLIKELY(interval == 0)) {
						if (checkinterval(jpgr) == 0) {
							return 0;
						}
						initbitmode(jpgr);
						interval = PRVT->rinterval;
					}
					interval -= 1;
				}
				
				if (UNLIKELY(decodeblock(jpgr, c1, c1->units[0]) == 0)) {
					return 0;
				}
				if (UNLIKELY(decodeblock(jpgr, c2, c2->units[0]) == 0)) {
					return 0;
				}
				if (UNLIKELY(decodeblock(jpgr, c3, c3->units[0]) == 0)) {
					return 0;
				}
				if (LIKELY(PRVT->pixels != NULL)) {
					inverseDCT(c1->units[0], c1->units[0], c1->qtable->values);
					inverseDCT(c2->units[0], c2->units[0], c2->qtable->values);
					inverseDCT(c3->units[0], c3->units[0], c3->qtable->values);
					setpixels(jpgr, y, x);
				}
			}
		}
		return 1;
	}
	
	for (y = 0; y < PRVT->nrows; y++) {
		for (x = 0; x < PRVT->ncols; x++) {
			if (PRVT->rinterval) {
				if (UNLIKELY(interval == 0)) {
					if (checkinterval(jpgr) == 0) {
						return 0;
					}
					initbitmode(jpgr);
					interval = PRVT->rinterval;
				}
				interval -= 1;
			}

			for (i = 0; i < PRVT->ncomponents; i++) {
				uintxx j;
				struct TJPGComponent* c;
				
				c = PRVT->components + PRVT->corder[i];
				for (j = 0; j < c->ucount; j++) {
					if (UNLIKELY(decodeblock(jpgr, c, c->units[j]) == 0)) {
						return 0;
					}
					inverseDCT(c->units[j], c->units[j], c->qtable->values);
				}
			}

			if (LIKELY(PRVT->pixels != NULL)) {
				setpixels(jpgr, y, x);
			}
		}
	}
	return 1;
}

CTB_INLINE uintxx
decodefirstDC(struct TJPGRPblc* jpgr, struct TJPGComponent* c, uintxx index)
{
	int16 s;
	int16* block;

	block = c->scan + (index << 6);
	memset(block, 0, 64 * sizeof(block[0]));
	
	ensurebits(jpgr, 16);
	s = decodesymbol((void*) c->dctable, getbits(jpgr, 16));
	if (UNLIKELY(s == 0)) {
		SETERROR(JPGR_EBADCODE);
		return 0;
	}
	dropbits(jpgr, GETLENGTH(s));
	s = GETSYMBOL(s);
	
	ensurebits(jpgr, 16);
	c->cofficient += extend(s, getbits(jpgr, s));
	dropbits(jpgr, s);

	block[0] = (int16) (c->cofficient << PRVT->al);
	return 1;
}

static uintxx
readfirstDC(struct TJPGRPblc* jpgr)
{
	uintxx y;
	uintxx x;
	uintxx totaly;
	uintxx totalx;
	uintxx i;
	uintxx interval;
	uintxx sc;
	struct TJPGComponent* c;
	
	initbitmode(jpgr);
	interval = PRVT->rinterval;
	
	sc = PRVT->nscancomponents == 1;
	totaly = PRVT->nrows;
	totalx = PRVT->ncols;
	
	c = PRVT->components + PRVT->scancomponent;
	if (sc) {
		totaly = c->nrows;
		totalx = c->ncols;
	}
	
	for (y = 0; y < totaly; y++) {
		for (x = 0; x < totalx; x++) {
			if (PRVT->rinterval) {
				if (interval == 0) {
					if (checkinterval(jpgr) == 0) {
						return 0;
					}
					initbitmode(jpgr);
					interval = PRVT->rinterval;
				}
				interval -= 1;
			}
			
			if (sc) {
				if (decodefirstDC(jpgr, c, (y * totalx) + x) == 0) {
					return 0;
				}
				continue;
			}
			
			for (i = 0; i < PRVT->ncomponents; i++) {
				uintxx y2;
				uintxx x2;
				uintxx y1;
				uintxx x1;
				
				c = PRVT->components + PRVT->corder[i];
				
				y1 = y * c->ysampling;
				x1 = x * c->xsampling;
				for (y2 = 0; y2 < c->ysampling; y2++) {
					uintxx offsety;
					
					offsety = (y1 + y2) * c->icols;
					for (x2 = 0; x2 < c->xsampling; x2++) {
						if (decodefirstDC(jpgr, c, offsety + x1 + x2) == 0) {
							return 0;
						}
					}
				}
			}
		}
		if (UNLIKELY(overread(jpgr)) == 1) {
			return 0;
		}
	}

	return 1;
}

static uintxx
refineDC(struct TJPGRPblc* jpgr)
{
	uintxx y;
	uintxx x;
	uintxx totaly;
	uintxx totalx;
	uintxx i;
	uintxx interval;
	uintxx sc;
	struct TJPGComponent* c;
	int16* block;
	
	initbitmode(jpgr);
	interval = PRVT->rinterval;
	
	sc = PRVT->nscancomponents == 1;
	totaly = PRVT->nrows;
	totalx = PRVT->ncols;
	
	c = PRVT->components + PRVT->scancomponent;
	if (sc) {
		totaly = c->nrows;
		totalx = c->ncols;
	}
	
	for (y = 0; y < totaly; y++) {
		for (x = 0; x < totalx; x++) {
			if (PRVT->rinterval) {
				if (interval == 0) {
					if (checkinterval(jpgr) == 0) {
						return 0;
					}
					initbitmode(jpgr);
					interval = PRVT->rinterval;
				}
				interval -= 1;
			}
			
			if (sc) {
				block = c->scan + (((y * c->icols) + x) << 6);
				ensurebits(jpgr, 1);
				block[0] |= (int16) (getbits(jpgr, 1) << PRVT->al);
				dropbits(jpgr, 1);
				continue;
			}
			
			for (i = 0; i < PRVT->ncomponents; i++) {
				uintxx y2;
				uintxx x2;
				uintxx y1;
				uintxx x1;
				
				c = PRVT->components + PRVT->corder[i];
				
				y1 = y * c->ysampling;
				x1 = x * c->xsampling;
				for (y2 = 0; y2 < c->ysampling; y2++) {
					uintxx offsety;
					
					offsety = (y1 + y2) * c->icols;
					for (x2 = 0; x2 < c->xsampling; x2++) {
						block = c->scan + ((offsety + x1 + x2) << 6);
						
						ensurebits(jpgr, 1);
						block[0] |= (int16) (getbits(jpgr, 1) << PRVT->al);
						dropbits(jpgr, 1);
					}
				}
			}
		}
		if (UNLIKELY(overread(jpgr) == 1)) {
			return 0;
		}
	}
	
	return 1;
}

CTB_INLINE uintxx
decodefirstAC(struct TJPGRPblc* jpgr, struct TJPGComponent* c, uintxx index)
{
	uintxx i;
	uintxx symbol;
	int16  s;
	int16* block;
	struct TJPGACHmTable* ac;
	
	ac = c->actable;
	if (PRVT->eobrun > 0) {
		PRVT->eobrun -= 1;
		return 1;
	}
	
	block = c->scan + (index << 6);
	i = PRVT->ss;
	while (i <= PRVT->se) {
		uintxx a;
		uintxx b;
		
		ensurebits(jpgr, 16);
		s = decodesymbol((void*) ac, getbits(jpgr, 16));
		if (UNLIKELY(s == 0)) {
			SETERROR(JPGR_EBADCODE);
			return 0;
		}
		symbol = GETSYMBOL(s);
		dropbits(jpgr, GETLENGTH(s));
		
		a = (symbol >> 0) & 0x0f;
		b = (symbol >> 4);
		if (a == 0) {
			if (b == 15) {
				i += 16;
			}
			else {
				if (b != 0) {
					ensurebits(jpgr, b);
					PRVT->eobrun = (((uintxx) 1) << b) + getbits(jpgr, b) - 1;
					dropbits(jpgr, b);

					return 1;
				}
				break;
			}
		}
		else {
			i += b;
			if (i >= 64) {
				SETERROR(JPGR_EBADDATA);
				return 0;
			}

			ensurebits(jpgr, a);
			block[i] = (int16) extend(a, getbits(jpgr, a)) << PRVT->al;
			dropbits(jpgr, a);
			i += 1;
		}
	}
	PRVT->eobrun = 0;
	return 1;
}

static uintxx
readfirstAC(struct TJPGRPblc* jpgr)
{
	uintxx y;
	uintxx x;
	uintxx interval;
	struct TJPGComponent* c;
	
	initbitmode(jpgr);
	interval = PRVT->rinterval;
	
	c = PRVT->components + PRVT->scancomponent;
	PRVT->eobrun = 0;
	for (y = 0; y < c->nrows; y++) {
		for (x = 0; x < c->ncols; x++) {
			if (PRVT->rinterval) {
				if (interval == 0) {
					if (checkinterval(jpgr) == 0) {
						return 0;
					}
					initbitmode(jpgr);
					interval = PRVT->rinterval;
				}
				interval -= 1;
			}
			
			if (decodefirstAC(jpgr, c, (y * c->icols) + x) == 0) {
				return 0;
			}
		}
		if (UNLIKELY(overread(jpgr) == 1)) {
			return 0;
		}
	}

	return 1;
}

static int16
refine(uintxx approximation, intxx value, uintxx nextbit)
{
	if (value > 0) {
		if (nextbit == 1)
			value += (intxx) ((uintxx)  1 << approximation);
		return (int16) value;
	}
	if (value < 0) {
		if (nextbit == 1)
			value += (intxx) ((uintxx) -1 << approximation);
		return (int16) value;
	}
	return (int16) value;
}

static uintxx
decoderefineAC(struct TJPGRPblc* jpgr, struct TJPGComponent* c, uintxx index)
{
	uintxx i;
	uintxx symbol;
	int16* block;
	struct TJPGACHmTable* ac;
	int16 s;
	
	ac = c->actable;
	block = c->scan + (index << 6);
	
	i = PRVT->ss;
	if (PRVT->eobrun != 0) {
		while (i <= PRVT->se) {
			if (block[i] != 0) {
				ensurebits(jpgr, 1);
				block[i] = refine(PRVT->al, block[i], getbits(jpgr, 1));
				dropbits(jpgr, 1);
			}
			i++;
		}
		PRVT->eobrun -= 1;
		return 1;
	}
	
	while (i <= PRVT->se) {
		intxx a;
		intxx b;
		ensurebits(jpgr, 16);
		
		s = decodesymbol((void*) ac, getbits(jpgr, 16));
		if (UNLIKELY(s == 0)) {
			SETERROR(JPGR_EBADCODE);
			return 0;
		}
		symbol = GETSYMBOL(s);
		dropbits(jpgr, GETLENGTH(s));
				
		a = (symbol >> 0) & 0x0f;
		b = (symbol >> 4);
		
		if (a == 1) {
			intxx n;
			
			ensurebits(jpgr, 1);
			n = extend(1, getbits(jpgr, 1)) << PRVT->al;
			dropbits(jpgr, 1);
			while ((b > 0 || block[i] != 0)) {
				if (block[i] != 0) {
					ensurebits(jpgr, 1);
					block[i] = refine(PRVT->al, block[i], getbits(jpgr, 1));
					dropbits(jpgr, 1);
				}
				else {
					b -= 1;
				}
				i += 1;
			}
			block[i] = (int16) n;
			i += 1;
		}
		else {
			if (a == 0) {
				intxx j;
				if (b < 15) {
					ensurebits(jpgr, 16);
					PRVT->eobrun = getbits(jpgr, b) + (((uintxx) 1) << b);
					dropbits(jpgr, b);
					
					while (i <= PRVT->se) {
						if (block[i] != 0) {
							ensurebits(jpgr, 1);
							j = getbits(jpgr, 1);
							block[i] = refine(PRVT->al, block[i], j);
							dropbits(jpgr, 1);
						}
						i += 1;
					}
					PRVT->eobrun -= 1;
					return 1;
				}
				else {
					while (b >= 0) {
						if (block[i] != 0) {
							ensurebits(jpgr, 1);
							j = getbits(jpgr, 1);
							block[i] = refine(PRVT->al, block[i], j);
							dropbits(jpgr, 1);
						}
						else {
							b -= 1;
						}
						i += 1;
					}
				}
			}
			else {
				SETERROR(JPGR_EBADDATA);
				return 0;
			}
		}
	}
	
	PRVT->eobrun = 0;
	return 1;
}

static uintxx
refineAC(struct TJPGRPblc* jpgr)
{
	uintxx y;
	uintxx x;
	uintxx interval;
	struct TJPGComponent* c;
	
	initbitmode(jpgr);
	interval = PRVT->rinterval;
	
	c = PRVT->components + PRVT->scancomponent;
	PRVT->eobrun = 0;
	for (y = 0; y < c->nrows; y++) {
		for (x = 0; x < c->ncols; x++) {
			if (PRVT->rinterval) {
				if (interval == 0) {
					if (checkinterval(jpgr) == 0) {
						return 0;
					}
					initbitmode(jpgr);
					interval = PRVT->rinterval;
				}
				interval -= 1;
			}
			
			if (decoderefineAC(jpgr, c, (y * c->icols) + x) == 0) {
				return 0;
			}
		}
		if (UNLIKELY(overread(jpgr) == 1)) {
			return 0;
		}
	}
	return 1;
}

static void
updateimg(struct TJPGRPblc* jpgr)
{
	uintxx y;
	uintxx x;
	uintxx i;
	int16* temp;
	int16* unit;
	struct TJPGComponent* c;
	void (*setpixels)(struct TJPGRPblc* jpgr, uintxx y, uintxx x);

	if (UNLIKELY(PRVT->pixels == NULL)) {
		return;
	}

	setpixels = setpixels1;
	if (PRVT->ncomponents == 3) {
		setpixels = setpixels3ss;
		if (PRVT->issubsampled == 0)
			setpixels = setpixels3ns;
	}

	for (y = 0; y < PRVT->nrows; y++) {
		for (x = 0; x < PRVT->ncols; x++) {
			for (i = 0; i < PRVT->ncomponents; i++) {
				uintxx y2;
				uintxx x2;
				uintxx y1;
				uintxx x1;
				uintxx j;
				
				c = PRVT->components + i;
				
				y1 = y * c->ysampling;
				x1 = x * c->xsampling;
				j = 0;
				for (y2 = 0; y2 < c->ysampling; y2++) {
					uintxx offsety;
					
					offsety = (y1 + y2) * c->icols;
					for (x2 = 0; x2 < c->xsampling; x2++) {
						uintxx v;

						temp = c->scan + ((offsety + x1 + x2) << 6);
						if (jpgr->isprogressive == 0) {
							/* non interleaved baseline image */
							inverseDCT(temp, c->units[j], c->qtable->values);
							j++;
							continue;
						}

						unit = c->units[j];
						for (v = 0; v < 64; v++) {
							unit[zzorder[v]] = temp[v];
						}
						inverseDCT(unit, unit, c->qtable->values);
						j++;
					}
				}
			}
			setpixels(jpgr, y, x);
		}
	}
}

uintxx
jpgr_decodeimg(TJPGReader* jpgr)
{
	uintxx r;
	uintxx i;
	ASSERT(jpgr);
	
	if (jpgr->state ^ 3) {
		if (jpgr->state == 2) {
			PBLC->state++;
		}
		else {
			if (jpgr->error == 0) {
				SETERROR(JPGR_EBADUSE);
			}
			goto L_ERROR;
		}
	}
	
	if (jpgr->isprogressive) {
		while (jpgr_decodepass(jpgr, 0))
			;
		if (jpgr->error) {
			return 0;
		}
				
		updateimg(PBLC);
		return 1;
	}

	/* check for quantization tables to be defined */
	for (i = 0; i < PRVT->ncomponents; i++) {
		struct TJPGComponent* c;

		c = PRVT->components + i;
		if (c->qtable->defined == 0) {
			SETERROR(JPGR_ENOQTTABLE);
			return 0;
		}
	}

	if (PRVT->isinterleaved == 0) {
		uintxx i;
		uintxx last;
		uint8 components[4];

		r = 0;
		last = PRVT->ncomponents - 1;
		for (i = 0; i < PRVT->ncomponents; i++) {
			components[PRVT->scancomponent] = 1;
			r = decodebaseline(PBLC);
			if (r == 0) {
				break;
			}
			r = parsesegments(PBLC);
			if (r == 0) {
				break;
			}
			else {
				if (jpgr->state == 4) {
					/* premature end of file */
					if (i != last) {
						SETERROR(JPGR_EBADDATA);
						return 0;
					}
				}
			}

			if (i != last && components[PRVT->scancomponent] == 1) {
				if (jpgr->error == 0)
					SETERROR(JPGR_EBADDATA);
				break;
			}
		}

		if (r == 1) {
			if (jpgr->state != 4) {
				if (jpgr->error == 0)
					SETERROR(JPGR_EBADDATA);
				SETSTATE(5);
			}
		}
		else {
			if (parsesegments(PBLC) == 0) {
				if (jpgr->error == 0)
					SETERROR(JPGR_EBADDATA);
				SETSTATE(5);
			}
			else {
				if (jpgr->state != 4)
					SETSTATE(5);
			}
		}

		updateimg(PBLC);
		return 1;
	}
	
	if (decodebaseline(PBLC)) {
		if (parsesegments(PBLC) == 0) {
			SETSTATE(5);
		}
		else {
			SETSTATE(4);
		}
		return 1;
	}

L_ERROR:
	SETSTATE(JPGR_BADSTATE);
	return 0;
}

void
jpgr_updateimg(TJPGReader* jpgr)
{
	ASSERT(jpgr);
		
	if (jpgr->isprogressive == 0) {
		return;
	}
	if (jpgr->state != 4 && jpgr->state != 3) {
		if (jpgr->error == 0) {
			SETERROR(JPGR_EBADUSE);
			SETSTATE(JPGR_BADSTATE);
			return;
		}
	}
	
	updateimg(PBLC);
}

uintxx
jpgr_decodepass(TJPGReader* jpgr, bool update)
{
	uintxx r;
	uintxx i;

	if (jpgr->state ^ 3) {
		if (jpgr->state == 2) {
			PBLC->state++;
		}
		else {
			SETSTATE(JPGR_BADSTATE);
			if (jpgr->error == 0) {
				SETERROR(JPGR_EBADUSE);
			}
			return 0;
		}
	}

	if (PRVT->npass == 0) {
		for (i = 0; i < PRVT->ncomponents; i++) {
			struct TJPGComponent* c;

			c = PRVT->components + i;
			if (c->qtable->defined == 0) {
				SETERROR(JPGR_ENOQTTABLE);
				goto L_ERROR;
			}
		}
	}

	if (PRVT->ss == 0) {
		if (PRVT->se != 0) {
			SETERROR(JPGR_EINVALIDPASS);
			goto L_ERROR;
		}
		if (PRVT->ah == 0) {
			if (readfirstDC(PBLC) == 0) {
				goto L_ERROR;
			}
		}
		else {
			if (refineDC(PBLC) == 0) {
				goto L_ERROR;
			}
		}
	}
	else {
		if (PRVT->nscancomponents != 1) {
			SETERROR(JPGR_EINVALIDPASS);
			goto L_ERROR;
		}
		
		if (PRVT->ah == 0) {
			if (readfirstAC(PBLC) == 0) {
				goto L_ERROR;
			}
		}
		else {
			if (refineAC(PBLC) == 0) {
				goto L_ERROR;
			}
		}
	}
	
	if (update) {
		updateimg(PBLC);
	}

	r = parsesegments(PBLC);
	if (r) {
		if (jpgr->state == 4) {
			/* end of file */
			return 0;
		}

		PRVT->npass++;
		if (PRVT->npass > JPGR_MAXPASSES) {
			SETERROR(JPGR_EPASSLIMIT);
			return 0;
		}
		return PRVT->npass;
	}

L_ERROR:
	if (jpgr->error == 0)
		SETERROR(JPGR_EBADDATA);
	SETSTATE(JPGR_BADSTATE);
	return 0;
}


#undef SETERROR
#undef SETSTATE
#undef PBLC
#undef PRVT
