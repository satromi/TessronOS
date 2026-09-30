/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	txc_tab.h
 *	The tables txc_tab.c holds (made by tools/gen_txc.py)
 *
 *	A place of the grid is a pointer, (row - 1) * 94 + (cell - 1).
 *	Rows 1 to 94 are in txc_main, rows 115 to 120 in txc_ibm; rows 95 to
 *	114 are the private use area, U+E000 on, and kept in no table.
 */

#ifndef __TXC_TAB_H__
#define __TXC_TAB_H__

#define TXC_MAIN_N	8836		/* rows 1 to 94 */
#define TXC_PUA_FROM	8836		/* row 95 */
#define TXC_PUA_TO	10716		/* past row 114 */
#define TXC_IBM_FROM	10716		/* row 115 */
#define TXC_IBM_N	564		/* rows 115 to 120 */
#define TXC_NEC_IBM_FROM 8272		/* row 89: the NEC selected IBM extensions */
#define TXC_BACK_N	7724

IMPORT CONST UH txc_main[TXC_MAIN_N];
IMPORT CONST UH txc_ibm[TXC_IBM_N];
IMPORT CONST UH txc_back[TXC_BACK_N][2];

#endif /* __TXC_TAB_H__ */
