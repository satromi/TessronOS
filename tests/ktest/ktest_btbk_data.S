/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_btbk_data.S
 *	What ktest_btbk reads, built into the kernel: a backup volume of
 *	four cabinets that refer to one another, and three records of
 *	etc/xtad. Paths are from build_make, where make runs.
 */

	.section .rodata.ktest_btbk, "a"

	.balign	8
	.global	kt_bk_vol
	.global	kt_bk_vol_end
kt_bk_vol:
	.incbin	"../tests/ktest/data/backup.TAD"
kt_bk_vol_end:

	/* a figure: two links, a line with an arrow, texts and a picture */
	.balign	8
	.global	kt_bk_fig
	.global	kt_bk_fig_end
kt_bk_fig:
	.incbin	"../etc/xtad/019aaa4e-de25-72ad-8518-ab392b7ea301_0.xtad"
kt_bk_fig_end:

	/* a figure of groups, filled rectangles, boxes of text and a link */
	.balign	8
	.global	kt_bk_box
	.global	kt_bk_box_end
kt_bk_box:
	.incbin	"../etc/xtad/a22ca516-a22c-7fdb-81fd-04cda3e1c8af_0.xtad"
kt_bk_box_end:

	/* a text of sizes, colours and bold */
	.balign	8
	.global	kt_bk_doc
	.global	kt_bk_doc_end
kt_bk_doc:
	.incbin	"../etc/xtad/019aafb3-2536-74e0-8f32-52673733bf21_0.xtad"
kt_bk_doc_end:

	/* a picture of 256 by 256 with an alpha channel, for a record to hold */
	.balign	8
	.global	kt_bk_png
	.global	kt_bk_png_end
kt_bk_png:
	.incbin	"../etc/xtad/019aaa4e-de25-72ad-8518-ab392b7ea301_0_0.png"
kt_bk_png_end:

	.section .note.GNU-stack, "", %progbits
