/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fsname.h
 *	Names between the UTF-8 of the file layer and the UTF-16 of the
 *	volumes of Windows (fsname.c).
 */

#ifndef __FSNAME_H__
#define __FSNAME_H__

/*
 * UTF-8 to UTF-16 units: how many, or -1 for bytes that are not UTF-8,
 * -2 when more than max are needed.
 */
IMPORT INT knl_fs_utf8_to_16( CONST UB *s, INT len, UH *out, INT max );

/*
 * UTF-16 units to UTF-8, not terminated: how many bytes, -1 when they do
 * not fit in max. A surrogate without its pair comes out as the code
 * it is, as Windows lets such a name be.
 */
IMPORT INT knl_fs_utf16_to_8( CONST UH *in, INT n, UB *out, INT max );

#endif /* __FSNAME_H__ */
