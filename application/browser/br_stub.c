/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	br_stub.c
 *	ブラウザ, when the system was built without it (design 17.19): the
 *	application is made from Chromium's engine outside the repository, with
 *	make BROWSER=1. This program stands in the program object's record 1
 *	otherwise and only says so, on the message line and the console.
 */

#include <tk/typedef.h>
#include <ts/uapp.h>
#include <ts/wm.h>

int main( void )
{
	(void)wm_msg_put("ブラウザはこのシステムに入っていません(make BROWSER=1 で作ります)");
	(void)tm_putstring((CONST UB *)"browser: not built in this system (make BROWSER=1)\n");
	return 0;
}
