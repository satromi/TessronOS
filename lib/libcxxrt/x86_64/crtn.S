/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	crtn.S (x86-64)
 *	The end of _init and _fini (crti.S)
 */

	.section .init, "ax", @progbits
	pop	%rbp
	ret

	.section .fini, "ax", @progbits
	pop	%rbp
	ret

	.section .note.GNU-stack, "", @progbits
