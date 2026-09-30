/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.00
 *
 *    Copyright (C) 2006-2019 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2019/12/11.
 *
 *----------------------------------------------------------------------
 */

/*
 *	string.c
 *	T-Kernel standard library 
 */

#include <tk/tkernel.h>
#include "kernel.h"

/*** binary operation ***/
/* memset : fill memory area */
void* knl_memset( void *s, int c, SZ n )
{
	register unsigned char *cp, cval;
	register UD *lp, lval;

	cp = (unsigned char *)s;
	cval = (unsigned char)c;

	if (n < 16) {
		while (n-- > 0) {
			*cp++ = cval;
		}
		return s;
	}

	/* align to 8 bytes, then fill by 64 bit words */
	while (((UBINT)cp & 7) != 0) {
		--n;
		*cp++ = cval;
	}

	lp = (UD *)cp;
	lval = (UD)cval * 0x0101010101010101ULL;

	while (n >= 8) {
		*lp++ = lval;
		n -= 8;
	}

	cp = (unsigned char *)lp;
	while (n > 0) {
		--n;
		*cp++ = cval;
	}

	return s;
}

void* knl_memcpy( void *dst, const void *src, SZ n )
{
	register unsigned char *cdst, *csrc;

	cdst = (unsigned char *)dst;
	csrc = (unsigned char *)src;

	/* the same place within 8 bytes: bytes up to the boundary, then
	   64 bit words, four at a time */
	if (n >= 32 && (((UBINT)cdst ^ (UBINT)csrc) & 7) == 0) {
		register UD *ldst, *lsrc;

		while (((UBINT)cdst & 7) != 0) {
			--n;
			*cdst++ = *csrc++;
		}
		ldst = (UD *)cdst;
		lsrc = (UD *)csrc;
		while (n >= 32) {
			ldst[0] = lsrc[0];
			ldst[1] = lsrc[1];
			ldst[2] = lsrc[2];
			ldst[3] = lsrc[3];
			ldst += 4;
			lsrc += 4;
			n -= 32;
		}
		while (n >= 8) {
			*ldst++ = *lsrc++;
			n -= 8;
		}
		cdst = (unsigned char *)ldst;
		csrc = (unsigned char *)lsrc;
	}
	while (n-- > 0) {
		*cdst++ = *csrc++;
	}

	return dst;
}

/* strlen : get text string length */
SZ knl_strlen( const char *s )
{
	register char *cp;

	cp = (char *)s;
	while (*cp) {
		++cp;
	}
	return (SZ)(cp - s);
}

/* strcpy : copy text string */
char* knl_strcpy( char *dst, const char *src )
{
	register char *cp;

	cp = dst;
	do {
		*cp++ = *src;
	} while (*src++);

	return dst;
}

char* knl_strncpy( char *dst, const char *src, SZ n )
{
	register char *cp;

	cp = dst;
	do {
		if (n-- <= 0) {
			return dst;
		}
		*cp++ = *src;
	} while (*src++);

	while (n-- > 0) {
		*cp++ = 0;
	}

	return dst;
}

/* strcmp : perform text string comparison */
int knl_strcmp( const char *s1, const char *s2 )
{
	register int result;

	while (*s1) {
		result = (unsigned char)*s1++ - (unsigned char)*s2++;
		if (result) {
			return result;
		}
	}

	return (unsigned char)*s1 - (unsigned char)*s2;
}

/* strcat : perform text string concatenation */
char* knl_strcat( char *dst, const char *src )
{
	register char *cp;

	cp = dst;
	while (*cp) {
		++cp;
	}

	while (*src) {
		*cp++ = *src++;
	}
	*cp = '\0';

	return dst;
}
