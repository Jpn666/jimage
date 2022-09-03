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

#include "../pngreader.h"
#include <inflator.h>

#if defined(PNGR_CFG_DOCRC)
	#define DOCRC 1
#else
	#define DOCRC 0
#endif

#if DOCRC
	#include <crypto/crc32.h>
#endif


/* chunk size limit for ICCP, ITXT, ZTXT and TEXT chunks or unknown
 * chunks (8MB) */
#define MAXCHUNKSIZE 0x800000L

/* ICC profile size limit */
#define MAXICCPSIZE  0x800000L


/* image size limit 4GB on 64bit or 2GB on 32bit platform */
#if defined(CTB_ENV64)
	#define MAXSAFESIZE 0x100000000LL
#else
	#define MAXSAFESIZE 0x080000000LL
#endif


/* private stuff */
struct TPNGRPrvt {
	/* public fields */
	struct TPNGRPblc hidden;

	uintxx hasalpha;

	/* to check chunk presence and order */
	struct TPNGRChunkMap {
		uintxx PLTE: 1;
		uintxx SBIT: 1;
		uintxx BKGD: 1;
		uintxx GAMA: 1;
		uintxx ICCP: 1;
		uintxx PHYS: 1;
		uintxx CHRM: 1;
		uintxx TRNS: 1;
		uintxx SRGB: 1;
	} chunkmap;

	uintxx docrc;
	uint32 crc32;

	/* row buffers */
	uint8* currrow;
	uint8* prevrow;
	uint8* rbuffers[2];

	/* raw scanline before decoding, including the filter byte */
	uintxx rawrowsize;
	uintxx rawpelsize;

	/* required memory for each row (including padding bytes) */
	uintxx rowmemory;

	/* the final imagemap row size and pixel size */
	uintxx rowsize;
	uintxx pelsize;

	/* data buffers */
	uint8* pixels;
	uint8* idxs;

	/* internal memory */
	uint8* memory;
	uintxx memorysize;

	/* progressive pass */
	uintxx interpolate;
	uintxx pass;
	uintxx passmemsize[7];
	uintxx passrowsize[7];

	/* allocated memory for the ICC profile (if any) */
	uint8* iccpmemory;
	uintxx iccpsize;

	/* input callback */
	TIMGInputFn inputfn;

	/* input callback parameter */
	void* payload;

	/* inflate state */
	struct TInflator* inflator;

	uintxx inputsize;
	uintxx remaining;  /* chunk remaining bytes */

	/* last inflate call result */
	uintxx result;

	/* raw and uncompressed buffer */
	uint8* tbgn;
	uint8* tend;
	uint8 source[4096];
	uint8 target[4096];

	/* custom allocator */
	struct TAllocator* allocator;
};


/* private and public cast, we only need PBLC to set values, only in the
 * exported functions */
#define PBLC ((struct TPNGRPblc*) pngr)
#define PRVT ((struct TPNGRPrvt*) pngr)


CTB_INLINE void*
reserve(struct TPNGRPrvt* p, uintxx amount)
{
	if (p->allocator) {
		return p->allocator->reserve(p->allocator->user, amount);
	}
	return CTB_MALLOC(amount);
}

CTB_INLINE void
release(struct TPNGRPrvt* p, void* memory)
{
	if (p->allocator) {
		p->allocator->release(p->allocator->user, memory);
		return;
	}
	CTB_FREE(memory);
}

TPNGReader*
pngr_create(ePNGRFlags flags, TAllocator* allocator)
{
	struct TPNGRPblc* pngr;

	if (allocator) {
		pngr = allocator->reserve(allocator->user, sizeof(struct TPNGRPrvt));
	}
	else {
		pngr = CTB_MALLOC(sizeof(struct TPNGRPrvt));
	}
	if (pngr == NULL) {
		return NULL;
	}
	PRVT->allocator  = allocator;

	if ((PRVT->inflator = inflator_create(allocator)) == NULL) {
		release(PRVT, pngr);
		return NULL;
	}
	PRVT->iccpmemory = NULL;
	PRVT->memory     = NULL;
	pngr_reset(pngr, 0);

	PBLC->flags = flags;
	return pngr;
}


#define SETERROR(ERROR) (PBLC->error = (ERROR))
#define SETSTATE(STATE) (PBLC->state = (STATE))

void
pngr_reset(TPNGReader* pngr, bool fullreset)
{
	uintxx i;
	ASSERT(pngr);

	/* state */
	PBLC->state = 0;
	PBLC->error = 0;
	PBLC->warnings   = 0;
	PBLC->properties = 0;

	/* header */
	PBLC->sizex = 0;
	PBLC->sizey = 0;

	PBLC->colortype = 0;
	PBLC->depth     = 0;
	PBLC->requiredmemory = 0;

	PBLC->compression = 0;
	PBLC->filter      = 0;
	PBLC->interlace   = 0;

	PBLC->palettesize = 0;
	for (i = 0; i < sizeof(PBLC->palette); i++) {
		PBLC->palette[i] = 0;
	}

	PBLC->alpha[0] = 0;
	PBLC->alpha[1] = 0;
	PBLC->alpha[2] = 0;
	PBLC->background[0] = 0;
	PBLC->background[1] = 0;
	PBLC->background[2] = 0;

	for (i = 0; i < sizeof(PBLC->sbits); i++) {
		PBLC->sbits[i] = 0;
	}

	PBLC->gamma = 0.f;

	PBLC->wpointx = 0.f;
	PBLC->wpointy = 0.f;
	PBLC->chromax[0] = PBLC->chromay[0] = 0.f;
	PBLC->chromax[1] = PBLC->chromay[1] = 0.f;
	PBLC->chromax[2] = PBLC->chromay[1] = 0.f;

	PBLC->srgbintent = 0;

	PBLC->iccpname[0] = 0;
	PBLC->iccprofile  = NULL;
	PBLC->iccpsize    = 0;
	PBLC->iccpchecksum = 0;

	PBLC->physx = 0;
	PBLC->physy = 0;
	PBLC->physunit = 0;

	/* private stuff */
	PRVT->chunkmap = (struct TPNGRChunkMap) {0, 0, 0, 0, 0, 0, 0, 0, 0};
	PRVT->hasalpha = 0;

	PRVT->crc32 = 0;
	PRVT->docrc = 0;

	PRVT->interpolate = 0;
	PRVT->pass = 0;

	PRVT->pixels = NULL;
	PRVT->idxs   = NULL;
	if (PRVT->memory) {
		if (fullreset) {
			release(PRVT, PRVT->memory);
			PRVT->memory     = NULL;
			PRVT->memorysize = 0;
		}
	}
	else {
		PRVT->memorysize = 0;
	}

	if (PRVT->iccpmemory) {
		if (fullreset) {
			release(PRVT, PRVT->iccpmemory);
			PRVT->iccpmemory = NULL;
			PRVT->iccpsize   = 0;
		}
	}
	else {
		PRVT->iccpsize = 0;
	}

	PRVT->inputsize = 0;
	PRVT->remaining = 0;
	PRVT->source[0] = 0xff;

	PRVT->payload = NULL;
	PRVT->inputfn = NULL;
	inflator_reset(PRVT->inflator);
}

void
pngr_destroy(TPNGReader* pngr)
{
	if (pngr == NULL) {
		return;
	}

	if (PRVT->memory) {
		release(PRVT, PRVT->memory);
	}
	if (PRVT->iccpmemory) {
		release(PRVT, PRVT->iccpmemory);
	}
	inflator_destroy(PRVT->inflator);
	release(PRVT, PBLC);
}

void
pngr_setinputfn(TPNGReader* pngr, TIMGInputFn fn, void* user)
{
	ASSERT(pngr);

	if (pngr->state) {
		SETSTATE(PNGR_BADSTATE);
		if (pngr->error == 0) {
			SETERROR(PNGR_EINCORRECTUSE);
		}
		return;
	}

	PRVT->inputfn = fn;
	PRVT->payload = user;
}

CTB_INLINE bool
readinput(struct TPNGRPblc* pngr, uint8* buffer, uintxx size)
{
	intxx r;

	r = PRVT->inputfn(buffer, size, PRVT->payload);
	if (UNLIKELY(r ^ size)) {
		static const uintxx error[] = {
			PNGR_EBADDATA,
			PNGR_EIOERROR
		};

		SETERROR(error[((uintxx) r) >> ((sizeof(intxx) << 3) - 1)]);
		return 0;
	}

#if DOCRC
	if (PRVT->docrc) {
		PRVT->crc32 = crc32_update(PRVT->crc32, buffer, size);
	}
#endif
	return 1;
}


#define TOI32(A, B, C, D) ((A << 0x18) | (B << 0x10) | (C << 0x08) | (D))
#define TOI16(A, B)       ((A << 0x08) | (B))


/* chunk header */
struct TChunkHead {
	uint32 length;

	/* fourcc */
	uint8  fcc[4];
};

CTB_INLINE struct TChunkHead
getchunkhead(struct TPNGRPblc* pngr)
{
	struct TChunkHead head;
	uint8 s[8];

	s[7] = 0x00;
	if (readinput(pngr, s, 8) == 0) {
		return (struct TChunkHead) {0, {0, 0, 0, 0}};
	}

	head.length = TOI32(s[0], s[1], s[2], s[3]);
	if (head.length > 0x7fffffff) {
		SETERROR(PNGR_EBADDATA);
		return (struct TChunkHead) {0, {0, 0, 0, 0}};
	}
	head.fcc[0] = s[4];
	head.fcc[1] = s[5];
	head.fcc[2] = s[6];
	head.fcc[3] = s[7];

	return head;
}


/* Initial crc32 value for each chunk */
#define CRC32_IHDR 0x575E51F5
#define CRC32_IEND 0x51BD9F7D
#define CRC32_IDAT 0xCA50F9E1
#define CRC32_PLTE 0xB45776AA
#define CRC32_SBIT 0x9992179E
#define CRC32_TRNS 0xC9468F33
#define CRC32_CHRM 0x0941A68D
#define CRC32_GAMA 0x4D1E48E0
#define CRC32_ICCP 0x5A76E6C1
#define CRC32_SRGB 0xEFE32C31
#define CRC32_BKGD 0xFDD22101
#define CRC32_PHYS 0x5216BA54


#if DOCRC

CTB_INLINE void
initcrc32(struct TPNGRPblc* pngr, uint32 crc)
{
	PRVT->crc32 = crc;
	if ((pngr->flags & PNGR_NOCRCCHECK) == 0)
		PRVT->docrc = 1;
}

#endif

#if DOCRC
	#define INITCRC32(R, CRC) initcrc32((R), (CRC))
#else
	#define INITCRC32(R, CRC)
#endif


CTB_INLINE void
checkcrc32(struct TPNGRPblc* pngr)
{
	uint8 s[4];
#if DOCRC
	bool check;

	check = PRVT->docrc;
	PRVT->docrc = 0;
	if (readinput(pngr, s, 4)) {
		if (check) {
			uint32 crc32;

			crc32 = TOI32(s[0], s[1], s[2], s[3]);
			if (crc32 != (PRVT->crc32 ^ 0xffffffff)) {
				SETERROR(PNGR_EBADCRC);
			}
		}
	}
#else
	readinput(pngr, s, 4);
#endif
}


CTB_INLINE bool
checksignature(struct TPNGRPblc* pngr)
{
	static const uint8 signature[8] = {
		0x89,
		0x50,
		0x4e,
		0x47,
		0x0d,
		0x0a,
		0x1a,
		0x0a
	};
	uint8 s[8];
	intxx i;

	s[7] = 0x00;
	if (readinput(pngr, s, 8) == 0) {
		return 0;
	}

	for (i = 0; i < 8; i++) {
		if (s[i] ^ signature[i]) {
			SETERROR(PNGR_EINVALIDIMAGE);
			return 0;
		}
	}
	return 1;
}

CTB_INLINE bool
isvalidmode(uintxx colordepth, uintxx colortype)
{
	switch (colortype) {
		case 0:
			if (colordepth == 1 ||
				colordepth == 2 || colordepth == 4) {
				return 1;
			}
			/* fallthrough */
		case 2:
		case 4:
		case 6:
			if (colordepth == 8 || colordepth == 16) {
				return 1;
			}
			break;
		case 3:
			if (colordepth == 1 ||
				colordepth == 2 ||
				colordepth == 4 || colordepth == 8) {
				return 1;
			}
	}
	return 0;
}

static bool
parseIHDR(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uint8 *s;

	INITCRC32(pngr, CRC32_IHDR);
	s = PRVT->source;
	if (head.length ^ 13 || readinput(pngr, s, 13) == 0) {
		return 0;
	}

	pngr->sizex = TOI32(s[0], s[1], s[2], s[3]);
	pngr->sizey = TOI32(s[4], s[5], s[6], s[7]);
	if (pngr->sizey == 0 || pngr->sizey > 0x7fffffff ||
		pngr->sizex == 0 || pngr->sizex > 0x7fffffff) {
		goto L_ERROR;
	}

	/* image properties */
	pngr->depth       = s[ 8];
	pngr->colortype   = s[ 9];
	pngr->compression = s[10];
	pngr->filter      = s[11];
	pngr->interlace   = s[12];

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}

	if (isvalidmode(pngr->depth, pngr->colortype)) {
		/* only compresion method 0 is defined by the standard (deflate), the
		 * same for filter method */
		if ((pngr->compression | pngr->filter) || pngr->interlace > 1) {
			goto L_ERROR;
		}

		/* set the color palette to red */
		if (pngr->colortype == 3) {
			uintxx i;

			for (i = 0; i < 256; i++) {
				pngr->palette[(i * 4) + 0] = 0xff;
				pngr->palette[(i * 4) + 1] = 0x00;
				pngr->palette[(i * 4) + 2] = 0x00;
				pngr->palette[(i * 4) + 3] = 0xff;
			}
		}
		return 1;
	}

L_ERROR:
	SETERROR(PNGR_EBADDATA);
	return 0;
}

CTB_INLINE bool
readzlibheader(struct TPNGRPblc* pngr)
{
	uintxx i;
	uintxx cm;
	uintxx ci;
	uint8 s[2];

	/* since it's part of an IDAT chuck these 2 bytes can be on diferent
	 * chunks */
	for (i = 0; i < 2;) {
		if (PRVT->remaining) {
			if (readinput(pngr, s + i, 1)) {
				PRVT->remaining--;
				i++;
				continue;
			}
			return 0;
		}
		else {
			struct TChunkHead head;

			checkcrc32(pngr);
			if (pngr->error) {
				return 0;
			}
			head = getchunkhead(pngr);
			if (head.fcc[0] != 'I' ||
				head.fcc[1] != 'D' ||
				head.fcc[2] != 'A' || head.fcc[3] != 'T') {
				if (pngr->error == 0)
					SETERROR(PNGR_EBADDATA);
				return 0;
			}

			INITCRC32(pngr, CRC32_IDAT);
			PRVT->remaining = head.length;
		}
	}

	/* check the zlib header */
	cm = (s[0] >> 0) & 0x0f;
	ci = (s[0] >> 4) & 0x0f;
	if (cm == 0 && ci <= 7) {
		uintxx fdict;
		uintxx fchck;

		fchck = (s[0] << 8) | s[1];
		if (fchck % 31) {
			goto L_ERROR;
		}
		fdict = (s[1] >> 5) & 0x01;
		if (fdict) {
			goto L_ERROR;
		}
		return 1;
	}

L_ERROR:
	SETERROR(PNGR_EBADDATA);
	return 0;
}


/* critical chunks */
static bool parsePLTE(struct TPNGRPblc*, struct TChunkHead);

/* ancillary chunks */
static bool parseSBIT(struct TPNGRPblc*, struct TChunkHead);
static bool parseTRNS(struct TPNGRPblc*, struct TChunkHead);
static bool parseCHRM(struct TPNGRPblc*, struct TChunkHead);
static bool parseGAMA(struct TPNGRPblc*, struct TChunkHead);
static bool parseICCP(struct TPNGRPblc*, struct TChunkHead);
static bool parseSRGB(struct TPNGRPblc*, struct TChunkHead);
static bool parseBKGD(struct TPNGRPblc*, struct TChunkHead);
static bool parsePHYS(struct TPNGRPblc*, struct TChunkHead);

static bool parseancillary(struct TPNGRPblc*, uint32, struct TChunkHead);


uintxx
parsechunks(struct TPNGRPblc* pngr)
{
	uint32 fcc;

	for (;;) {
		struct TChunkHead head;

		head = getchunkhead(pngr);
		if (pngr->error) {
			/* failed to read the chunk head */
			return 0;
		}

		fcc = TOI32(head.fcc[0], head.fcc[1], head.fcc[2], head.fcc[3]);
		switch (fcc) {
			case TOI32('I', 'E', 'N', 'D'):
				if (pngr->state == 4) {
					if (pngr->error) {
						return 0;
					}

					INITCRC32(pngr, CRC32_IEND);
					checkcrc32(pngr);
					if (pngr->error) {
						return 0;
					}
					return 1;
				}
				SETERROR(PNGR_ECHUNKORDER);
				return 0;

			case TOI32('I', 'H', 'D', 'R'):
				SETERROR(PNGR_EDUPLICATEDCHUNK);
				return 0;

			case TOI32('P', 'L', 'T', 'E'):
				if (parsePLTE(pngr, head) == 0) {
					if (pngr->error == 0)
						SETERROR(PNGR_EBADDATA);
					return 0;
				}
				continue;

			case TOI32('I', 'D', 'A', 'T'):
				if (pngr->state == 4) {
					SETERROR(PNGR_ECHUNKORDER);
					return 0;
				}

				if (pngr->colortype == 3) {
					if (pngr->palettesize == 0) {
						SETERROR(PNGR_EMISSINGCHUNK);
						return 0;
					}
				}

				INITCRC32(pngr, CRC32_IDAT);
				PRVT->remaining = head.length;
				if (readzlibheader(pngr)) {
					return 1;
				}
				return 0;

			default:
				break;
		}

		if (parseancillary(pngr, fcc, head) == 0) {
			if (pngr->error == 0) {
				SETERROR(PNGR_EBADDATA);
			}
			break;
		}
	}
	return 0;
}

CTB_INLINE bool
consumechunk(struct TPNGRPblc* pngr, uintxx total)
{
	uintxx j;

	while (total) {
		j = sizeof(PRVT->source);
		if (j > total)
			j = total;

		if (readinput(pngr, PRVT->source, j) == 0) {
			return 0;
		}
		total -= j;
	}
	return 1;
}

static bool
parseancillary(struct TPNGRPblc* pngr, uint32 fcc, struct TChunkHead head)
{
	switch (fcc) {
		/* ancillary chunks */
		case TOI32('t', 'R', 'N', 'S'):
			if (parseTRNS(pngr, head) == 0) {
				return 0;
			}
			return 1;

		case TOI32('c', 'H', 'R', 'M'):
			if (parseCHRM(pngr, head) == 0) {
				return 0;
			}
			return 1;

		case TOI32('g', 'A', 'M', 'A'):
			if (parseGAMA(pngr, head) == 0) {
				return 0;
			}
			return 1;

		case TOI32('i', 'C', 'C', 'P'):
			if (parseICCP(pngr, head) == 0) {
				return 0;
			}
			return 1;

		case TOI32('s', 'B', 'I', 'T'):
			if (parseSBIT(pngr, head) == 0) {
				return 0;
			}
			return 1;

		case TOI32('s', 'R', 'G', 'B'):
			if (parseSRGB(pngr, head) == 0) {
				return 0;
			}
			return 1;

		case TOI32('b', 'K', 'G', 'D'):
			if (parseBKGD(pngr, head) == 0) {
				return 0;
			}
			return 1;

		case TOI32('p', 'H', 'Y', 'S'):
			if (parsePHYS(pngr, head) == 0) {
				return 0;
			}
			return 1;

		default:
			/* we don't care about duplicated or malformed chunks that we
			 * don't neeed to parse to recompose the final image.
			 * hIST
			 * sPLT
			 * tIME
			 * iTXT
			 * tEXT
			 * zTXT */
			break;
	}

	if (head.length) {
		if (head.length > MAXCHUNKSIZE) {
			SETERROR(PNGR_ELIMIT);
			return 0;
		}
		if (consumechunk(pngr, head.length) == 0) {
			return 0;
		}
	}
	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}
	return 1;
}


#define ADAM7PASSES 7

/* adam 7 pass */
struct TPassInfo {
	/* pass origin */
	uint8 originx;
	uint8 originy;

	/* pass step */
	uint8 stepx;
	uint8 stepy;
};

static const struct TPassInfo passinfo[] = {
	{0, 0, 8, 8},
	{4, 0, 8, 8},
	{0, 4, 4, 8},
	{2, 0, 4, 4},
	{0, 2, 2, 4},
	{1, 0, 2, 2},
	{0, 1, 1, 2}
};

#define ORIGIN_X(PASS) passinfo[PASS].originx
#define ORIGIN_Y(PASS) passinfo[PASS].originy

#define STEP_X(PASS) passinfo[PASS].stepx
#define STEP_Y(PASS) passinfo[PASS].stepy


CTB_INLINE void
setuppasses(struct TPNGRPblc* pngr)
{
	uintxx sizex;
	uintxx sizey;
	uintxx i;

	for (i = 0; i < ADAM7PASSES; i++) {
		uint8 shiftx[] = {3, 3, 2, 2, 1, 1, 0};
		uint8 shifty[] = {3, 3, 3, 2, 2, 1, 1};

		sizex = (pngr->sizex + STEP_X(i) - ORIGIN_X(i) - 1) >> shiftx[i];
		sizey = (pngr->sizey + STEP_Y(i) - ORIGIN_Y(i) - 1) >> shifty[i];
		if (sizex == 0 || sizey == 0) {
			PRVT->passrowsize[i] = 0;
			continue;
		}

		PRVT->passrowsize[i] = sizex;
		if (pngr->depth < 8) {
			uint64 v;

			v = ((uint64) pngr->depth * sizex) + 7;
			PRVT->passmemsize[i] = (uintxx) (v >> 3) + 1;
			continue;
		}

		PRVT->passmemsize[i] = sizex * PRVT->rawpelsize;
		PRVT->passmemsize[i]++;
	}
	PRVT->interpolate = 1;
}

CTB_INLINE bool
checklimits(uintxx sizex, uintxx sizey, uintxx pelsize)
{
	uint64 v;

	/* internal memory */
	v = sizex * pelsize;
	if (v > (MAXSAFESIZE >> 2)) {
		return 0;
	}
	v = v * 2;
	if (v > (MAXSAFESIZE >> 2)) {
		return 0;
	}

	/* image size */
	v = sizex * sizey;
	if (v > MAXSAFESIZE) {
		return 0;
	}
	v = v * pelsize;
	if (v > MAXSAFESIZE) {
		return 0;
	}
	return 1;
}

CTB_INLINE bool
setvalues(struct TPNGRPblc* pngr, struct TImageInfo* info)
{
	static const uintxx cmap[] = {
		1, 0, 3, 1, 2, 0, 4
	};
	uintxx pelsize;
	uintxx mode;
	uintxx r;

	switch (pngr->colortype) {
		case 0:
			if (PRVT->hasalpha) {
				mode = IMAGE_GRAYALPHA;
				break;
			}
			mode = IMAGE_GRAY;
			break;
		case 2:
		case 3:
			if (PRVT->hasalpha) {
				mode = IMAGE_RGBALPHA;
				break;
			}
			mode = IMAGE_RGB;
			break;
		case 4:
			mode = IMAGE_GRAYALPHA;
			break;
		case 6:
			mode = IMAGE_RGBALPHA;
			break;
		default:
			mode = 0;
	}

	r = pelsize = cmap[pngr->colortype];
	if (pngr->colortype == 3) {
		pelsize += 2;
	}
	if (PRVT->hasalpha) {
		pelsize++;
	}

	if (pngr->depth == 16) {
		pelsize = pelsize << 1;
	}
	if (checklimits(pngr->sizex, pngr->sizey, pelsize) == 0) {
		return 0;
	}
	r = r * ((pngr->depth + 7) >> 3);

	PRVT->rawrowsize = PRVT->rowmemory = (pngr->sizex * r) + 1;
	PRVT->rawpelsize = r;
	if (pngr->depth < 8) {
		uint64 v;

		v = ((uint64) pngr->depth * pngr->sizex) + 7;
		PRVT->rawrowsize = (uintxx) (v >> 3) + 1;

		/* extra bytes to do bit expansion on the row */
		switch (pngr->depth) {
			case 1: PRVT->rowmemory += (8 - 1); break;
			case 2: PRVT->rowmemory += (4 - 1); break;
			case 4: PRVT->rowmemory += (2 - 1); break;
			default:
				break;
		}
	}

	/* extra bytes */
	PRVT->rowmemory += 16;

	PRVT->rowsize = pelsize * pngr->sizex;
	PRVT->pelsize = pelsize;

	/* sets the imageinfo struct */
	info->sizex = pngr->sizex;
	info->sizey = pngr->sizey;
	info->colortype = mode;

	info->depth = 8;
	if (pngr->depth == 16) {
		info->depth = 16;
	}
	info->size = imginfo_getrowsize(info) * pngr->sizey;

	return 1;
}

uintxx
pngr_initdecoder(TPNGReader* pngr, TImageInfo* info)
{
	ASSERT(pngr && info);

	if (pngr->state) {
		if (pngr->error == 0) {
			SETERROR(PNGR_EINCORRECTUSE);
		}
		goto L_ERROR;
	}

	/* at this point we need an input function */
	if (PRVT->inputfn == NULL) {
		SETERROR(PNGR_EIOERROR);
		goto L_ERROR;
	}

	if (checksignature(PBLC)) {
		struct TChunkHead head;

		head = getchunkhead(PBLC);
		if (head.fcc[0] != 'I' ||
			head.fcc[1] != 'H' ||
			head.fcc[2] != 'D' || head.fcc[3] != 'R') {
			if (pngr->error == 0) {
				SETERROR(PNGR_EBADDATA);
			}
			goto L_ERROR;
		}

		/* parse all the chunks until the first IDAT chunk */
		if (parseIHDR(PBLC, head) && parsechunks(PBLC)) {
			if (setvalues(PBLC, info)) {
				if (pngr->interlace) {
					setuppasses(PBLC);
				}

				/* sets the decompression stuff ready */
				PRVT->tbgn = PRVT->target;
				PRVT->tend = PRVT->target;

				PRVT->result = INFLT_SRCEXHSTD;

				/* ready to start decoding */
				SETSTATE(1);
				PBLC->requiredmemory = PRVT->rowmemory << 1;
				return 1;
			}

			SETERROR(PNGR_ELIMIT);
		}
	}

L_ERROR:
	if (pngr->error == 0)
		SETERROR(PNGR_EBADDATA);
	SETSTATE(PNGR_BADSTATE);
	return 0;
}

void
pngr_setbuffers(TPNGReader* pngr, uint8* pixels, uint8* idxs)
{
	uintxx i;
	uint8* memory;
	ASSERT(pngr);

	if (pngr->state ^ 1) {
		SETSTATE(PNGR_BADSTATE);
		if (pngr->error == 0) {
			SETERROR(PNGR_EINCORRECTUSE);
		}
		return;
	}

	memory = NULL;
	if (PRVT->memory) {
		if (PRVT->memorysize > PBLC->requiredmemory) {
			memory = PRVT->memory;
		}
		else {
			release(PRVT, PRVT->memory);
			PRVT->memory     = NULL;
			PRVT->memorysize = 0;
		}
	}

	if (memory == NULL) {
		memory = reserve(PRVT, PBLC->requiredmemory);
		if (memory == NULL) {
			SETSTATE(PNGR_BADSTATE);
			SETERROR(PNGR_EOOM);
			return;
		}
		PRVT->memory = memory;
	}

	PRVT->rbuffers[0] = memory;
	PRVT->rbuffers[1] = memory + PRVT->rowmemory;

	PRVT->currrow = PRVT->rbuffers[0];
	PRVT->prevrow = PRVT->rbuffers[1];
	if (pngr->interlace == 0) {
		for (i = 0; i < PRVT->rowmemory; i++) {
			PRVT->prevrow[i] = 0;
		}
	}

	PRVT->pixels = pixels;
	if (pngr->colortype == 3) {
		PRVT->idxs = idxs;
	}
	SETSTATE(2);
}

static bool
parsePLTE(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uint8* s;
	uintxx psize;
	uintxx limit;
	uintxx i;
	uintxx j;

	if (PRVT->chunkmap.PLTE) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	PRVT->chunkmap.PLTE = 1;

	/* it shall not appear for colour types 0 and 4 */
	if (pngr->colortype == 0 || pngr->colortype == 4) {
		return 0;
	}
	if (head.length > 0x300) {
		return 0;
	}

	/* a chunk length not divisible by 3 is an error */
	psize = (head.length * 0x156) >> 10;
	if (psize == 0 || head.length - (psize * 3)) {
		return 0;
	}

	/* the number of palette entries shall not exceed the range that can be
	 * represented in the image bit depth */
	limit = 0xff;
	if (pngr->colortype == 3) {
		limit = ((uintxx) 1) << pngr->depth;
	}
	if (psize > limit) {
		return 0;
	}

	INITCRC32(pngr, CRC32_PLTE);

	s = pngr->palette;
	pngr->palettesize = psize;
	if (readinput(pngr, s, head.length) == 0) {
		return 0;
	}

	/* expand the palette from RGB to RGBA */
	j = psize * 4;
	i = psize * 3;
	while (i) {
		pngr->palette[--j] = 0xff;
		pngr->palette[--j] = pngr->palette[--i];
		pngr->palette[--j] = pngr->palette[--i];
		pngr->palette[--j] = pngr->palette[--i];
	}

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}
	return 1;
}


#define SETPROPERTY(P) (pngr->properties |= (P))
#define  ADDWARNING(W) (pngr->warnings   |= (W))

static bool
parseTRNS(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uint8 *s;

	if (PRVT->chunkmap.TRNS) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->colortype == 3) {
			if (PRVT->chunkmap.PLTE == 0) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	PRVT->chunkmap.TRNS = 1;

	/* a tRNS chunk shall not appear for colour types 4 and 6, since a full
	 * alpha channel is already present in those cases */
	if (pngr->colortype == 4 || pngr->colortype == 6) {
		return 0;
	}

	INITCRC32(pngr, CRC32_TRNS);
	s = PRVT->source;
	if (pngr->colortype == 3) {
		intxx i;

		/* the tRNS chunk shall not contain more alpha values than there are
		 * palette entries, but a tRNS chunk may contain fewer values than
		 * there are palette entries */
		if (pngr->palettesize == 0 || head.length > pngr->palettesize) {
			return 0;
		}

		if (readinput(pngr, s, head.length) == 0) {
			return 0;
		}
		for (i = 0; (intxx) head.length > i; i++) {
			pngr->palette[(i * 4) + 3] = s[i];
		}
	}
	else {
		if (pngr->colortype == 0) {
			if (head.length != 2 || readinput(pngr, s, 2) == 0) {
				return 0;
			}

			if (pngr->colortype ^ 16) {
				pngr->alpha[0] = (uint16) s[1];
			}
			else {
				pngr->alpha[0] = TOI16(s[0], s[1]);
			}
			/* unused values are zero */
		}
		if (pngr->colortype == 2) {
			if (head.length != 6 || readinput(pngr, s, 6) == 0) {
				return 0;
			}

			if (pngr->colortype ^ 16) {
				pngr->alpha[0] = (uint16) s[1];
				pngr->alpha[1] = (uint16) s[3];
				pngr->alpha[2] = (uint16) s[5];
			}
			else {
				pngr->alpha[0] = TOI16(s[0], s[1]);
				pngr->alpha[1] = TOI16(s[2], s[3]);
				pngr->alpha[2] = TOI16(s[4], s[5]);
			}
		}

		SETPROPERTY(PNGR_TRNS);
	}
	PRVT->hasalpha = 1;

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}
	return 1;
}

static bool
parseCHRM(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uintxx i;
	uint32 a;
	uint32 b;
	uint8* s;

	if (PRVT->chunkmap.CHRM) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->colortype == 3) {
			if (PRVT->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	PRVT->chunkmap.CHRM = 1;

	INITCRC32(pngr, CRC32_CHRM);
	s = PRVT->source;
	if (head.length != 32 || readinput(pngr, s, 32) == 0) {
		return 0;
	}

	/* each value is encoded as a four-byte PNG unsigned integer,
	 * representing the x or y value times 100000 */
	a = TOI32(s[0], s[1], s[2], s[3]); s += 4;
	b = TOI32(s[0], s[1], s[2], s[3]); s += 4;
	pngr->wpointx = a * 0.00001f;
	pngr->wpointy = b * 0.00001f;

	if (a == 0 || b == 0) {
		ADDWARNING(PNGR_BADCHRM);
	}

	for (i = 0; i < 3; i++) {
		a = TOI32(s[0], s[1], s[2], s[3]); s += 4;
		b = TOI32(s[0], s[1], s[2], s[3]); s += 4;

		pngr->chromax[i] = a * 0.00001f;
		pngr->chromay[i] = b * 0.00001f;
		if (a == 0 || b == 0) {
			ADDWARNING(PNGR_BADCHRM);
		}
	}

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}

	if ((pngr->warnings & PNGR_BADCHRM) == 0) {
		SETPROPERTY(PNGR_CHRM);
	}
	return 1;
}

static bool
parseGAMA(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uintxx n;
	uint8* s;

	if (PRVT->chunkmap.GAMA) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->colortype == 3) {
			if (PRVT->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	PRVT->chunkmap.GAMA = 1;

	INITCRC32(pngr, CRC32_GAMA);
	s = PRVT->source;
	if (head.length != 4 || readinput(pngr, s, 4) == 0) {
		return 0;
	}

	/* the value is encoded as a four-byte PNG unsigned integer,
	 * representing gamma times 100000 */
	n = TOI32(s[0], s[1], s[2], s[3]);
	pngr->gamma = n * 0.00001f;

	if (n == 0) {
		ADDWARNING(PNGR_BADGAMA);
	}
	else {
		SETPROPERTY(PNGR_GAMA);
	}

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}

	return 1;
}

static bool
parseSBIT(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uintxx size;
	uintxx j;
	uintxx i;
	uint8* s;

	if (PRVT->chunkmap.SBIT) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->colortype == 3) {
			if (PRVT->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	PRVT->chunkmap.SBIT = 1;

	size = 0;
	switch (pngr->colortype) {
		case 0: size = 1; break;
		case 2:
		case 3: size = 3; break;
		case 4: size = 2; break;
		case 6: size = 4; break;
	}

	INITCRC32(pngr, CRC32_SBIT);

	/* each depth specified in sBIT shall be greater than zero and less than
	 * or equal to the sample depth (which is 8 for indexed-colour images,
	 * and the bit depth given in IHDR for other colour types) */
	s = pngr->sbits;
	if (head.length != size || readinput(pngr, s, size) == 0) {
		return 0;
	}

	j = pngr->depth;
	if (pngr->colortype == 3)
		j = 8;

	for (i = 0; i < size; i++) {
		if (s[i] > j || s[i] == 0) {
			j = 0;  /* bad value */
			ADDWARNING(PNGR_BADSBIT);
			break;
		}
	}

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}

	if (j != 0) {
		SETPROPERTY(PNGR_SBIT);
	}
	return 1;
}

static bool
parseSRGB(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uint8 s[1];

	if (PRVT->chunkmap.SRGB) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->colortype == 3) {
			if (PRVT->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	PRVT->chunkmap.SRGB = 1;

	INITCRC32(pngr, CRC32_SRGB);

	/* only 4 posible values: perceptual, relative, saturation, absolute */
	if (head.length != 1 || readinput(pngr, s, 1) == 0) {
		return 0;
	}

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}

	if (s[0] < 4) {
		SETPROPERTY(PNGR_SRGB);
		pngr->srgbintent = s[0];
	}
	else {
		ADDWARNING(PNGR_BADSRGB);
	}
	return 1;
}

static bool
parseBKGD(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uintxx size;
	uintxx entry;
	uint8* s;

	if (PRVT->chunkmap.BKGD) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->colortype == 3) {
			if (PRVT->chunkmap.PLTE == 0) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	PRVT->chunkmap.BKGD = 1;

	size = 0;
	switch (pngr->colortype) {
		case 0: size = 2; break;
		case 2:
		case 6: size = 6; break;
		case 3: size = 1; break;
	}

	INITCRC32(pngr, CRC32_BKGD);
	s = PRVT->source;
	if (head.length != size || readinput(pngr, s, size) == 0) {
		return 0;
	}

	/* palette index  */
	if (size == 1) {
		entry = s[0] * 3;
		if (PRVT->hasalpha) {
			entry += s[0];
		}
		pngr->background[0] = pngr->palette[entry + 0];
		pngr->background[1] = pngr->palette[entry + 1];
		pngr->background[2] = pngr->palette[entry + 2];
	}
	else {
		pngr->background[0] = TOI16(s[0], s[1]);
		s += 2;
		if (size > 2) {
			pngr->background[1] = TOI16(s[0], s[1]); s += 2;
			pngr->background[2] = TOI16(s[0], s[1]); s += 2;
		}
	}

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}

	SETPROPERTY(PNGR_BKGD);
	return 1;
}

static bool
parsePHYS(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	uint8* s;

	if (PRVT->chunkmap.PHYS) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	PRVT->chunkmap.PHYS = 1;

	INITCRC32(pngr, CRC32_PHYS);
	s = PRVT->source;
	if (head.length != 9 || readinput(pngr, s, 9) == 0) {
		return 0;
	}
	pngr->physx = TOI32(s[0], s[1], s[2], s[3]); s += 4;
	pngr->physy = TOI32(s[0], s[1], s[2], s[3]); s += 4;

	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}

	/* the following values are defined for the unit specifier:
	 * 0 unit is unknown
	 * 1 unit is the metre */
	pngr->physunit = s[0];
	if (s[0] == 0 || s[0] == 1) {
		SETPROPERTY(PNGR_PHYS);
	}
	else {
		ADDWARNING(PNGR_BADPHYS);
	}
	return 1;
}


#define SRCBUFFERSZ sizeof(((struct TPNGRPrvt*) 0)->source)
#define TGTBUFFERSZ sizeof(((struct TPNGRPrvt*) 0)->target)

/* we need at least a target buffer of 128 bytes and a source
 * buffer of 80 bytes, but 256 bytes for both buffers is a sane limit */
typedef union {
	char sbuffer_badsize[-1 + (SRCBUFFERSZ > 256) * 2];
	char tbuffer_badsize[-1 + (TGTBUFFERSZ > 256) * 2];
} TTypePNGRBufferSizeStaticAssert;


CTB_INLINE uintxx
checkiccheader(uint8* header)
{
	uintxx size;
	uint8* s;

	/* we need to parse the ICC profile header to determine the final profile
	 * size after decompression */
	s = header;

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
		return 0;
	}
	return size;
}

CTB_INLINE void
filterstring(uint8* src, uint8* dst, uintxx size)
{
	uintxx i;

	for (i = 0; i < size; i++) {
		dst[i] = src[i];
		if (src[i] == 0) {
			break;
		}
		if (src[i] < 161) {
			if (src[i] >= 32 && src[i] <= 126) {
				continue;
			}

			/* replace the invalid character with: ? */
			dst[i] = 0x63;
		}
	}
}

static bool
readiccprofile(struct TPNGRPblc* pngr, uintxx size)
{
	uintxx r;
	uintxx total;
	uintxx headerdone;
	uintxx result;
	uintxx remaining;
	uint8* s;
	uint8* profile;
	uint8* profileend;

	/* 1-79 + NULL + compression method + zlib header */
	total = 80;
	if (total > size)
		total = size;

	s = PRVT->source;
	remaining  = 0;
	profile    = NULL;
	profileend = NULL;
	for (r = 0; r < total; r++) {
		if (readinput(pngr, s + r, 1) == 0) {
			return 0;
		}
		if (s[r] == 0x00)
			break;
	}
	filterstring(s, pngr->iccpname, r);

	/* compresion method + zlib header */
	if (readinput(pngr, s, 3) == 0) {
		return 0;
	}

	/* only compression method 0 is allowed */
	if (s[0]) {
		goto L_ERROR;
	}
	r += 4;

	remaining  = size - r;
	headerdone = 0;

	result = INFLT_SRCEXHSTD;
	inflator_settgt(PRVT->inflator, PRVT->target, 0x80);
	for (;;) {
		if (result == INFLT_TGTEXHSTD) {
			if (headerdone || inflator_tgtend(PRVT->inflator) != 0x80) {
				goto L_ERROR;
			}

			total = checkiccheader(PRVT->target);
			if (total == 0) {
				goto L_ERROR;
			}

			/* realloc the profile memory if the current profile is
			 * larger than the allocated memory */
			if (size > PRVT->iccpsize) {
				uint8* buffer;

				if (PRVT->iccpmemory) {
					release(PRVT, PRVT->iccpmemory);
					PRVT->iccpmemory = NULL;
					PRVT->iccpsize   = 0;
				}
				buffer = reserve(PRVT, total);
				if (buffer == NULL) {
					SETERROR(PNGR_EOOM);
					return 0;
				}
				PRVT->iccpmemory = buffer;
				PRVT->iccpsize   = total;
			}
			profile    = PRVT->iccpmemory;
			profileend = profile + total;

			/* copy the header */
			memcpy(profile, PRVT->target, 0x80);

			/* now we are decoding it to the final memory location */
			profile += 0x80;
			inflator_settgt(PRVT->inflator, profile, total - 0x80);
			headerdone = 1;
		}
		else {
			if (result == INFLT_SRCEXHSTD) {
				if (remaining == 0) {
					goto L_ERROR;
				}

				if (remaining < (r = SRCBUFFERSZ))
					r = remaining;

				if (readinput(pngr, s, r)) {
					inflator_setsrc(PRVT->inflator, s, (PRVT->inputsize = r));
					remaining -= r;
				}
				else {
					return 0;
				}
			}
		}

		result = inflator_inflate(PRVT->inflator, 0);
		if (result == INFLT_OK) {
			uintxx left;

			profile += inflator_tgtend(PRVT->inflator);
			if (profileend != profile) {
				goto L_ERROR;
			}

			left = PRVT->inputsize - (r = inflator_srcend(PRVT->inflator));
			if (left >= 4) {
				s = s + r;
				pngr->iccpchecksum = TOI32(s[0], s[1], s[2], s[3]);
			}
			else {
				/* truncated checksum */
				if (remaining + left >= 4) {
					switch (left) {
						case 3: s[2] = s[r + 2];  /* fallthrough */
						case 2: s[1] = s[r + 1];  /* fallthrough */
						case 1: s[0] = s[r + 0];
					}

					left = 4 - left;
					if (readinput(pngr, s + left, left) == 0) {
						return 0;
					}
					pngr->iccpchecksum = TOI32(s[0], s[1], s[2], s[3]);
					remaining -= left;
				}
				else {
					goto L_ERROR;
				}
			}

			/* ignore the remaning input left in the chunk */
			if (remaining) {
				if (consumechunk(pngr, remaining) == 0) {
					return 0;
				}
			}

			checkcrc32(pngr);
			if (pngr->error) {
				return 0;
			}
			pngr->iccprofile = PRVT->iccpmemory;
			pngr->iccpsize   = total;

			inflator_reset(PRVT->inflator);
			return 1;
		}
		else {
			if (result == INFLT_ERROR) {
				goto L_ERROR;
			}
		}
	}

L_ERROR:
	if (remaining) {
		if (consumechunk(pngr, remaining) == 0) {
			return 0;
		}
	}
	checkcrc32(pngr);
	if (pngr->error) {
		return 0;
	}
	pngr->iccpname[0] = 0;

	inflator_reset(PRVT->inflator);
	return 0;
}

static bool
parseICCP(struct TPNGRPblc* pngr, struct TChunkHead head)
{
	if (PRVT->chunkmap.ICCP) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->colortype == 3) {
			if (PRVT->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	PRVT->chunkmap.ICCP = 1;

	if (head.length > MAXCHUNKSIZE) {
		SETERROR(PNGR_ELIMIT);
		return 0;
	}

	INITCRC32(pngr, CRC32_ICCP);
	if (pngr->flags & PNGR_IGNOREICCP) {
		if (head.length) {
			if (consumechunk(pngr, head.length) == 0) {
				return 0;
			}
		}
		checkcrc32(pngr);
		if (pngr->error) {
			return 0;
		}
		return 1;
	}

	if (readiccprofile(pngr, head.length)) {
		SETPROPERTY(PNGR_ICCP);
	}
	else {
		if (pngr->error) {
			return 0;
		}
		ADDWARNING(PNGR_BADICCP);
	}
	return 1;
}

#undef SETPROPERTY
#undef ADDWARNING

#undef TOI32
#undef TOI16


static uintxx
inflateidat(struct TPNGRPblc* pngr)
{
	uintxx r;
	uintxx limit;

	for (;;) {
		if (PRVT->result == INFLT_SRCEXHSTD) {
			limit = PRVT->remaining;

			if (limit == 0) {
				struct TChunkHead head;

				checkcrc32(pngr);
				if (pngr->error) {
					break;
				}

				head = getchunkhead(pngr);
				if (head.fcc[0] != 'I' ||
					head.fcc[1] != 'D' ||
					head.fcc[2] != 'A' || head.fcc[3] != 'T') {
					/* bad or incomplete stream */
					if (pngr->error == 0) {
						SETERROR(PNGR_EBADDATA);
					}
					SETSTATE(PNGR_BADSTATE);
					return 0;
				}

				INITCRC32(pngr, CRC32_IDAT);
				PRVT->remaining = head.length;
				continue;
			}

			if (limit > SRCBUFFERSZ) {
				limit = SRCBUFFERSZ;
			}

			if (readinput(pngr, PRVT->source, limit) == 0) {
				return 0;
			}
			PRVT->remaining -= (PRVT->inputsize = limit);

			inflator_setsrc(PRVT->inflator, PRVT->source, limit);
		}
		else {
			if (PRVT->result ^ INFLT_TGTEXHSTD) {
				SETERROR(PNGR_EDEFLATE);
				SETSTATE(PNGR_BADSTATE);
				return 0;
			}
		}

		inflator_settgt(PRVT->inflator, PRVT->target, TGTBUFFERSZ);

		PRVT->result = inflator_inflate(PRVT->inflator, 0);
		if (PRVT->result == INFLT_ERROR) {
			SETERROR(PNGR_EDEFLATE);
			SETSTATE(PNGR_BADSTATE);
			return 0;
		}

		r = inflator_tgtend(PRVT->inflator);
		if (r || PRVT->result == INFLT_OK) {
			return r;
		}
	}

	return 0;
}

#undef SRCBUFFERSZ
#undef TGTBUFFERSZ


CTB_INLINE uintxx
consumetail(struct TPNGRPblc* pngr, intxx remaining)
{
	struct TChunkHead head;

	while (remaining) {
		head = getchunkhead(pngr);
		if (head.fcc[0] != 'I' ||
			head.fcc[1] != 'D' ||
			head.fcc[2] != 'A' || head.fcc[3] != 'T') {
			/* bad or incomplete stream */
			if (pngr->error == 0) {
				SETERROR(PNGR_EBADDATA);
			}
			return 0;
		}

		INITCRC32(pngr, CRC32_IDAT);
		remaining -= head.length;
		if (remaining < 0) {
			SETERROR(PNGR_EBADDATA);
			return 0;
		}

		consumechunk(pngr, head.length);
		checkcrc32(pngr);
		if (pngr->error) {
			return 0;
		}
	}
	return 1;
}

CTB_INLINE bool
checktail(struct TPNGRPblc* pngr)
{
	if (PRVT->result == INFLT_SRCEXHSTD ||
		PRVT->result == INFLT_TGTEXHSTD) {
		inflateidat(pngr);
	}

	if (PRVT->result == INFLT_OK) {
		uintxx remaining;

		if (PRVT->remaining) {
			SETERROR(PNGR_EBADDATA);
			return 0;
		}
		checkcrc32(pngr);
		if (pngr->error) {
			return 0;
		}

		/* le adler32 zlib stream tail */
		remaining = PRVT->inputsize - inflator_srcend(PRVT->inflator);
		remaining = 4 - remaining;
		if (remaining > 0) {
			if (consumetail(pngr, remaining) == 0) {
				return 0;
			}
		}
		return 1;
	}
	return 0;
}

CTB_INLINE bool
fetchrow(struct TPNGRPblc* pngr, uint8* target, uintxx size)
{
	uintxx total;
	uintxx avaible;
	uintxx j;

	for (total = size; total; ) {
		avaible = (uintxx) (PRVT->tend - PRVT->tbgn);
		if (LIKELY(avaible)) {
			j = avaible;
			if (j > total) {
				j = total;
			}

			memcpy(target, PRVT->tbgn, j);
			target     += j;
			PRVT->tbgn += j;

			total -= j;
		}
		else {
			uintxx r;

			if (UNLIKELY((r = inflateidat(pngr)) == 0)) {
				return 0;
			}
			PRVT->tbgn = PRVT->target;
			PRVT->tend = PRVT->tbgn + r;
			continue;
		}
	}

	return 1;
}


#if defined(PNGR_CFG_EXTERNALASM)
extern void pngr_unfilterASM(uint8*, uint8*, uintxx, uintxx, uintxx);

#else

CTB_INLINE uint8
paetchfilter(uint8 a, uint8 b, uint8 c)
{
	int16 p;
	int16 pa, pb, pc;
	int16 ta, tb, tc;

	p  = a + b - c;
	pa = p - a;
	pb = p - b;
	pc = p - c;

	/* branchless abs */
	ta = pa >> 15;
	tb = pb >> 15;
	tc = pc >> 15;
	pa ^= ta;
	pb ^= tb;
	pc ^= tc;
	pa -= ta;
	pb -= tb;
	pc -= tc;

	if (pa <= pb && pa <= pc) {
		return a;
	}
	else {
		if (pb <= pc) {
			return b;
		}
	}
	return c;
}

static void
unfilter(uint8* curr, uint8* prev, uintxx filter, uintxx size, uintxx psize)
{
	uintxx i;

	switch (filter) {
		case 1:
			for (i = psize; i < size;) {
				curr[i] += curr[i - psize]; i++;
				curr[i] += curr[i - psize]; i++;
				curr[i] += curr[i - psize]; i++;
				curr[i] += curr[i - psize]; i++;
			}

			break;

		case 2:
			for (i = 0; i < size;) {
				curr[i] += prev[i]; i++;
				curr[i] += prev[i]; i++;
				curr[i] += prev[i]; i++;
				curr[i] += prev[i]; i++;
			}

			break;

		case 3:
			for (i = 0; i < psize; i++) {
				curr[i] += prev[i] >> 1;
			}
			for (i = psize; i < size;) {
				curr[i] += (curr[i - psize] + prev[i]) >> 1; i++;
				curr[i] += (curr[i - psize] + prev[i]) >> 1; i++;
				curr[i] += (curr[i - psize] + prev[i]) >> 1; i++;
				curr[i] += (curr[i - psize] + prev[i]) >> 1; i++;
			}

			break;

		case 4:
			for (i = 0; i < psize; i++) {
				curr[i] += prev[i];
			}

			for (i = psize; i < size; i++) {
				uint8 a;
				uint8 b;
				uint8 c;

				a = curr[i - psize];
				c = prev[i - psize];
				b = prev[i];
				curr[i] += paetchfilter(a, b, c);
			}

			break;
	}
}

#endif

static void
unpack(uint8* row, uintxx size, uintxx depth)
{
	/* inplace row expantion */
	intxx i;
	intxx j;

	switch (depth) {
		case 1:
			i = ((size * 1) + 7) >> 3;
			j = i * 8;
			for (i--; i >= 0; i--) {
				uint8 v;

				v = row[i];
				row[--j] = (v >> 0) & 1;
				row[--j] = (v >> 1) & 1;
				row[--j] = (v >> 2) & 1;
				row[--j] = (v >> 3) & 1;
				row[--j] = (v >> 4) & 1;
				row[--j] = (v >> 6) & 1;
				row[--j] = (v >> 6) & 1;
				row[--j] = (v >> 7) & 1;
			}
			break;

		case 2:
			i = ((size * 2) + 7) >> 3;
			j = i * 4;
			for (i--; i >= 0; i--) {
				uint8 v;

				v = row[i];
				row[--j] = (v >> 0) & ((((uint8) 1) << 2) - 1);
				row[--j] = (v >> 2) & ((((uint8) 1) << 2) - 1);
				row[--j] = (v >> 4) & ((((uint8) 1) << 2) - 1);
				row[--j] = (v >> 6) & ((((uint8) 1) << 2) - 1);
			}
			break;

		case 4:
			i = ((size * 4) + 7) >> 3;
			j = i * 2;
			for (--i; i >= 0; i--) {
				uint8 v;

				v = row[i];
				row[--j] = (v >> 0) & ((((uint8) 1) << 4) - 1);
				row[--j] = (v >> 4) & ((((uint8) 1) << 4) - 1);
			}
			break;
	}
}


#if defined(PNGR_CFG_EXTERNALASM)
	#define UNFILTER pngr_unfilterASM
#else
	#define UNFILTER unfilter
#endif

CTB_INLINE uint8*
decoderow(struct TPNGRPblc* pngr, uintxx sizex, uintxx rowsize)
{
	uint8* curr;
	uint8* prev;
	uintxx filter;

	PRVT->currrow = PRVT->rbuffers[0];
	if (PRVT->prevrow == PRVT->rbuffers[0]) {
		PRVT->currrow = PRVT->rbuffers[1];
	}

	curr = PRVT->currrow;
	prev = PRVT->prevrow;
	if (fetchrow(pngr, curr, rowsize) == 0) {
		SETSTATE(PNGR_BADSTATE);
		return NULL;
	}

	filter = curr[0];
	curr++;
	prev++;
	if (LIKELY(filter)) {
		if (UNLIKELY(filter > 4)) {
			SETERROR(PNGR_EBADDATA);
			SETSTATE(PNGR_BADSTATE);
			return NULL;
		}
		UNFILTER(curr, prev, filter, rowsize - 1, PRVT->rawpelsize);
	}

	if (UNLIKELY(pngr->depth < 8)) {
		unpack(curr, sizex, pngr->depth);
	}

	/* swap rows */
	PRVT->prevrow = curr - 1;
	PRVT->currrow = prev - 1;
	return curr;
}


#if CTB_IS_LITTLEENDIAN
	#define BYTE0_OFFSET 1
	#define BYTE1_OFFSET 0
#else
	#define BYTE0_OFFSET 0
	#define BYTE1_OFFSET 1
#endif

static void
setrow(struct TPNGRPblc* pngr, uint8* pixels, uint8* row)
{
	uintxx i;

	if (PRVT->hasalpha) {
		if (pngr->colortype == 0 || pngr->colortype == 2) {
			if (pngr->depth ^ 16) {
				uint8 sample[4];

				sample[0] = (uint8) pngr->alpha[0];
				sample[1] = (uint8) pngr->alpha[1];
				sample[2] = (uint8) pngr->alpha[2];
				if (pngr->colortype == 0) {
					for (i = 0; i < pngr->sizex; i++) {
						pixels[0] = row[0];
						pixels[1] = 0xff;
						if (row[0] == sample[0]) {
							pixels[1] = 0x00;
						}
						pixels += 2;
						row += 1;
					}
				}
				else {
					for (i = 0; i < pngr->sizex; i++) {
						pixels[0] = row[0];
						pixels[1] = row[1];
						pixels[2] = row[2];
						pixels[3] = 0xff;
						if (row[0] == sample[0] &&
							row[1] == sample[1] &&
							row[2] == sample[2]) {
							pixels[3] = 0x00;
						}
						pixels += 4;
						row += 3;
					}
				}
			}
			else {
				uint8* sample;

				sample = (uint8*) pngr->alpha;
				if (pngr->colortype == 0) {
					for (i = 0; i < pngr->sizex; i++) {
						pixels[0] = row[BYTE0_OFFSET + 0];
						pixels[1] = row[BYTE1_OFFSET + 0];
						pixels[2] = 0xff;
						pixels[3] = 0xff;
						if (pixels[0] == sample[0] &&
							pixels[1] == sample[1]) {
							pixels[2] = 0x00;
							pixels[3] = 0x00;
						}
						pixels += 4;
						row += 2;
					}
				}
				else {
					for (i = 0; i < pngr->sizex; i++) {
						pixels[0] = row[BYTE0_OFFSET + 0];
						pixels[1] = row[BYTE1_OFFSET + 0];
						pixels[2] = row[BYTE0_OFFSET + 2];
						pixels[3] = row[BYTE1_OFFSET + 2];
						pixels[4] = row[BYTE0_OFFSET + 4];
						pixels[5] = row[BYTE1_OFFSET + 4];
						pixels[6] = 0xff;
						pixels[7] = 0xff;
						if (pixels[0] == sample[0] &&
							pixels[1] == sample[1] &&
							pixels[2] == sample[2] &&
							pixels[3] == sample[3] &&
							pixels[4] == sample[4] &&
							pixels[5] == sample[5]) {
							pixels[6] = 0x00;
							pixels[7] = 0x00;
						}
						pixels += 8;
						row += 6;
					}
				}
			}
			return;
		}
	}

#if CTB_IS_LITTLEENDIAN
	if (pngr->depth == 16) {
		uintxx total;

		total = pngr->sizex * (PRVT->pelsize >> 1);
		for (i = 0; i < total; i++) {
			*pixels++ = row[1];
			*pixels++ = row[0];
			row += 2;
		}
		return;
	}
#endif

	memcpy(pixels, row, PRVT->rowsize);
}

uintxx
pngr_decodeimg(TPNGReader* pngr)
{
	uintxx i;
	uintxx j;
	uint8* pixels;
	uint8* idxs;
	ASSERT(pngr);

	if (pngr->state ^ 3) {
		if (pngr->state == 2) {
			PBLC->state++;
		}
		else {
			SETSTATE(PNGR_BADSTATE);
			if (pngr->error == 0) {
				SETERROR(PNGR_EINCORRECTUSE);
			}
			return 0;
		}
	}

	if (pngr->interlace) {
		PRVT->interpolate = 0;
		switch (PRVT->pass) {
			case 0: pngr_decodepass(pngr);  /* fallthrough */
			case 1: pngr_decodepass(pngr);  /* fallthrough */
			case 2: pngr_decodepass(pngr);  /* fallthrough */
			case 3: pngr_decodepass(pngr);  /* fallthrough */
			case 4: pngr_decodepass(pngr);  /* fallthrough */
			case 5: pngr_decodepass(pngr);  /* fallthrough */
			case 6: pngr_decodepass(pngr);  /* fallthrough */
			default:
				break;
		}
		if (pngr->state == 4 || pngr->state == 5) {
			return 1;
		}
		return 0;
	}

	pixels = PRVT->pixels;
	idxs   = PRVT->idxs;
	for (i = 0; i < pngr->sizey; i++) {
		uint8* row;

		row = decoderow(PBLC, pngr->sizex, PRVT->rawrowsize);
		if (UNLIKELY(row == NULL)) {
			SETSTATE(PNGR_BADSTATE);
			return 0;
		}

		if (LIKELY(pixels != NULL)) {
			if (UNLIKELY(pngr->colortype == 3)) {
				uintxx entry;

				/* we don't check the range here */
				if (PRVT->hasalpha) {
					for (j = 0; j < pngr->sizex; j++) {
						entry = row[j] * 4;

						*pixels++ = pngr->palette[entry + 0];
						*pixels++ = pngr->palette[entry + 1];
						*pixels++ = pngr->palette[entry + 2];
						*pixels++ = pngr->palette[entry + 3];
					}
				}
				else {
					for (j = 0; j < pngr->sizex; j++) {
						entry = row[j] * 4;
						*pixels++ = pngr->palette[entry + 0];
						*pixels++ = pngr->palette[entry + 1];
						*pixels++ = pngr->palette[entry + 2];
					}
				}
			}
			else {
				setrow(PBLC, pixels, row);
				pixels += PRVT->rowsize;
			}
		}

		if (LIKELY(idxs != NULL)) {
			for (j = 0; j < pngr->sizex; j++) {
				idxs[j] = row[j];
			}
			idxs += pngr->sizex;
		}
	}

	if (checktail(PBLC) == 0)  {
		SETSTATE(5);
		return 1;
	}

	SETSTATE(4);
	if (parsechunks(PBLC) == 0 || pngr->warnings) {
		SETSTATE(5);
	}
	return 1;
}


CTB_INLINE uint8*
getsample(struct TPNGRPblc* pngr, uint8* source, uint8* pixel)
{
	if (pngr->colortype == 3) {
		uintxx j;

		/* we don't check the range here */
		if (PRVT->hasalpha) {
			j = source[0] * 4;
			pixel[0] = pngr->palette[j + 0];
			pixel[1] = pngr->palette[j + 1];
			pixel[2] = pngr->palette[j + 2];
			pixel[3] = pngr->palette[j + 3];
		}
		else {
			j = source[0] * 4;
			pixel[0] = pngr->palette[j + 0];
			pixel[1] = pngr->palette[j + 1];
			pixel[2] = pngr->palette[j + 2];
		}
		return pixel;
	}

	if (PRVT->hasalpha) {
		if (pngr->colortype == 0 || pngr->colortype == 2) {
			if (pngr->depth ^ 16) {
				uint8 sample[4];

				sample[0] = (uint8) pngr->alpha[0];
				sample[1] = (uint8) pngr->alpha[1];
				sample[2] = (uint8) pngr->alpha[2];
				if (pngr->colortype == 0) {
					pixel[0] = source[0];
					pixel[1] = 0xff;
					if (source[0] == sample[0]) {
						pixel[1] = 0x00;
					}
				}
				else {
					pixel[0] = source[0];
					pixel[1] = source[1];
					pixel[2] = source[2];
					pixel[3] = 0xff;
					if (source[0] == sample[0] &&
						source[1] == sample[1] &&
						source[2] == sample[2]) {
						pixel[3] = 0x00;
					}
				}
			}
			else {
				uint8* sample;

				sample = (uint8*) pngr->alpha;
				if (pngr->colortype == 0) {
					pixel[0] = source[BYTE0_OFFSET + 0];
					pixel[1] = source[BYTE1_OFFSET + 0];
					pixel[2] = 0xff;
					pixel[3] = 0xff;
					if (pixel[0] == sample[0] &&
						pixel[1] == sample[1]) {
						pixel[2] = 0x00;
						pixel[3] = 0x00;
					}
				}
				else {
					pixel[0] = source[BYTE0_OFFSET + 0];
					pixel[1] = source[BYTE1_OFFSET + 0];
					pixel[2] = source[BYTE0_OFFSET + 2];
					pixel[3] = source[BYTE1_OFFSET + 2];
					pixel[4] = source[BYTE0_OFFSET + 4];
					pixel[5] = source[BYTE1_OFFSET + 4];
					pixel[6] = 0xff;
					pixel[7] = 0xff;
					if (pixel[0] == sample[0] &&
						pixel[1] == sample[1] &&
						pixel[2] == sample[2] &&
						pixel[3] == sample[3] &&
						pixel[4] == sample[4] &&
						pixel[5] == sample[5]) {
						pixel[6] = 0x00;
						pixel[7] = 0x00;
					}
				}
			}
			return pixel;
		}
	}

#if CTB_IS_LITTLEENDIAN
	if (pngr->depth == 16) {
		uint16* p;

		p = (void*) pixel;
		switch (PRVT->pelsize) {
			case 8: p[3] = (source[7] << 0x08) | source[6];  /* fallthrough */
			case 6: p[2] = (source[5] << 0x08) | source[4];  /* fallthrough */
			case 4: p[1] = (source[3] << 0x08) | source[2];  /* fallthrough */
			case 2: p[0] = (source[1] << 0x08) | source[0];
		}
		return pixel;
	}
#endif
	return source;
}

#undef BYTE0_OFFSET
#undef BYTE1_OFFSET


CTB_FORCEINLINE void
fill(struct TPNGRPblc* pngr, uint8* offset, uint8* s, uintxx x2, uintxx y2)
{
	uintxx x;
	uintxx y;
	uint8* position;

	for (y = 0; y < y2; y++) {
		position = offset + (y * PRVT->rowsize);

		for (x = 0; x < x2; x++) {
			switch (PRVT->pelsize) {
				case 8: position[7] = s[7];
				/* 7 */ position[6] = s[6];  /* fallthrough */
				case 6: position[5] = s[5];
				/* 5 */ position[4] = s[4];  /* fallthrough */
				case 4: position[3] = s[3];  /* fallthrough */
				case 3: position[2] = s[2];  /* fallthrough */
				case 2: position[1] = s[1];  /* fallthrough */
				case 1: position[0] = s[0];
			}
			position += PRVT->pelsize;
		}
	}
}

uintxx
pngr_decodepass(TPNGReader* pngr)
{
	uintxx x;
	uintxx y;
	uintxx i;
	uintxx r;
	uintxx rowsize;
	uintxx memsize;
	uint8* peloffsety;
	uint8* idxoffsety;
	uint8* offsetx;
	uintxx stepx;
	uintxx stepy;
	ASSERT(pngr);

	if (pngr->state ^ 3) {
		if (pngr->state == 2) {
			PBLC->state++;
		}
		else {
			SETSTATE(PNGR_BADSTATE);
			if (pngr->error == 0) {
				SETERROR(PNGR_EINCORRECTUSE);
			}
			return 0;
		}
	}

	i = PRVT->pass;
	rowsize = PRVT->passrowsize[i];
	memsize = PRVT->passmemsize[i];
	if (rowsize == 0) {
		/* empty pass */
		goto L_DONE;
	}

	/* init the row buffers */
	PRVT->prevrow = PRVT->rbuffers[0];
	PRVT->currrow = PRVT->rbuffers[1];
	memset(PRVT->prevrow, 0, memsize);

	/* */
	stepx = STEP_X(i) * PRVT->pelsize;
	stepy = STEP_Y(i) * PRVT->rowsize;

	/* target buffer */
	peloffsety  = PRVT->pixels;
	peloffsety += ORIGIN_X(i) * PRVT->pelsize;
	peloffsety += ORIGIN_Y(i) * PRVT->rowsize;

	idxoffsety  = PRVT->idxs;
	idxoffsety += ORIGIN_X(i);
	idxoffsety += ORIGIN_Y(i) * pngr->sizex;

	for (y = ORIGIN_Y(i); y < pngr->sizey; y += STEP_Y(i)) {
		uint8* rowpointer;
		uint8* row;
		uint8* sample;

		rowpointer = decoderow(PBLC, rowsize, memsize);
		if (rowpointer == NULL) {
			SETSTATE(PNGR_BADSTATE);
			return 0;
		}

		row = rowpointer;
		if (LIKELY(PRVT->pixels != NULL)) {
			offsetx = peloffsety;
			if (PRVT->interpolate) {
				uintxx sx;
				uintxx sy;

				for (x = ORIGIN_X(i); x < pngr->sizex; x += STEP_X(i)) {
					const uintxx passsizex[] = {8, 4, 4, 2, 2, 1, 1};
					const uintxx passsizey[] = {8, 8, 4, 4, 2, 2, 1};
					uint8 pixel[8];

					sx = pngr->sizex - x;
					sy = pngr->sizey - y;
					if (UNLIKELY(sx > passsizex[i])) sx = passsizex[i];
					if (UNLIKELY(sy > passsizey[i])) sy = passsizey[i];

					sample = getsample(PBLC, row, pixel);
					fill(PBLC, offsetx, sample, sx, sy);

					row += PRVT->rawpelsize;
					offsetx += stepx;
				}
			}
			else {
				for (x = ORIGIN_X(i); x < pngr->sizex; x += STEP_X(i)) {
					uint8 pixel[8];

					sample = getsample(PBLC, row, pixel);
					switch (PRVT->pelsize) {
						case 8: offsetx[7] = sample[7];
						/* 7 */ offsetx[6] = sample[6];  /* fallthrough */
						case 6: offsetx[5] = sample[5];
						/* 5 */ offsetx[4] = sample[4];  /* fallthrough */
						case 4: offsetx[3] = sample[3];  /* fallthrough */
						case 3: offsetx[2] = sample[2];  /* fallthrough */
						case 2: offsetx[1] = sample[1];  /* fallthrough */
						case 1: offsetx[0] = sample[0];
					}
					row += PRVT->rawpelsize;
					offsetx += stepx;
				}
			}
			peloffsety += stepy;
		}

		row = rowpointer;
		if (LIKELY(PRVT->idxs != NULL)) {
			offsetx = idxoffsety;

			for (x = ORIGIN_X(i); x < pngr->sizex; x += STEP_X(i)) {
				offsetx[0] = *row++;
				offsetx += STEP_X(i);
			}
			idxoffsety += STEP_Y(i) * pngr->sizex;
		}
	}

L_DONE:
	r = ++PRVT->pass;
	if (r == ADAM7PASSES) {
		if (checktail(PBLC) == 0)  {
			SETSTATE(5);
			return 0;
		}

		SETSTATE(4);
		if (parsechunks(PBLC) == 0 || pngr->warnings) {
			SETSTATE(5);
		}
		return 0;
	}
	return r;
}

#undef STEP_X
#undef STEP_Y
#undef ORIGIN_X
#undef ORIGIN_Y


#undef SETERROR
#undef SETSTATE
#undef PBLC
#undef PRVT
