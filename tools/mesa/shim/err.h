/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	err.h (probe)
 *	The BSD error reporting calls some of Mesa's sources include.
 */

#ifndef TS_PROBE_ERR_H
#define TS_PROBE_ERR_H

void	err( int eval, const char *fmt, ... ) __attribute__((noreturn));
void	errx( int eval, const char *fmt, ... ) __attribute__((noreturn));
void	warn( const char *fmt, ... );
void	warnx( const char *fmt, ... );

#endif /* TS_PROBE_ERR_H */
