/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ctype.h
 *	<ctype.h> for the ASCII letters wpa_supplicant looks at
 */

#ifndef __TSWPA_CTYPE_H__
#define __TSWPA_CTYPE_H__

static inline int isdigit( int c ) { return c >= '0' && c <= '9'; }
static inline int isxdigit( int c ) { return isdigit(c) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ); }
static inline int isupper( int c ) { return c >= 'A' && c <= 'Z'; }
static inline int islower( int c ) { return c >= 'a' && c <= 'z'; }
static inline int isalpha( int c ) { return isupper(c) || islower(c); }
static inline int isalnum( int c ) { return isalpha(c) || isdigit(c); }
static inline int isspace( int c ) { return c == ' ' || ( c >= '\t' && c <= '\r' ); }
static inline int isprint( int c ) { return c >= 0x20 && c < 0x7f; }
static inline int toupper( int c ) { return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower( int c ) { return isupper(c) ? c - 'A' + 'a' : c; }

#endif
