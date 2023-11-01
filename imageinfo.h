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

#ifndef c2c1cf9b_51ac_4737_8c7c_b6c7e6624943
#define c2c1cf9b_51ac_4737_8c7c_b6c7e6624943

/*
 * imageinfo.h
 * Shared struct used to retrieve image information.
 */

#include <ctoolbox.h>


/* supported color modes for the resulting decoding */
typedef enum  {
	IMAGE_INVALID   = 0,
	IMAGE_GRAY      = 1,
	IMAGE_GRAYALPHA = 2,
	IMAGE_RGB       = 3,
	IMAGE_RGBALPHA  = 4,
	IMAGE_YCBCR     = 5
} eColorType;


/* */
struct TImageInfo {
	/* image size */
	uintxx sizex;
	uintxx sizey;

	uintxx colortype;  /* color mode */
	uintxx depth;      /* bits per channel */

	/* size in bytes */
	uintxx size;
};

typedef struct TImageInfo TImageInfo;


/*
 * IO function prototype.
 * Return value must be the number of bytes readed to the buffer
 * (zero if there is no more input avaible or -1 if there is an error). */
typedef intxx (*TIMGInputFn)(uint8* buffer, uintxx size, void* user);


/*
 * */
CTB_INLINE uintxx imginfo_getpelsize(TImageInfo* imginfo);

/*
 * */
CTB_INLINE uintxx imginfo_getrowsize(TImageInfo* imginfo);


/*
 * Inlines */

CTB_INLINE uintxx
imginfo_getpelsize(TImageInfo* imginfo)
{
	uintxx pelsize;
	CTB_ASSERT(imginfo);

	pelsize = 0;
	switch (imginfo->colortype) {
		case IMAGE_GRAY:      pelsize = 1; break;
		case IMAGE_GRAYALPHA: pelsize = 2; break;
		case IMAGE_RGBALPHA:  pelsize = 4; break;
		case IMAGE_RGB:
		case IMAGE_YCBCR:
			pelsize = 3; break;
	}
	return pelsize * (imginfo->depth >> 3);
}

CTB_INLINE uintxx
imginfo_getrowsize(TImageInfo* imginfo)
{
	CTB_ASSERT(imginfo);

	return imginfo_getpelsize(imginfo) * imginfo->sizex;
}


#endif
