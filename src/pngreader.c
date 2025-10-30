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

#include <jimage/pngreader.h>
#include <jdeflate/zstrm.h>
#include <ctoolbox/memory.h>
#include <ctoolbox/ckdint.h>


/* chunk size limit for ICCP, ITXT, ZTXT and TEXT chunks or unknown
 * chunks (8MB) */
#define MAXCHUNKSIZE 0x800000L

/* ICC profile size limit */
#define MAXICCPSIZE  0x800000L


/* private stuff */
struct TPNGRPrvt {
	/* public fields */
	struct TPNGReader public;

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
	uint8* mainmemory;
	uintxx mainmsize;

	/* ICCP function callback */
	TPNGRICCPFn iccpfn;

	/* ICCP function payload */
	void* iccpuser;

	uint32 iccpmode;
	uintxx iccpsize;

	/* progressive pass */
	uintxx interpolate;
	uintxx pass;
	uintxx passmemsize[7];
	uintxx passrowsize[7];

	/* input callback */
	TIMGInputFn inputfn;

	/* input callback parameter */
	void* payload;

	/* */
	const struct TZStrm* zstrm;

	/* chunk remaining bytes */
	uintxx remaining;  

	/* internal buffer */
	uint8 source[4096];

	/* custom allocator */
	const struct TAllocator* allctr;
};


CTB_INLINE void*
request_(struct TPNGRPrvt* p, uintxx amount)
{
	const struct TAllocator* a;

	a = p->allctr;
	return a->request(amount, a->user);
}

CTB_INLINE void
dispose_(struct TPNGRPrvt* p, void* memory, uintxx amount)
{
	const struct TAllocator* a;

	a = p->allctr;
	a->dispose(memory, amount, a->user);
}

const TPNGReader*
pngr_create(ePNGRFlags flags, const TAllocator* allctr)
{
	struct TPNGRPrvt* pngr;

	if (allctr == NULL) {
		allctr = ctb_getdefaultallocator();
	}

	pngr = allctr->request(sizeof(struct TPNGRPrvt), allctr->user);
	if (pngr == NULL) {
		return NULL;
	}
	pngr->allctr = allctr;

	pngr->zstrm = zstrm_create(ZSTRM_INFLATE | ZSTRM_ZLIB, 0, allctr);
	if (pngr->zstrm == NULL) {
		dispose_(pngr, pngr, sizeof(struct TPNGRPrvt));
		return NULL;
	}

	pngr->mainmemory = NULL;
	pngr_reset((const void*) pngr);

	pngr->public.flags = flags;
	return (const void*) pngr;
}


#define SETERROR(ERROR) (pngr->public.error = (ERROR), printf("set error on line %u\n", __LINE__))
#define SETSTATE(STATE) (pngr->public.state = (STATE))

void
pngr_reset(const TPNGReader* state)
{
	uintxx i;
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state);

	pngr = CTB_CONSTCAST(state);

	/* plublic fields */
	pngr->public.state = 0;
	pngr->public.error = 0;
	pngr->public.warnings   = 0;
	pngr->public.properties = 0;

	/* header */
	pngr->public.sizex = 0;
	pngr->public.sizey = 0;

	pngr->public.colortype = 0;
	pngr->public.depth     = 0;
	pngr->public.requiredmemory = 0;

	pngr->public.compression = 0;
	pngr->public.filter      = 0;
	pngr->public.interlace   = 0;

	pngr->public.palettesize = 0;
	for (i = 0; i < sizeof(pngr->public.palette); i++) {
		pngr->public.palette[i] = 0;
	}

	pngr->public.alpha[0] = 0;
	pngr->public.alpha[1] = 0;
	pngr->public.alpha[2] = 0;
	pngr->public.background[0] = 0;
	pngr->public.background[1] = 0;
	pngr->public.background[2] = 0;

	for (i = 0; i < sizeof(pngr->public.sbits); i++) {
		pngr->public.sbits[i] = 0;
	}

	pngr->public.gamma = 0.f;

	pngr->public.wpointx = 0.f;
	pngr->public.wpointy = 0.f;
	pngr->public.chromax[0] = pngr->public.chromay[0] = 0.f;
	pngr->public.chromax[1] = pngr->public.chromay[1] = 0.f;
	pngr->public.chromax[2] = pngr->public.chromay[1] = 0.f;

	pngr->public.srgbintent = 0;

	pngr->public.physx = 0;
	pngr->public.physy = 0;
	pngr->public.physunit = 0;

	/* private stuff */
	pngr->chunkmap = (struct TPNGRChunkMap) {0, 0, 0, 0, 0, 0, 0, 0, 0};
	pngr->hasalpha = 0;

	pngr->crc32 = 0;
	pngr->docrc = 0;

	pngr->interpolate = 0;
	pngr->pass = 0;

	pngr->pixels = NULL;
	pngr->idxs   = NULL;

	if (pngr->mainmemory) {
		dispose_(pngr, pngr->mainmemory, pngr->mainmsize);
		pngr->mainmemory = NULL;
	}
	pngr->mainmsize = 0;

	pngr->iccpfn   = NULL;
	pngr->iccpuser = NULL;

	pngr->iccpmode = 0;
	pngr->iccpsize = 0;

	pngr->remaining = 0;
	pngr->source[0] = 0;

	pngr->payload = NULL;
	pngr->inputfn = NULL;
	zstrm_reset(pngr->zstrm);
}

void
pngr_destroy(const TPNGReader* state)
{
	struct TPNGRPrvt* pngr;

	pngr = CTB_CONSTCAST(state);
	if (pngr == NULL) {
		return;
	}

	if (pngr->mainmemory) {
		dispose_(pngr, pngr->mainmemory, pngr->mainmsize);
	}

	zstrm_destroy(pngr->zstrm);
	dispose_(pngr, pngr, sizeof(struct TPNGRPrvt));
}

void
pngr_setICCPfn(const TPNGReader* state, TPNGRICCPFn fn, void* user)
{
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state);

	pngr = CTB_CONSTCAST(state);
	if (pngr->public.state || pngr->iccpmode) {
		SETSTATE(0xDEADBEEF);
		if (pngr->public.error == 0) {
			SETERROR(PNGR_EINCORRECTUSE);
		}
		return;
	}

	pngr->iccpfn = fn;
	pngr->iccpuser = user;
}

void
pngr_setinputfn(const TPNGReader* state, TIMGInputFn fn, void* user)
{
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state);

	pngr = CTB_CONSTCAST(state);
	if (pngr->public.state || pngr->iccpmode) {
		SETSTATE(0xDEADBEEF);
		if (pngr->public.error == 0) {
			SETERROR(PNGR_EINCORRECTUSE);
		}
		return;
	}

	pngr->inputfn = fn;
	pngr->payload = user;
}

CTB_INLINE bool
readinput(struct TPNGRPrvt* pngr, uint8* buffer, uintxx size)
{
	intxx r;

	r = pngr->inputfn(buffer, size, pngr->payload);
	if (CTB_EXPECT0((uintxx) r ^ size)) {
		static const uintxx error[] = {
			PNGR_EBADDATA,
			PNGR_EIOERROR
		};

		SETERROR(error[((uintxx) r) >> ((sizeof(intxx) << 3) - 1)]);
		return 0;
	}

	if (pngr->docrc) {
		pngr->crc32 = zstrm_crc32update(pngr->crc32, buffer, size);
	}
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
getchunkhead(struct TPNGRPrvt* pngr)
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


CTB_INLINE void
initcrc32(struct TPNGRPrvt* pngr, uint32 crc)
{
	pngr->crc32 = crc;
	if ((pngr->public.flags & PNGR_NOCRCCHECK) == 0) {
		pngr->docrc = 1;
	}
}


CTB_INLINE void
checkcrc32(struct TPNGRPrvt* pngr)
{
	uint8 s[4];
	bool check;

	check = pngr->docrc;
	pngr->docrc = 0;
	if (readinput(pngr, s, 4)) {
		if (check) {
			uint32 crc32;

			crc32 = TOI32(s[0], s[1], s[2], s[3]);
			if (crc32 != (pngr->crc32 ^ 0xffffffff)) {
				SETERROR(PNGR_EBADCRC);
			}
		}
	}
}


CTB_INLINE bool
checksignature(struct TPNGRPrvt* pngr)
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
			if (colordepth == 1 || colordepth == 2 || colordepth == 4) {
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
parseIHDR(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uint8 *s;

	initcrc32(pngr, CRC32_IHDR);
	s = pngr->source;
	if (head.length ^ 13 || readinput(pngr, s, 13) == 0) {
		return 0;
	}

	pngr->public.sizex = TOI32(s[0], s[1], s[2], s[3]);
	pngr->public.sizey = TOI32(s[4], s[5], s[6], s[7]);
	if (pngr->public.sizey == 0 || pngr->public.sizey > 0x7fffffff) {
		goto L_ERROR;
	}
	if (pngr->public.sizex == 0 || pngr->public.sizex > 0x7fffffff) {
		goto L_ERROR;
	}

	/* image properties */
	pngr->public.depth       = s[ 8];
	pngr->public.colortype   = s[ 9];
	pngr->public.compression = s[10];
	pngr->public.filter      = s[11];
	pngr->public.interlace   = s[12];

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}

	if (isvalidmode(pngr->public.depth, pngr->public.colortype)) {
		/* only compresion method 0 is defined by the standard (deflate), the
		 * same for filter method */
		if (pngr->public.compression | pngr->public.filter) {
			goto L_ERROR;
		}
		if (pngr->public.interlace > 1) {
			goto L_ERROR;
		}

		/* set the color palette to red */
		if (pngr->public.colortype == 3) {
			uintxx i;

			for (i = 0; i < 256; i++) {
				pngr->public.palette[(i * 4) + 0] = 0xff;
				pngr->public.palette[(i * 4) + 1] = 0x00;
				pngr->public.palette[(i * 4) + 2] = 0x00;
				pngr->public.palette[(i * 4) + 3] = 0xff;
			}
		}
		return 1;
	}

L_ERROR:
	SETERROR(PNGR_EBADDATA);
	return 0;
}

/* critical chunks */
static bool parsePLTE(struct TPNGRPrvt*, struct TChunkHead);

/* ancillary chunks */
static bool parseSBIT(struct TPNGRPrvt*, struct TChunkHead);
static bool parseTRNS(struct TPNGRPrvt*, struct TChunkHead);
static bool parseCHRM(struct TPNGRPrvt*, struct TChunkHead);
static bool parseGAMA(struct TPNGRPrvt*, struct TChunkHead);
static bool parseICCP(struct TPNGRPrvt*, struct TChunkHead);
static bool parseSRGB(struct TPNGRPrvt*, struct TChunkHead);
static bool parseBKGD(struct TPNGRPrvt*, struct TChunkHead);
static bool parsePHYS(struct TPNGRPrvt*, struct TChunkHead);

static bool parseancillary(struct TPNGRPrvt*, uint32, struct TChunkHead);


static uintxx
parsechunks(struct TPNGRPrvt* pngr)
{
	uint32 fcc;

	for (;;) {
		struct TChunkHead head;

		head = getchunkhead(pngr);
		if (pngr->public.error) {
			/* failed to read the chunk head */
			return 0;
		}

		fcc = TOI32(head.fcc[0], head.fcc[1], head.fcc[2], head.fcc[3]);
		if (pngr->public.state == 3) {
			if (fcc == TOI32('I', 'D', 'A', 'T')) {
				initcrc32(pngr, CRC32_IDAT);
				pngr->remaining = head.length;
				return 1;
			}
			return 0;
		}

		switch (fcc) {
			case TOI32('I', 'E', 'N', 'D'):
				if (pngr->public.state == 4) {
					if (pngr->public.error) {
						return 0;
					}

					initcrc32(pngr, CRC32_IEND);
					checkcrc32(pngr);
					if (pngr->public.error) {
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
					if (pngr->public.error == 0)
						SETERROR(PNGR_EBADDATA);
					return 0;
				}
				continue;

			case TOI32('I', 'D', 'A', 'T'):
				if (pngr->public.state == 4) {
					SETERROR(PNGR_ECHUNKORDER);
					return 0;
				}

				if (pngr->public.colortype == 3) {
					if (pngr->public.palettesize == 0) {
						SETERROR(PNGR_EMISSINGCHUNK);
						return 0;
					}
				}

				initcrc32(pngr, CRC32_IDAT);
				pngr->remaining = head.length;
				return 1;

			default:
				break;
		}

		if (parseancillary(pngr, fcc, head) == 0) {
			if (pngr->public.error == 0) {
				SETERROR(PNGR_EBADDATA);
			}
			break;
		}
	}
	return 0;
}

CTB_INLINE bool
consumebytes(struct TPNGRPrvt* pngr, uintxx total)
{
	uintxx j;

	while (total) {
		j = sizeof(pngr->source);
		if (j > total) {
			j = total;
		}

		if (readinput(pngr, pngr->source, j) == 0) {
			return 0;
		}
		total -= j;
	}
	return 1;
}

static bool
parseancillary(struct TPNGRPrvt* pngr, uint32 fcc, struct TChunkHead head)
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
		if (consumebytes(pngr, head.length) == 0) {
			return 0;
		}
	}
	checkcrc32(pngr);
	if (pngr->public.error) {
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
setuppasses(struct TPNGRPrvt* pngr)
{
	uintxx sizex;
	uintxx sizey;
	uintxx i;

	for (i = 0; i < ADAM7PASSES; i++) {
		uint8 shiftx[] = {3, 3, 2, 2, 1, 1, 0};
		uint8 shifty[] = {3, 3, 3, 2, 2, 1, 1};

		sizex = pngr->public.sizex + STEP_X(i) - ORIGIN_X(i) - 1;
		sizey = pngr->public.sizey + STEP_Y(i) - ORIGIN_Y(i) - 1;
		sizex = sizex >> shiftx[i];
		sizey = sizey >> shifty[i];
		if (sizex == 0 || sizey == 0) {
			pngr->passrowsize[i] = 0;
			continue;
		}

		pngr->passrowsize[i] = sizex;
		if (pngr->public.depth < 8) {
			uint64 v;

			v = ((uint64) pngr->public.depth * sizex) + 7;
			pngr->passmemsize[i] = (uintxx) (v >> 3) + 1;
			continue;
		}

		pngr->passmemsize[i] = sizex * pngr->rawpelsize;
		pngr->passmemsize[i]++;
	}
	pngr->interpolate = 1;
}

CTB_INLINE bool
checklimits(uintxx sizex, uintxx sizey, uintxx pelsize)
{
#if defined(CTB_ENV64)
	uint64 v[1];

	/* internal memory */
	if (ckdu64_mul(sizex, pelsize << 1, v)) {
		return 0;
	}

	/* image size */
	if (ckdu64_mul(sizex, sizey, v) == 0) {
		if (ckdu64_mul(pelsize, v[0], v) == 0) {
			return 1;
		}
	}
	return 0;
#else
	uint32 v[1];

	/* internal memory */
	if (ckdu32_mul(sizex, pelsize << 1, v)) {
		return 0;
	}

	/* image size */
	if (ckdu32_mul(sizex, sizey, v) == 0) {
		if (ckdu32_mul(pelsize, v[0], v) == 0) {
			return 1;
		}
	}
	return 0;
#endif
}

CTB_INLINE bool
setvalues(struct TPNGRPrvt* pngr, struct TImageInfo* info)
{
	static const uintxx cmap[] = {
		1, 0, 3, 1, 2, 0, 4
	};
	uintxx pelsize;
	uintxx mode;
	uintxx r;

	switch (pngr->public.colortype) {
		case 0:
			if (pngr->hasalpha) {
				mode = IMAGE_GRAYALPHA;
				break;
			}
			mode = IMAGE_GRAY;
			break;
		case 2:
		case 3:
			if (pngr->hasalpha) {
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

	r = pelsize = cmap[pngr->public.colortype];
	if (pngr->public.colortype == 3) {
		pelsize += 2;
	}
	if (pngr->hasalpha) {
		pelsize++;
	}

	if (pngr->public.depth == 16) {
		pelsize = pelsize << 1;
	}
	if (checklimits(pngr->public.sizex, pngr->public.sizey, pelsize) == 0) {
		return 0;
	}
	r = r * ((pngr->public.depth + 7) >> 3);

	pngr->rawrowsize = pngr->rowmemory = (pngr->public.sizex * r) + 1;
	pngr->rawpelsize = r;
	if (pngr->public.depth < 8) {
		uint64 v;

		v = ((uint64) pngr->public.depth * pngr->public.sizex) + 7;
		pngr->rawrowsize = (uintxx) (v >> 3) + 1;

		/* extra bytes to do bit expansion on the row */
		switch (pngr->public.depth) {
			case 1: pngr->rowmemory += (8 - 1); break;
			case 2: pngr->rowmemory += (4 - 1); break;
			case 4: pngr->rowmemory += (2 - 1); break;
			default:
				break;
		}
	}

	/* extra bytes */
	pngr->rowmemory += 16;

	pngr->rowsize = pelsize * pngr->public.sizex;
	pngr->pelsize = pelsize;

	/* sets the imageinfo struct */
	info->sizex = pngr->public.sizex;
	info->sizey = pngr->public.sizey;
	info->colortype = mode;

	info->depth = 8;
	if (pngr->public.depth == 16) {
		info->depth = 16;
	}
	info->size = imginfo_getrowsize(info) * pngr->public.sizey;

	return 1;
}

uintxx
pngr_initdecoder(const TPNGReader* state, TImageInfo* info)
{
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state && info);

	pngr = CTB_CONSTCAST(state);
	if (pngr->public.state || pngr->iccpmode) {
		if (pngr->public.error == 0) {
			SETERROR(PNGR_EINCORRECTUSE);
		}
		goto L_ERROR;
	}

	/* at this point we need an input function */
	if (pngr->inputfn == NULL) {
		SETERROR(PNGR_EIOERROR);
		goto L_ERROR;
	}

	if (checksignature(pngr)) {
		struct TChunkHead head;

		head = getchunkhead(pngr);
		if (head.fcc[0] != 'I' ||
			head.fcc[1] != 'H' ||
			head.fcc[2] != 'D' || head.fcc[3] != 'R') {
			if (pngr->public.error == 0) {
				SETERROR(PNGR_EBADDATA);
			}
			goto L_ERROR;
		}

		/* parse all the chunks until the first IDAT chunk */
		if (parseIHDR(pngr, head) && parsechunks(pngr)) {
			if (setvalues(pngr, info)) {
				if (pngr->public.interlace) {
					setuppasses(pngr);
				}

				/* ready to start decoding */
				SETSTATE(1);
				pngr->public.requiredmemory = pngr->rowmemory << 1;
				return 1;
			}

			SETERROR(PNGR_ELIMIT);
		}
	}

L_ERROR:
	if (pngr->public.error == 0) {
		SETERROR(PNGR_EBADDATA);
	}
	SETSTATE(0xDEADBEEF);
	return 0;
}

void
pngr_setbuffers(const TPNGReader* state, uint8* pixels, uint8* idxs)
{
	uintxx i;
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state);

	pngr = CTB_CONSTCAST(state);
	if (pngr->public.state ^ 1) {
		SETSTATE(0xDEADBEEF);
		if (pngr->public.error == 0) {
			SETERROR(PNGR_EINCORRECTUSE);
		}
		return;
	}

	CTB_ASSERT(pngr->mainmemory == NULL);
	pngr->mainmemory = request_(pngr, pngr->public.requiredmemory);
	if (pngr->mainmemory == NULL) {
		SETSTATE(0xDEADBEEF);
		SETERROR(PNGR_EOOM);
		return;
	}
	pngr->mainmsize = pngr->public.requiredmemory;

	pngr->rbuffers[0] = pngr->mainmemory;
	pngr->rbuffers[1] = pngr->mainmemory + pngr->rowmemory;

	pngr->currrow = pngr->rbuffers[0];
	pngr->prevrow = pngr->rbuffers[1];
	if (pngr->public.interlace == 0) {
		for (i = 0; i < pngr->rowmemory; i++) {
			pngr->prevrow[i] = 0;
		}
	}

	pngr->pixels = pixels;
	if (pngr->public.colortype == 3) {
		pngr->idxs = idxs;
	}
	SETSTATE(2);
}

static bool
parsePLTE(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uint8* s;
	uintxx psize;
	uintxx limit;
	uintxx i;
	uintxx j;

	if (pngr->chunkmap.PLTE) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	pngr->chunkmap.PLTE = 1;

	/* it shall not appear for colour types 0 and 4 */
	if (pngr->public.colortype == 0 || pngr->public.colortype == 4) {
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
	if (pngr->public.colortype == 3) {
		limit = ((uintxx) 1) << pngr->public.depth;
	}
	if (psize > limit) {
		return 0;
	}

	initcrc32(pngr, CRC32_PLTE);

	s = pngr->public.palette;
	pngr->public.palettesize = psize;
	if (readinput(pngr, s, head.length) == 0) {
		return 0;
	}

	/* expand the palette from RGB to RGBA */
	j = psize * 4;
	i = psize * 3;
	while (i) {
		pngr->public.palette[--j] = 0xff;
		pngr->public.palette[--j] = pngr->public.palette[--i];
		pngr->public.palette[--j] = pngr->public.palette[--i];
		pngr->public.palette[--j] = pngr->public.palette[--i];
	}

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}
	return 1;
}


#define SETPROPERTY(CHUNK) (pngr->public.properties |= (CHUNK))

static bool
parseTRNS(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uint8 *s;

	if (pngr->chunkmap.TRNS) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->public.colortype == 3) {
			if (pngr->chunkmap.PLTE == 0) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	pngr->chunkmap.TRNS = 1;

	/* a tRNS chunk shall not appear for colour types 4 and 6, since a full
	 * alpha channel is already present in those cases */
	if (pngr->public.colortype == 4 || pngr->public.colortype == 6) {
		return 0;
	}

	initcrc32(pngr, CRC32_TRNS);
	s = pngr->source;
	if (pngr->public.colortype == 3) {
		intxx i;

		/* the tRNS chunk shall not contain more alpha values than there are
		 * palette entries, but a tRNS chunk may contain fewer values than
		 * there are palette entries */
		if (head.length > pngr->public.palettesize) {
			return 0;
		}
		if (pngr->public.palettesize == 0) {
			return 0;
		}

		if (readinput(pngr, s, head.length) == 0) {
			return 0;
		}
		for (i = 0; (intxx) head.length > i; i++) {
			pngr->public.palette[(i * 4) + 3] = s[i];
		}
	}
	else {
		if (pngr->public.colortype == 0) {
			if (head.length != 2 || readinput(pngr, s, 2) == 0) {
				return 0;
			}

			if (pngr->public.colortype ^ 16) {
				pngr->public.alpha[0] = (uint16) s[1];
			}
			else {
				pngr->public.alpha[0] = TOI16(s[0], s[1]);
			}
			/* unused values are zero */
		}
		if (pngr->public.colortype == 2) {
			if (head.length != 6 || readinput(pngr, s, 6) == 0) {
				return 0;
			}

			if (pngr->public.colortype ^ 16) {
				pngr->public.alpha[0] = (uint16) s[1];
				pngr->public.alpha[1] = (uint16) s[3];
				pngr->public.alpha[2] = (uint16) s[5];
			}
			else {
				pngr->public.alpha[0] = TOI16(s[0], s[1]);
				pngr->public.alpha[1] = TOI16(s[2], s[3]);
				pngr->public.alpha[2] = TOI16(s[4], s[5]);
			}
		}

		SETPROPERTY(PNGR_TRNS);
	}
	pngr->hasalpha = 1;

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}
	return 1;
}


#define SETWARNING(WARNING) (pngr->public.warnings |= (WARNING))

static bool
parseCHRM(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uintxx i;
	uint32 a;
	uint32 b;
	uint8* s;

	if (pngr->chunkmap.CHRM) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->public.colortype == 3) {
			if (pngr->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	pngr->chunkmap.CHRM = 1;

	initcrc32(pngr, CRC32_CHRM);
	s = pngr->source;
	if (head.length != 32 || readinput(pngr, s, 32) == 0) {
		return 0;
	}

	/* each value is encoded as a four-byte PNG unsigned integer,
	 * representing the x or y value times 100000 */
	a = TOI32(s[0], s[1], s[2], s[3]);
	s += 4;
	b = TOI32(s[0], s[1], s[2], s[3]);
	s += 4;
	pngr->public.wpointx = (flt32) a * 0.00001f;
	pngr->public.wpointy = (flt32) b * 0.00001f;

	if (a == 0 || b == 0) {
		SETWARNING(PNGR_BADCHRM);
	}

	for (i = 0; i < 3; i++) {
		a = TOI32(s[0], s[1], s[2], s[3]);
		s += 4;
		b = TOI32(s[0], s[1], s[2], s[3]);
		s += 4;

		pngr->public.chromax[i] = (flt32) a * 0.00001f;
		pngr->public.chromay[i] = (flt32) b * 0.00001f;
		if (a == 0 || b == 0) {
			SETWARNING(PNGR_BADCHRM);
		}
	}

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}

	if ((pngr->public.warnings & PNGR_BADCHRM) == 0) {
		SETPROPERTY(PNGR_CHRM);
	}
	return 1;
}

static bool
parseGAMA(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uintxx n;
	uint8* s;

	if (pngr->chunkmap.GAMA) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->public.colortype == 3) {
			if (pngr->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	pngr->chunkmap.GAMA = 1;

	initcrc32(pngr, CRC32_GAMA);
	s = pngr->source;
	if (head.length != 4 || readinput(pngr, s, 4) == 0) {
		return 0;
	}

	/* the value is encoded as a four-byte PNG unsigned integer,
	 * representing gamma times 100000 */
	n = TOI32(s[0], s[1], s[2], s[3]);
	pngr->public.gamma = (flt32) n * 0.00001f;

	if (n == 0) {
		SETWARNING(PNGR_BADGAMA);
	}
	else {
		SETPROPERTY(PNGR_GAMA);
	}

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}

	return 1;
}

static bool
parseSBIT(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uintxx size;
	uintxx j;
	uintxx i;
	uint8* s;

	if (pngr->chunkmap.SBIT) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->public.colortype == 3) {
			if (pngr->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	pngr->chunkmap.SBIT = 1;

	size = 0;
	switch (pngr->public.colortype) {
		case 0: size = 1; break;
		case 2:
		case 3: size = 3; break;
		case 4: size = 2; break;
		case 6: size = 4; break;
	}

	initcrc32(pngr, CRC32_SBIT);

	/* each depth specified in sBIT shall be greater than zero and less than
	 * or equal to the sample depth (which is 8 for indexed-colour images,
	 * and the bit depth given in IHDR for other colour types) */
	s = pngr->public.sbits;
	if (head.length != size || readinput(pngr, s, size) == 0) {
		return 0;
	}

	j = pngr->public.depth;
	if (pngr->public.colortype == 3)
		j = 8;

	for (i = 0; i < size; i++) {
		if (s[i] > j || s[i] == 0) {
			j = 0;  /* bad value */
			SETWARNING(PNGR_BADSBIT);
			break;
		}
	}

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}

	if (j != 0) {
		SETPROPERTY(PNGR_SBIT);
	}
	return 1;
}

static bool
parseSRGB(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uint8 s[1];

	if (pngr->chunkmap.SRGB) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->public.colortype == 3) {
			if (pngr->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	pngr->chunkmap.SRGB = 1;

	initcrc32(pngr, CRC32_SRGB);

	/* only 4 posible values: perceptual, relative, saturation, absolute */
	if (head.length != 1 || readinput(pngr, s, 1) == 0) {
		return 0;
	}

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}

	if (s[0] < 4) {
		SETPROPERTY(PNGR_SRGB);
		pngr->public.srgbintent = s[0];
	}
	else {
		SETWARNING(PNGR_BADSRGB);
	}
	return 1;
}

static bool
parseBKGD(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uintxx size;
	uintxx entry;
	uint8* s;

	if (pngr->chunkmap.BKGD) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->public.colortype == 3) {
			if (pngr->chunkmap.PLTE == 0) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	pngr->chunkmap.BKGD = 1;

	size = 0;
	switch (pngr->public.colortype) {
		case 0: size = 2; break;
		case 2:
		case 6: size = 6; break;
		case 3: size = 1; break;
	}

	initcrc32(pngr, CRC32_BKGD);
	s = pngr->source;
	if (head.length != size || readinput(pngr, s, size) == 0) {
		return 0;
	}

	/* palette index  */
	if (size == 1) {
		entry = s[0] * 3;
		if (pngr->hasalpha) {
			entry += s[0];
		}
		pngr->public.background[0] = pngr->public.palette[entry + 0];
		pngr->public.background[1] = pngr->public.palette[entry + 1];
		pngr->public.background[2] = pngr->public.palette[entry + 2];
	}
	else {
		pngr->public.background[0] = TOI16(s[0], s[1]);
		s += 2;
		if (size > 2) {
			pngr->public.background[1] = TOI16(s[0], s[1]);
			s += 2;
			pngr->public.background[2] = TOI16(s[0], s[1]);
			s += 2;
		}
	}

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}

	SETPROPERTY(PNGR_BKGD);
	return 1;
}

static bool
parsePHYS(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	uint8* s;

	if (pngr->chunkmap.PHYS) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	pngr->chunkmap.PHYS = 1;

	initcrc32(pngr, CRC32_PHYS);
	s = pngr->source;
	if (head.length != 9 || readinput(pngr, s, 9) == 0) {
		return 0;
	}
	pngr->public.physx = TOI32(s[0], s[1], s[2], s[3]);
	s += 4;
	pngr->public.physy = TOI32(s[0], s[1], s[2], s[3]);
	s += 4;

	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}

	/* the following values are defined for the unit specifier:
	 * 0 unit is unknown
	 * 1 unit is the metre */
	pngr->public.physunit = s[0];
	if (s[0] == 0 || s[0] == 1) {
		SETPROPERTY(PNGR_PHYS);
	}
	else {
		SETWARNING(PNGR_BADPHYS);
	}
	return 1;
}


uintxx
pngr_readICCP(const TPNGReader* state, uint8* target)
{
	uintxx r;
	uintxx total;
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state);

	pngr = CTB_CONSTCAST(state);
	if (pngr->public.state == 0xDEADBEEF) {
		return 0;
	}
	if (pngr->iccpmode ^ 1) {
		SETERROR(PNGR_EINCORRECTUSE);
		SETSTATE(0xDEADBEEF);
		return 0;
	}
	else {
		switch (pngr->public.state) {
			case 0:
				break;
			case 1:
			case 2:
			case 3:
				if (pngr->public.error == 0) {
					SETERROR(PNGR_EINCORRECTUSE);
				}
				SETSTATE(0xDEADBEEF);
				return 0;
			case 4:
			case 5:
				return 0;
		}
	}

	if (target == NULL) {
		return 1;
	}

	ctb_memcpy(target, pngr->source, 128);
	target += 128;

	total = pngr->iccpsize - 128;
	r = zstrm_inflate(pngr->zstrm, target, total);
	if (r ^ total || pngr->zstrm->error) {
		SETWARNING(PNGR_BADICCP);
		return 0;
	}
	return 1;
}


static intxx
ICCPsourcefn(uint8* source, uintxx total, struct TPNGRPrvt* pngr)
{
	uintxx r;

	if (pngr->remaining == 0) {
		return 0;
	}

	r = total;
	if (r > pngr->remaining) {
		r = pngr->remaining;
	}
	if (readinput(pngr, source, r) == 0) {
		return -1;
	}

	pngr->remaining -= r;
	return (intxx) r;
}

CTB_INLINE uintxx
parseICCPheader(uint8* s)
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
	return size;
}

static bool
readICCP(struct TPNGRPrvt* pngr, uintxx chunksize)
{
	uintxx r;
	uintxx total;
	uint8* s;

	total = 80;
	if (total > chunksize) {
		total = chunksize;
	}

	/* 1-79 + NULL + compression method + zlib compressed data */
	s = pngr->source;
	for (r = 0; r < total; r++) {
		if (readinput(pngr, s + r, 1) == 0) {
			return 0;
		}
		if (s[r] == 0x00) {
			r++;
			break;
		}
	}

	r = chunksize - r;
	if (r == 0) {
		return 0;
	}

	if (readinput(pngr, s, 1) == 0) {
		return 0;
	}
	r--;
	if (s[0]) {
		consumebytes(pngr, r);
		return 0;
	}
	s = pngr->source;

	pngr->remaining = r;
	zstrm_setsourcefn(pngr->zstrm, (TZStrmIFn) ICCPsourcefn, pngr);

	r = zstrm_inflate(pngr->zstrm, s, 128);
	if (r == 128) {
		total = parseICCPheader(s);
		if (total == 0 || total > MAXICCPSIZE) {
			r = 0;
			goto L1;
		}
		pngr->iccpsize = total;

		pngr->iccpmode = 1;
		pngr->iccpfn((void*) pngr, total, pngr->iccpuser);
		r = 1;
	}
	else {
		r = 0;
	}

L1:
	zstrm_reset(pngr->zstrm);
	if (pngr->remaining) {
		consumebytes(pngr, pngr->remaining);
		if (pngr->public.error) {
			return 0;
		}
	}
	return (bool) r;
}

static bool
parseICCP(struct TPNGRPrvt* pngr, struct TChunkHead head)
{
	if (pngr->chunkmap.ICCP) {
		SETERROR(PNGR_EDUPLICATEDCHUNK);
		return 0;
	}
	if (pngr->public.state == 4) {
		SETERROR(PNGR_ECHUNKORDER);
		return 0;
	}
	else {
		if (pngr->public.colortype == 3) {
			if (pngr->chunkmap.PLTE) {
				SETERROR(PNGR_ECHUNKORDER);
				return 0;
			}
		}
	}
	pngr->chunkmap.ICCP = 1;

	initcrc32(pngr, CRC32_ICCP);
	if (pngr->iccpfn) {
		if (readICCP(pngr, head.length)) {
			SETPROPERTY(PNGR_ICCP);
		}
		else {
			if (pngr->public.error) {
				return 0;
			}
			SETWARNING(PNGR_BADICCP);
		}
	}
	else {
		if (consumebytes(pngr, head.length) == 0) {
			return 0;
		}
	}
	checkcrc32(pngr);
	if (pngr->public.error) {
		return 0;
	}
	return 1;
}

#undef SETPROPERTY
#undef SETWARNING

#undef TOI32
#undef TOI16


CTB_INLINE bool
readnextIDAT(struct TPNGRPrvt* pngr)
{
	uintxx r;

	if (pngr->public.error) {
		return 0;
	}
	r = parsechunks(pngr);
	if (r == 0) {
		SETERROR(PNGR_EDEFLATE);
		return 0;
	}
	return 1;
}

static intxx
IDATsourcefn(uint8* source, uintxx total, struct TPNGRPrvt* pngr)
{
	uintxx remaining;
	uintxx r;
	uint8* s;

	s = source;
	if (pngr->remaining == 0) {
		if (readnextIDAT(pngr) == 0) {
			return -1;
		}
	}
	remaining = pngr->remaining;

	if (remaining) {
		r = total;
		if (total > remaining) {
			r = remaining;
		}
		if (readinput(pngr, s, r) == 0) {
			return -1;
		}
		s += r;

		remaining -= r;
		if (r == total) {
			return (intxx) (s - source);
		}
		total -= r;
	}

	if (remaining == 0) {
		checkcrc32(pngr);
		pngr->remaining = 0;
	}
	else {
		pngr->remaining = remaining;
	}
	return (intxx) (s - source);
}


#if defined(PNGR_CFG_EXTERNALASM)

extern void pngr_unfilterASM(uint8*, uint8*, uintxx, uintxx);

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
unfilter(uint8* curr, uint8* prev, uintxx size, uintxx fp)
{
	uintxx i;
	uintxx psize;

	switch (fp >> 16) {
		case 1:
			psize = fp & 0xffff;
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
			psize = fp & 0xffff;
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
			psize = fp & 0xffff;
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
			i = (((intxx) size * 1) + 7) >> 3;
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
			i = (((intxx) size * 2) + 7) >> 3;
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
			i = (((intxx) size * 4) + 7) >> 3;
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
decoderow(struct TPNGRPrvt* pngr, uintxx sizex, uintxx rowsize)
{
	uint8* curr;
	uint8* prev;
	uintxx filter;
	uintxx r;

	pngr->currrow = pngr->rbuffers[0];
	if (pngr->prevrow == pngr->rbuffers[0]) {
		pngr->currrow = pngr->rbuffers[1];
	}

	curr = pngr->currrow;
	prev = pngr->prevrow;
	r = zstrm_inflate(pngr->zstrm, curr, rowsize);
	if (r ^ rowsize) {
		if (pngr->public.error == 0) {
			SETERROR(PNGR_EBADDATA);
		}
		SETSTATE(0xDEADBEEF);
		return NULL;		
	}

	filter = curr[0];
	curr++;
	prev++;
	if (CTB_EXPECT1(filter)) {
		if (CTB_EXPECT0(filter > 4)) {
			SETERROR(PNGR_EBADDATA);
			SETSTATE(0xDEADBEEF);
			return NULL;
		}
		UNFILTER(curr, prev, rowsize - 1, (filter << 16) | pngr->rawpelsize);
	}

	if (CTB_EXPECT0(pngr->public.depth < 8)) {
		unpack(curr, sizex, pngr->public.depth);
	}

	/* swap rows */
	pngr->prevrow = curr - 1;
	pngr->currrow = prev - 1;
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
setrow(struct TPNGRPrvt* pngr, uint8* pixels, uint8* row)
{
	uintxx i;

	if (pngr->hasalpha) {
		if (pngr->public.colortype == 0 || pngr->public.colortype == 2) {
			if (pngr->public.depth ^ 16) {
				uint8 sample[4];

				sample[0] = (uint8) pngr->public.alpha[0];
				sample[1] = (uint8) pngr->public.alpha[1];
				sample[2] = (uint8) pngr->public.alpha[2];
				if (pngr->public.colortype == 0) {
					for (i = 0; i < pngr->public.sizex; i++) {
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
					for (i = 0; i < pngr->public.sizex; i++) {
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

				sample = (uint8*) pngr->public.alpha;
				if (pngr->public.colortype == 0) {
					for (i = 0; i < pngr->public.sizex; i++) {
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
					for (i = 0; i < pngr->public.sizex; i++) {
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
	if (pngr->public.depth == 16) {
		uintxx total;

		total = pngr->public.sizex * (pngr->pelsize >> 1);
		for (i = 0; i < total; i++) {
			*pixels++ = row[1];
			*pixels++ = row[0];
			row += 2;
		}
		return;
	}
#endif

	ctb_memcpy(pixels, row, pngr->rowsize);
}

uintxx
pngr_decodeimg(const TPNGReader* state)
{
	uintxx i;
	uintxx j;
	uint8* pixels;
	uint8* idxs;
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state);

	pngr = CTB_CONSTCAST(state);
	if (pngr->public.state ^ 3) {
		if (pngr->public.state == 2) {
			SETSTATE(3);
			zstrm_setsourcefn(pngr->zstrm, (TZStrmIFn) IDATsourcefn, pngr);
		}
		else {
			SETSTATE(0xDEADBEEF);
			if (pngr->public.error == 0) {
				SETERROR(PNGR_EINCORRECTUSE);
			}
			return 0;
		}
	}

	if (pngr->public.interlace) {
		pngr->interpolate = 0;
		switch (pngr->pass) {
			case 0: pngr_decodepass(state);  /* fallthrough */
			case 1: pngr_decodepass(state);  /* fallthrough */
			case 2: pngr_decodepass(state);  /* fallthrough */
			case 3: pngr_decodepass(state);  /* fallthrough */
			case 4: pngr_decodepass(state);  /* fallthrough */
			case 5: pngr_decodepass(state);  /* fallthrough */
			case 6: pngr_decodepass(state);  /* fallthrough */
			default:
				break;
		}
		if (pngr->public.state == 4 || pngr->public.state == 5) {
			return 1;
		}
		return 0;
	}

	pixels = pngr->pixels;
	idxs   = pngr->idxs;
	for (i = 0; i < pngr->public.sizey; i++) {
		uint8* row;

		row = decoderow(pngr, pngr->public.sizex, pngr->rawrowsize);
		if (CTB_EXPECT0(row == NULL)) {
			SETSTATE(0xDEADBEEF);
			return 0;
		}

		if (CTB_EXPECT1(pixels != NULL)) {
			if (CTB_EXPECT0(pngr->public.colortype == 3)) {
				uintxx entry;

				/* we don't check the range here */
				if (pngr->hasalpha) {
					for (j = 0; j < pngr->public.sizex; j++) {
						entry = row[j] * 4;

						*pixels++ = pngr->public.palette[entry + 0];
						*pixels++ = pngr->public.palette[entry + 1];
						*pixels++ = pngr->public.palette[entry + 2];
						*pixels++ = pngr->public.palette[entry + 3];
					}
				}
				else {
					for (j = 0; j < pngr->public.sizex; j++) {
						entry = row[j] * 4;
						*pixels++ = pngr->public.palette[entry + 0];
						*pixels++ = pngr->public.palette[entry + 1];
						*pixels++ = pngr->public.palette[entry + 2];
					}
				}
			}
			else {
				setrow(pngr, pixels, row);
				pixels += pngr->rowsize;
			}
		}

		if (CTB_EXPECT1(idxs != NULL)) {
			for (j = 0; j < pngr->public.sizex; j++) {
				idxs[j] = row[j];
			}
			idxs += pngr->public.sizex;
		}
	}

	SETSTATE(4);
	if (parsechunks(pngr) == 0 || pngr->public.warnings) {
		SETSTATE(5);
	}
	return 1;
}


CTB_INLINE uint8*
getsample(struct TPNGRPrvt* pngr, uint8* source, uint8* pixel)
{
	if (pngr->public.colortype == 3) {
		uintxx j;

		/* we don't check the range here */
		if (pngr->hasalpha) {
			j = source[0] * 4;
			pixel[0] = pngr->public.palette[j + 0];
			pixel[1] = pngr->public.palette[j + 1];
			pixel[2] = pngr->public.palette[j + 2];
			pixel[3] = pngr->public.palette[j + 3];
		}
		else {
			j = source[0] * 4;
			pixel[0] = pngr->public.palette[j + 0];
			pixel[1] = pngr->public.palette[j + 1];
			pixel[2] = pngr->public.palette[j + 2];
		}
		return pixel;
	}

	if (pngr->hasalpha) {
		if (pngr->public.colortype == 0 || pngr->public.colortype == 2) {
			if (pngr->public.depth ^ 16) {
				uint8 sample[4];

				sample[0] = (uint8) pngr->public.alpha[0];
				sample[1] = (uint8) pngr->public.alpha[1];
				sample[2] = (uint8) pngr->public.alpha[2];
				if (pngr->public.colortype == 0) {
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

				sample = (uint8*) pngr->public.alpha;
				if (pngr->public.colortype == 0) {
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
	if (pngr->public.depth == 16) {
		uint16* p;

		p = (void*) pixel;
		switch (pngr->pelsize) {
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


CTB_INLINE void
fill(struct TPNGRPrvt* pngr, uint8* offset, uint8* s, uintxx x2, uintxx y2)
{
	uintxx x;
	uintxx y;
	uint8* position;

	for (y = 0; y < y2; y++) {
		position = offset + (y * pngr->rowsize);

		for (x = 0; x < x2; x++) {
			switch (pngr->pelsize) {
				case 8: position[7] = s[7];
				/* 7 */ position[6] = s[6];  /* fallthrough */
				case 6: position[5] = s[5];
				/* 5 */ position[4] = s[4];  /* fallthrough */
				case 4: position[3] = s[3];  /* fallthrough */
				case 3: position[2] = s[2];  /* fallthrough */
				case 2: position[1] = s[1];  /* fallthrough */
				case 1: position[0] = s[0];
			}
			position += pngr->pelsize;
		}
	}
}

uintxx
pngr_decodepass(const TPNGReader* state)
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
	struct TPNGRPrvt* pngr;
	CTB_ASSERT(state);

	pngr = CTB_CONSTCAST(state);
	if (pngr->public.state ^ 3) {
		if (pngr->public.state == 2) {
			SETSTATE(3);
			zstrm_setsourcefn(pngr->zstrm, (TZStrmIFn) IDATsourcefn, pngr);
		}
		else {
			SETSTATE(0xDEADBEEF);
			if (pngr->public.error == 0) {
				SETERROR(PNGR_EINCORRECTUSE);
			}
			return 0;
		}
	}

	i = pngr->pass;
	rowsize = pngr->passrowsize[i];
	memsize = pngr->passmemsize[i];
	if (rowsize == 0) {
		/* empty pass */
		goto L_DONE;
	}

	/* init the row buffers */
	pngr->prevrow = pngr->rbuffers[0];
	pngr->currrow = pngr->rbuffers[1];
	ctb_memset(pngr->prevrow, 0, memsize);

	/* */
	stepx = STEP_X(i) * pngr->pelsize;
	stepy = STEP_Y(i) * pngr->rowsize;

	/* target buffer */
	peloffsety  = pngr->pixels;
	peloffsety += ORIGIN_X(i) * pngr->pelsize;
	peloffsety += ORIGIN_Y(i) * pngr->rowsize;

	idxoffsety  = pngr->idxs;
	idxoffsety += ORIGIN_X(i);
	idxoffsety += ORIGIN_Y(i) * pngr->public.sizex;

	for (y = ORIGIN_Y(i); y < pngr->public.sizey; y += STEP_Y(i)) {
		uint8* rowpointer;
		uint8* row;
		uint8* sample;

		rowpointer = decoderow(pngr, rowsize, memsize);
		if (rowpointer == NULL) {
			SETSTATE(0xDEADBEEF);
			return 0;
		}

		row = rowpointer;
		if (CTB_EXPECT1(pngr->pixels != NULL)) {
			offsetx = peloffsety;
			if (pngr->interpolate) {
				uintxx sx;
				uintxx sy;

				for (x = ORIGIN_X(i); x < pngr->public.sizex; x += STEP_X(i)) {
					const uintxx passsizex[] = {8, 4, 4, 2, 2, 1, 1};
					const uintxx passsizey[] = {8, 8, 4, 4, 2, 2, 1};
					uint8 pixel[8];

					sx = pngr->public.sizex - x;
					sy = pngr->public.sizey - y;
					if (CTB_EXPECT0(sx > passsizex[i])) sx = passsizex[i];
					if (CTB_EXPECT0(sy > passsizey[i])) sy = passsizey[i];

					sample = getsample(pngr, row, pixel);
					fill(pngr, offsetx, sample, sx, sy);

					row += pngr->rawpelsize;
					offsetx += stepx;
				}
			}
			else {
				for (x = ORIGIN_X(i); x < pngr->public.sizex; x += STEP_X(i)) {
					uint8 pixel[8];

					sample = getsample(pngr, row, pixel);
					switch (pngr->pelsize) {
						case 8: offsetx[7] = sample[7];
						/* 7 */ offsetx[6] = sample[6];  /* fallthrough */
						case 6: offsetx[5] = sample[5];
						/* 5 */ offsetx[4] = sample[4];  /* fallthrough */
						case 4: offsetx[3] = sample[3];  /* fallthrough */
						case 3: offsetx[2] = sample[2];  /* fallthrough */
						case 2: offsetx[1] = sample[1];  /* fallthrough */
						case 1: offsetx[0] = sample[0];
					}
					row += pngr->rawpelsize;
					offsetx += stepx;
				}
			}
			peloffsety += stepy;
		}

		row = rowpointer;
		if (CTB_EXPECT1(pngr->idxs != NULL)) {
			offsetx = idxoffsety;

			for (x = ORIGIN_X(i); x < pngr->public.sizex; x += STEP_X(i)) {
				offsetx[0] = *row++;
				offsetx += STEP_X(i);
			}
			idxoffsety += STEP_Y(i) * pngr->public.sizex;
		}
	}

L_DONE:
	r = ++pngr->pass;
	if (r == ADAM7PASSES) {
		SETSTATE(4);
		if (parsechunks(pngr) == 0 || pngr->public.warnings) {
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
