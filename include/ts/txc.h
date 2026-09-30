/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	txc.h
 *	Text in the encodings of Japanese files, to and from UTF-8
 *	(design 18.20)
 *
 *	The system keeps text in UTF-8. A file from outside may be Shift_JIS
 *	(as the Windows code page 932 has it, with the NEC and IBM
 *	extensions), EUC-JP, ISO-2022-JP, UTF-16 or UTF-8; these calls turn
 *	it into UTF-8 and back, and guess which one a file is. Shift_JIS,
 *	EUC-JP and ISO-2022-JP share one table of JIS X 0208, so a character
 *	one of them has decodes the same whichever it came in.
 *
 *	JIS X 0212 (補助漢字) is read and written in EUC-JP (SS3) and in
 *	ISO-2022-JP (ESC $ ( D, as ISO-2022-JP-2 has it) when the program
 *	links lib/libbpk's table of it (txc_has_x0212).
 *
 *	A character that cannot be read, or that the encoding written has no
 *	place for, becomes 〓 (U+3013, the geta mark), and is counted.
 *
 *	Nothing here calls the system: the same file serves the kernel and a
 *	process.
 */

#ifndef __TS_TXC_H__
#define __TS_TXC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Encodings */
#define TXC_AUTO	0		/* to be found out (txc_detect) */
#define TXC_UTF8	1
#define TXC_SJIS	2		/* Shift_JIS, code page 932 */
#define TXC_EUCJP	3
#define TXC_JIS		4		/* ISO-2022-JP */
#define TXC_UTF16LE	5
#define TXC_UTF16BE	6
#define TXC_NENC	7

#define TXC_GETA	0x3013		/* what a character with no place becomes */

/*
 * The encoding of a text, from its first bytes: a byte order mark says
 * it outright; otherwise escape sequences mean ISO-2022-JP, well formed
 * multibyte UTF-8 means UTF-8, and between Shift_JIS and EUC-JP the one
 * that reads with fewer faults and more kana wins. Text of ASCII alone
 * is UTF-8.
 */
IMPORT INT txc_detect( CONST UB *s, SZ n );

/* The bytes of a byte order mark at the start, and the encoding it names */
IMPORT INT txc_bom( CONST UB *s, SZ n, INT *p_enc );

/*
 * Text in an encoding turned into UTF-8. Whole characters are written
 * to out while they fit and it ends in a nought; the answer is how
 * many bytes the whole would take (out may be NULL to ask only that).
 * A byte order mark is passed over. *p_bad (may be NULL) is how many
 * characters became 〓.
 */
IMPORT SZ  txc_to_utf8( INT enc, CONST UB *s, SZ n, UB *out, SZ max, INT *p_bad );

/* UTF-8 turned into an encoding, the same way (no byte order mark is written) */
IMPORT SZ  txc_from_utf8( INT enc, CONST UB *s, SZ n, UB *out, SZ max, INT *p_bad );

/* The name of an encoding as a file's metadata keeps it ("shift_jis"), and back */
IMPORT CONST char *txc_name( INT enc );
IMPORT INT txc_by_name( CONST UB *name );

/*
 * One character. txc_utf8_get reads one from UTF-8 (the bytes it took,
 * 0 at the end; a malformed one takes one byte and reads as -1);
 * txc_utf8_put writes one (the bytes, at most 4).
 */
IMPORT INT txc_utf8_get( CONST UB *s, SZ n, INT *p_c );
IMPORT INT txc_utf8_put( INT c, UB *out );

/*
 * A character of JIS X 0208 by row and cell (1 to 94; 0 for none), and
 * the Shift_JIS code of a character (0 when it has none; below 0x100 a
 * single byte).
 */
IMPORT INT  txc_jis_char( INT row, INT cell );
IMPORT BOOL txc_has_x0212( void );
IMPORT UINT txc_sjis_code( INT c );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TXC_H__ */
