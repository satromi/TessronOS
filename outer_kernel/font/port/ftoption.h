/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ftoption.h
 *	What FreeType may and may not use here (design 16.6)
 *
 *	The upstream file is taken as it is and then the things that have
 *	no meaning inside a kernel are switched off again. Doing it this
 *	way rather than by keeping an edited copy means an upstream change
 *	to any other option arrives by itself, and what TessronOS decided is
 *	the short list below.
 *
 *	A font arrives as bytes that somebody else has already read, so
 *	nothing here opens a file, looks at an environment, or decompresses
 *	anything.
 */

#ifndef __TS_FTOPTION_H__
#define __TS_FTOPTION_H__

/* everything the upstream decides, first */
#include <freetype/config/ftoption.h>

/*
 * No compressed font files: the gzip and bzip2 sources are not built in,
 * and a font is handed over as a plain block of bytes.
 */
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_USE_BROTLI

/*
 * No Mac resource forks. Reading one means opening a file by name, and
 * the font layer never has a name to open.
 */
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK

/* No environment to read properties out of */
#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES

#endif /* __TS_FTOPTION_H__ */
