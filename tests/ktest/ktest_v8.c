/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_v8.c
 *	V8 as a process (design 17.19): tests/uprog/v8prog.cc, on the boot
 *	volume as /boot/V8PROG.ELF when the build was made with BROWSER=1
 *	(tools/browser/build.sh). It starts V8 without a compiler to machine
 *	code, runs a script and compares its answer; its exit code says
 *	whether the answer was right. Every page it used comes back. Skipped
 *	when the program is not on the volume. Skia in a window and the
 *	application ブラウザ are tried after it.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/so.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/wm.h>
#include <ts/disp.h>
#include <ts/hid.h>
#include <ts/part.h>
#include "../../application/desktop/desktop.h"

IMPORT UD knl_pf_free_count( INT zone );

#define PROG		"/boot/V8PROG.ELF"
#define RUN_TMO		600000		/* ms: the interpreter under TCG */

LOCAL BOOL	have_prog = FALSE;

LOCAL void test_present( void )
{
	T_FSTAT	st;

	if ( fs_stat(PROG, &st) < EX_OK ) {
		KT_SKIP("no " PROG " (make BROWSER=1)");
	}
	tm_printf((UB*)"  %s: %d bytes\n", PROG, (INT)st.size);
	have_prog = TRUE;
}

LOCAL void test_script( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UD	before, after;
	ID	pid;
	ER	er;

	if ( !have_prog ) KT_SKIP("no program");

	before = knl_pf_free_count(-1);
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = NULL;
	cprc.argsz  = 0;
	pid = ts_cre_prc(PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return;
	}
	tm_printf((UB*)"  exit code %d\n", psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, 0);
	after = knl_pf_free_count(-1);
	tm_printf((UB*)"  free pages %d -> %d\n", (INT)before, (INT)after);
	KT_ASSERT_EQ(before, after);
}

/*
 * Skia's raster in a window: tests/uprog/skiaprog.cc (/boot/SKIAPROG.ELF)
 * draws, shows and checks its pixels; the screen is taken as a picture
 * while its window is up (/boot/SKIA.PPM).
 */
#define SKPROG		"/boot/SKIAPROG.ELF"
#define SKREADY		"/boot/SKIAPROG.RDY"

typedef struct {
	UW	stay;
	char	ready[60];
} SKARG;

LOCAL void test_skia( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	T_FSTAT	st;
	SKARG	arg;
	INT	i;
	ID	pid;
	ER	er;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("no " SKPROG " (make BROWSER=1)");
	}
	(void)fs_unlink(SKREADY);
	knl_memset(&arg, 0, sizeof(arg));
	arg.stay = 2000;
	knl_strcpy(arg.ready, SKREADY);

	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = &arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SKPROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	for ( i = 0; i < 1200 && fs_stat(SKREADY, &st) < EX_OK; i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(i < 1200);
	if ( i < 1200 && kt_screen() ) {
		(void)kt_shot("/boot/SKIA.PPM");
	}
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return;
	}
	tm_printf((UB*)"  exit code %d\n", psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, 0);
	(void)fs_unlink(SKREADY);
}

/*
 * The application ブラウザ (application/browser), started from its program
 * object on a page object as the desktop starts it. A task of this test
 * serves three pages over HTTP on the loopback interface: "/" has a link
 * to "/go", which sends the browser on to "/next" (a redirect), and
 * "/next" comes in chunks. The page object is copied from the base and
 * set to start at "/"; the browser is told to follow the first link, make
 * a file when the last page is shown and end a little later. The pages
 * asked for must be "/", "/go" and "/next", in that order, a window must
 * say it shows the page object, and the object must then hold "/next"
 * with "/" to go back to, the window's size, and the page in record 0.
 */
#define HTTP_PORT	18080		/* test_browser; test_desk the next */
#define BRREADY		"/boot/BROWSER.RDY"
#define LOOPBACK	0x7f000001UL
#define SEEN_MAX	12
#define BIG_ROWS	3000		/* rows of the large page of test_blink_mem */

LOCAL volatile INT	srv_sock = -1;
LOCAL volatile BOOL	srv_stop;
LOCAL volatile INT	srv_seen;
LOCAL char		srv_path[SEEN_MAX][40];
LOCAL ID		srv_done;

LOCAL CONST char page_top[] =
	"<html><head><title>TessronOS の試験</title></head><body>"
	"<h1>試験の頁</h1><p>日本語の文と <a href=\"/go\">次の頁へ</a> の結び。</p>"
	"<ul><li>一つ目</li><li>二つ目</li></ul><hr><pre>  pre  text</pre></body></html>";
/*
 * The page of test_blink: a style sheet of its own and one it links, a
 * box placed by its style, four images (PNG, JPEG, GIF and WebP, each 16
 * by 16 pixels of one colour, shown 64 by 64), and a script that names
 * the page and adds to it, and a canvas a second script fills with its 2D
 * context. What is drawn and what the page object is given show that the
 * sheet and the images were fetched, decoded and drawn and the scripts
 * run.
 */
LOCAL CONST char page_blink[] =
	"<!DOCTYPE html><html><head><title>before</title>"
	"<link rel=\"stylesheet\" href=\"/b.css\">"
	"<style>#box{position:absolute;left:40px;top:150px;width:200px;height:100px;"
	"background:rgb(0,192,0)}</style></head><body>"
	"<h1>Blink の試験</h1><p id=\"p\">段落の文。</p><div id=\"box\"></div>"
	"<img src=\"/i.png\" width=64 height=64 style=\"position:absolute;left:300px;top:150px\">"
	"<img src=\"/i.jpg\" width=64 height=64 style=\"position:absolute;left:380px;top:150px\">"
	"<img src=\"/i.gif\" width=64 height=64 style=\"position:absolute;left:460px;top:150px\">"
	"<img src=\"/i.webp\" width=64 height=64 style=\"position:absolute;left:540px;top:150px\">"
	"<script>document.cookie = 'j=2'; localStorage.setItem('s', 'L' + localStorage.length);"
	"document.title = 'JS ' + (6 * 7) + ' ' + document.cookie + ' ' + localStorage.getItem('s');"
	"var d = document.createElement('div'); d.textContent = 'スクリプトが作った';"
	"document.body.appendChild(d);</script>"
	"<canvas id=\"c\" width=64 height=64 style=\"position:absolute;left:620px;top:150px\"></canvas>"
	"<script>var g = document.getElementById('c').getContext('2d');"
	"g.fillStyle = 'rgb(250,160,0)'; g.fillRect(0, 0, 64, 64);</script></body></html>";
LOCAL CONST char page_blink_css[] =
	"body { background: rgb(240, 240, 255); } h1 { color: rgb(200, 0, 0); }";

/* The images of the page, made each of one colour */
/* png, 83 bytes */
LOCAL CONST UB img_png[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x91, 0x68,
	0x36, 0x00, 0x00, 0x00, 0x1a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x64, 0x60, 0x38, 0xc1,
	0x40, 0x0a, 0x60, 0x62, 0x20, 0x11, 0x8c, 0x6a, 0x18, 0xd5, 0x30, 0x74, 0x34, 0x00, 0x00, 0x96,
	0x56, 0x00, 0xe8, 0x3d, 0xda, 0x78, 0xd3, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
	0x42, 0x60, 0x82,
};
/* jpg, 635 bytes */
LOCAL CONST UB img_jpg[] = {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
	0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
	0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
	0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
	0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
	0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
	0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
	0x01, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
	0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
	0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
	0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
	0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
	0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
	0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
	0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
	0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
	0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00, 0x03,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
	0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
	0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
	0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
	0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
	0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
	0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
	0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
	0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
	0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xe5,
	0xe8, 0xa2, 0x8a, 0xff, 0x00, 0x3c, 0xcf, 0xee, 0x83, 0xff, 0xd9,
};
/* gif, 68 bytes */
LOCAL CONST UB img_gif[] = {
	0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x10, 0x00, 0x10, 0x00, 0x81, 0x00, 0x00, 0xc8, 0x00, 0xc8,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x10, 0x00, 0x40, 0x08, 0x1d, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13,
	0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x10, 0x23, 0x4a, 0x9c, 0x48, 0xb1, 0xa2, 0xc5, 0x81,
	0x01, 0x01, 0x00, 0x3b,
};
/* webp, 36 bytes */
LOCAL CONST UB img_webp[] = {
	0x52, 0x49, 0x46, 0x46, 0x1c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c,
	0x10, 0x00, 0x00, 0x00, 0x2f, 0x0f, 0xc0, 0x03, 0x00, 0x07, 0x50, 0xd0, 0x28, 0x68, 0xff, 0x03,
	0x11, 0xd1, 0xff, 0x00,
};

LOCAL CONST struct {
	CONST char	*path, *type;
	CONST UB	*data;
	SZ		size;
	UW		rgb;		/* the colour it is made of */
} page_imgs[] = {
	{ "/i.png",  "image/png",  img_png,  sizeof(img_png),  0x0000C8 },
	{ "/i.jpg",  "image/jpeg", img_jpg,  sizeof(img_jpg),  0xC86400 },
	{ "/i.gif",  "image/gif",  img_gif,  sizeof(img_gif),  0xC800C8 },
	{ "/i.webp", "image/webp", img_webp, sizeof(img_webp), 0x00A0A0 },
};
#define N_IMGS	( sizeof(page_imgs) / sizeof(page_imgs[0]) )

/*
 * The pages of test_blink_input: a field in a form that goes to "/r",
 * and "/r", a page taller than the window, red at its top and blue under
 * that
 */
LOCAL CONST char page_form[] =
	"<!DOCTYPE html><html><head><title>form</title></head><body style=\"margin:0\">"
	"<form action=\"/r\"><input id=\"q\" name=\"q\" "
	"style=\"position:absolute;left:40px;top:100px;width:300px;height:30px\"></form></body></html>";
LOCAL CONST char page_tall[] =
	"<!DOCTYPE html><html><head><title>result</title></head><body style=\"margin:0\">"
	"<div style=\"height:400px;background:rgb(200,0,0)\"></div>"
	"<div style=\"height:3000px;background:rgb(0,0,200)\"></div></body></html>";

/*
 * The pages of test_blink_keep: "/k" is given a cookie that lasts an hour,
 * puts a value in localStorage and sends its form with POST to "/p",
 * whose title says what came in the body; "/k2", opened by another run of
 * the browser, puts in its title the cookie and the value it finds
 */
LOCAL CONST char page_keep[] =
	"<!DOCTYPE html><html><head><title>keep</title></head><body>"
	"<form method=\"post\" action=\"/p\"><input name=\"q\" value=\"abc\">"
	"<input name=\"r\" value=\"日本\"></form>"
	"<script>localStorage.setItem('keep', 'yes'); document.forms[0].submit();</script>"
	"</body></html>";
LOCAL CONST char page_keep2[] =
	"<!DOCTYPE html><html><head><title>keep2</title></head><body>"
	"<script>document.title = 'kept ' + document.cookie + ' ' + localStorage.getItem('keep');"
	"</script></body></html>";
LOCAL char	srv_body[128];		/* what the last POST carried */

/*
 * The page of test_blink_media: a WebGL canvas cleared to one colour with
 * a triangle of another drawn over its middle by a pair of shaders, and a
 * sound ("/a.wav", half a second of silence) whose length the script puts
 * in the title once the player has read it, followed by the first byte of
 * crypto.subtle's SHA-256 of "abc" (ba); "c" says that crypto gives a UUID
 * and random values. TextEncoder and TextDecoder carry a character there
 * and back, and an interface that is not built (AudioContext) reads as
 * undefined; " x" is added when either is not so. The video ("/v.webm") names no preload, so it is not
 * fetched unless it plays.
 */
LOCAL CONST char page_media[] =
	"<!DOCTYPE html><html><head><title>media</title></head><body style=\"margin:0\">"
	"<canvas id=\"w\" width=\"64\" height=\"64\" style=\"position:absolute;left:40px;top:100px\"></canvas>"
	"<video src=\"/v.webm\" width=\"64\" height=\"48\" style=\"position:absolute;left:200px;top:100px\"></video>"
	"<script>var gl = document.getElementById('w').getContext('webgl');"
	"var r = gl ? 'gl' : 'nogl';"
	"r += (window.crypto && crypto.randomUUID().length == 36"
	" && crypto.getRandomValues(new Uint8Array(4)).length == 4) ? ' c' : ' noc';"
	"if (new TextDecoder().decode(new TextEncoder().encode('\\u65e5')) != '\\u65e5'"
	" || typeof window.AudioContext != 'undefined') r += ' x';"
	"if (gl) { gl.clearColor(0, 0.5, 1, 1); gl.clear(gl.COLOR_BUFFER_BIT);"
	"var vs = gl.createShader(gl.VERTEX_SHADER);"
	"gl.shaderSource(vs, 'attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }');"
	"gl.compileShader(vs);"
	"var fs = gl.createShader(gl.FRAGMENT_SHADER);"
	"gl.shaderSource(fs, 'precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }');"
	"gl.compileShader(fs);"
	"var pr = gl.createProgram(); gl.attachShader(pr, vs); gl.attachShader(pr, fs); gl.linkProgram(pr);"
	"gl.useProgram(pr); var b = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, b);"
	"gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-0.5, -0.5, 0.5, -0.5, 0, 0.5]), gl.STATIC_DRAW);"
	"var l = gl.getAttribLocation(pr, 'p'); gl.enableVertexAttribArray(l);"
	"gl.vertexAttribPointer(l, 2, gl.FLOAT, false, 0, 0); gl.drawArrays(gl.TRIANGLES, 0, 3); }"
	"document.title = r;"
	"var a = new Audio('/a.wav');"
	"function done(t) { crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc')).then("
	"function (d) { document.title = t + ' ' + new Uint8Array(d)[0].toString(16); },"
	"function () { document.title = t + ' nodigest'; }); }"
	"a.onloadedmetadata = function () { done(r + ' audio ' + Math.round(a.duration * 10)); };"
	"a.onerror = function () { document.title = r + ' audio error'; };"
	"</script></body></html>";
#define WAV_FRAMES	24000		/* half a second at 48 kHz */

LOCAL CONST char page_next[] =
	"<html><head><title>次の頁</title></head><body><h2>たどり着いた</h2>"
	"<p>リンクをたどり、転送を経て、分けて送られた頁を組んだ。</p></body></html>";

LOCAL void srv_put( INT s, CONST char *t )
{
	(void)so_send(s, t, knl_strlen(t), 0);
}

LOCAL void srv_num( char *b, UW v, INT base )
{
	char	t[12];
	INT	n = 0, i = 0;

	do {
		t[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while ( v != 0 );
	while ( n > 0 ) b[i++] = t[--n];
	b[i] = '\0';
}

LOCAL INT find_text( CONST char *s, INT n, CONST char *w );

/* The value of a header of the request, as a number; -1 when it has none */
LOCAL INT srv_hdr_num( CONST char *req, INT n, CONST char *name )
{
	INT	at = find_text(req, n, name), v = 0;

	if ( at < 0 ) return -1;
	for ( at += (INT)knl_strlen(name); at < n && req[at] == ' '; at++ ) ;
	while ( at < n && req[at] >= '0' && req[at] <= '9' ) v = v * 10 + ( req[at++] - '0' );
	return v;
}

/* One request: its path is noted and the page for it sent; a POST's body is kept */
LOCAL void srv_one( INT s )
{
	char	req[2048], path[40], num[16];
	INT	n = 0, k, i, head = -1, off, len;

	while ( n < (INT)sizeof(req) - 1 ) {
		k = so_recv(s, req + n, sizeof(req) - 1 - n, 0);
		if ( k <= 0 ) break;
		n += k;
		req[n] = '\0';
		if ( head < 0 ) {
			for ( i = 3; i < n; i++ ) {
				if ( req[i - 3] == '\r' && req[i - 2] == '\n' && req[i - 1] == '\r' && req[i] == '\n' ) break;
			}
			if ( i < n ) head = i + 1;
		}
		if ( head < 0 ) continue;
		/* the body of a POST, as long as its Content-Length says */
		len = ( req[0] == 'P' ) ? srv_hdr_num(req, head, "Content-Length:") : 0;
		if ( len <= 0 || n - head >= len ) break;
	}
	req[n] = '\0';
	if ( n < 5 || ( req[0] != 'G' && req[0] != 'P' ) ) return;
	off = ( req[0] == 'P' ) ? 5 : 4;
	for ( i = 0; i < (INT)sizeof(path) - 1 && req[off + i] != ' ' && req[off + i] != '\0'; i++ ) {
		path[i] = req[off + i];
	}
	path[i] = '\0';
	if ( srv_seen < SEEN_MAX ) knl_strcpy(srv_path[srv_seen], path);
	srv_seen++;
	if ( req[0] == 'P' && head > 0 ) {
		len = n - head;
		if ( len > (INT)sizeof(srv_body) - 1 ) len = sizeof(srv_body) - 1;
		knl_memcpy(srv_body, req + head, len);
		srv_body[len] = '\0';
	}

	if ( knl_strcmp(path, "/m") == 0 ) {
		srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ");
		srv_num(num, knl_strlen(page_media), 10);
		srv_put(s, num);
		srv_put(s, "\r\nConnection: close\r\n\r\n");
		srv_put(s, page_media);
	} else if ( knl_strcmp(path, "/a.wav") == 0 ) {
		/* 16-bit stereo PCM at 48 kHz: the header, then silence */
		UB	h[44], z[1024];
		UW	data = WAV_FRAMES * 4, v;
		INT	j;

		knl_memcpy(h, "RIFF\0\0\0\0WAVEfmt \20\0\0\0\1\0\2\0\200\273\0\0\0\356\2\0\4\0\20\0data\0\0\0\0", 44);
		v = data + 36;
		for ( j = 0; j < 4; j++ ) h[4 + j] = (UB)( v >> ( 8 * j ) );
		for ( j = 0; j < 4; j++ ) h[40 + j] = (UB)( data >> ( 8 * j ) );
		srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: ");
		srv_num(num, data + 44, 10);
		srv_put(s, num);
		srv_put(s, "\r\nConnection: close\r\n\r\n");
		(void)so_send(s, h, 44, 0);
		knl_memset(z, 0, sizeof(z));
		for ( v = 0; v < data; v += sizeof(z) ) {
			(void)so_send(s, z, ( data - v < sizeof(z) ) ? data - v : sizeof(z), 0);
		}
	} else if ( knl_strcmp(path, "/k") == 0 || knl_strcmp(path, "/k2") == 0 ) {
		CONST char *body = ( path[2] == '\0' ) ? page_keep : page_keep2;

		srv_put(s, ( path[2] == '\0' ) ? "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
					 "Set-Cookie: pk=kept; Max-Age=3600; Path=/\r\n"
					 : "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n");
		srv_put(s, "Content-Length: ");
		srv_num(num, knl_strlen(body), 10);
		srv_put(s, num);
		srv_put(s, "\r\nConnection: close\r\n\r\n");
		srv_put(s, body);
	} else if ( knl_strcmp(path, "/p") == 0 ) {
		/* the title says what the form sent */
		char	page[256];

		tm_sprintf((UB *)page, (UB *)"<html><head><title>posted %s</title></head><body>%s</body></html>",
			   req[0] == 'P' ? srv_body : "(GET)", req[0] == 'P' ? srv_body : "");
		srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ");
		srv_num(num, knl_strlen(page), 10);
		srv_put(s, num);
		srv_put(s, "\r\nConnection: close\r\n\r\n");
		srv_put(s, page);
	} else if ( knl_strcmp(path, "/") == 0 ) {
		srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ");
		srv_num(num, knl_strlen(page_top), 10);
		srv_put(s, num);
		srv_put(s, "\r\nConnection: close\r\n\r\n");
		srv_put(s, page_top);
	} else if ( knl_strcmp(path, "/b") == 0 || knl_strcmp(path, "/b.css") == 0 ) {
		CONST char *body = ( path[2] == '\0' ) ? page_blink : page_blink_css;

		srv_put(s, ( path[2] == '\0' ) ? "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
					 "Set-Cookie: k=v1; Path=/\r\n"
					 : "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\n");
		srv_put(s, "Content-Length: ");
		srv_num(num, knl_strlen(body), 10);
		srv_put(s, num);
		srv_put(s, "\r\nConnection: close\r\n\r\n");
		srv_put(s, body);
	} else if ( path[0] == '/' && path[1] == 'i' && path[2] == '.' ) {
		for ( i = 0; i < (INT)N_IMGS && knl_strcmp(path, page_imgs[i].path) != 0; i++ ) ;
		if ( i < (INT)N_IMGS ) {
			srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: ");
			srv_put(s, page_imgs[i].type);
			srv_put(s, "\r\nContent-Length: ");
			srv_num(num, page_imgs[i].size, 10);
			srv_put(s, num);
			srv_put(s, "\r\nConnection: close\r\n\r\n");
			(void)so_send(s, page_imgs[i].data, page_imgs[i].size, 0);
		} else {
			srv_put(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		}
	} else if ( knl_strcmp(path, "/f") == 0 || ( path[0] == '/' && path[1] == 'r' ) ) {
		CONST char *body = ( path[1] == 'f' ) ? page_form : page_tall;

		srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ");
		srv_num(num, knl_strlen(body), 10);
		srv_put(s, num);
		srv_put(s, "\r\nConnection: close\r\n\r\n");
		srv_put(s, body);
	} else if ( knl_strcmp(path, "/big") == 0 ) {
		/* a large page: rows of text, links and boxes, then a script making objects */
		char	row[256];

		srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
			   "Connection: close\r\n\r\n<!DOCTYPE html><html><head><title>big</title>"
			   "<style>.r{border:1px solid #888;margin:2px;padding:2px}.r b{color:#a00}</style>"
			   "</head><body><h1>大きな頁</h1>");
		for ( i = 0; i < BIG_ROWS; i++ ) {
			tm_sprintf((UB *)row, (UB *)"<div class=\"r\" id=\"r%d\"><b>行 %d</b> 日本語の文と "
				   "<a href=\"#r%d\">次の行へのリンク</a> とその続きの文。</div>\n", i, i, i + 1);
			srv_put(s, row);
		}
		srv_put(s, "<script>var a = []; for (var i = 0; i < 100000; i++) a.push({n: i, s: 'x' + i});"
			   "document.title = 'big ' + a.length + ' ' + document.querySelectorAll('div').length;"
			   "</script></body></html>");
	} else if ( knl_strcmp(path, "/go") == 0 ) {
		srv_put(s, "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	} else if ( knl_strcmp(path, "/next") == 0 ) {
		SZ	len = knl_strlen(page_next), half = len / 2;

		srv_put(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
			   "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
		srv_num(num, half, 16);
		srv_put(s, num);
		srv_put(s, "\r\n");
		(void)so_send(s, page_next, half, 0);
		srv_put(s, "\r\n");
		srv_num(num, len - half, 16);
		srv_put(s, num);
		srv_put(s, "\r\n");
		(void)so_send(s, page_next + half, len - half, 0);
		srv_put(s, "\r\n0\r\n\r\n");
	} else {
		srv_put(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	}
}

LOCAL void srv_task( INT stacd, void *exinf )
{
	INT	s;

	(void)stacd;
	(void)exinf;
	while ( !srv_stop ) {
		s = so_accept(srv_sock, NULL, NULL);
		if ( s < 0 ) break;
		if ( !srv_stop ) srv_one(s);
		(void)so_close(s);
	}
	(void)tk_sig_sem(srv_done, 1);
	tk_exd_tsk();
}

#define BRBASE		"01a0d8c4-5b22-7b13-9d54-8a6f7e8c9d01"	/* the base in the template box */

LOCAL char	brmeta[OB_ATR_MAX + 1];
LOCAL ID	mem_pid;		/* the browser run_browser started */
LOCAL CONST char *shot_later;	/* a second screen, 3 s after the first */
LOCAL char	brarg[256];

LOCAL INT find_text( CONST char *s, INT n, CONST char *w )
{
	INT	i, j, k = (INT)knl_strlen(w);

	for ( i = 0; i + k <= n; i++ ) {
		for ( j = 0; j < k && s[i + j] == w[j]; j++ ) ;
		if ( j == k ) return i;
	}
	return -1;
}

/* An object's metadata into brmeta; its length */
LOCAL INT read_meta( CONST TS_UUID *u )
{
	SZ	asz = 0;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	brmeta[0] = '\0';
	if ( k <= 0 ) return 0;
	if ( ob_get_atr(k, (UB *)brmeta, OB_ATR_MAX, &asz) < E_OK ) asz = 0;
	ob_cls_obj(k);
	brmeta[asz] = '\0';
	return (INT)asz;
}

/* The window that says it shows u */
LOCAL BOOL window_of( CONST TS_UUID *u, TS_UUID *win )
{
	TS_UUID	list[64];
	char	us[40], want[64];
	UB	j[512];
	INT	cnt = 0, i;
	SZ	asz = 0;
	BOOL	yes = FALSE;
	ID	kw;

	ts_uuid_to_str(u, us, sizeof(us));
	tm_sprintf((UB *)want, (UB *)"\"shows\":\"%s\"", us);
	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) < E_OK ) return FALSE;
	for ( i = 0; i < cnt && i < 64 && !yes; i++ ) {
		kw = ob_opn_obj(&list[i], OB_OP_ATRRD);
		if ( kw <= 0 ) continue;
		if ( ob_get_atr(kw, j, sizeof(j) - 1, &asz) >= E_OK && find_text((char *)j, (INT)asz, want) >= 0 ) {
			*win = list[i];
			yes = TRUE;
		}
		ob_cls_obj(kw);
	}
	return yes;
}

/*
 * ブラウザ started from its program object on the system volume, as the
 * desktop starts it on a page object ('page', or none), with lines for
 * the test after it; its exit code, or -1 when it did not end. The screen
 * is taken to 'shot' once it says the last page is shown, and a window
 * must then say it shows the page object.
 */
LOCAL INT run_browser( CONST TS_UUID *page, CONST char *lines, CONST char *shot,
			void (*check)( CONST TS_UUID *page ) )
{
	T_OBCRE	c;
	T_PSTS	psts;
	T_FSTAT	st;
	TS_UUID	prog, pu;
	INT	i, n = 0;
	ID	pid = -1;
	ER	er;

	(void)fs_unlink(BRREADY);
	if ( page != NULL ) {
		knl_memcpy(brarg, page, sizeof(*page));
		n = sizeof(*page);
		brarg[n++] = '\n';
	}
	knl_strcpy(brarg + n, lines);
	n += (INT)knl_strlen(lines) + 1;
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_BROWSER, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = brarg;
	c.argsz = n;
	er = ob_cre_obj(&c, &pu);
	KT_ASSERT_ER(er, E_OK);
	if ( er >= E_OK ) pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return -1;
	mem_pid = pid;

	for ( i = 0; i < 1200 && fs_stat(BRREADY, &st) < EX_OK; i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(i < 1200);
	if ( i < 1200 && page != NULL ) {
		TS_UUID	w;

		KT_ASSERT(window_of(page, &w));
	}
	if ( i < 1200 && ( shot != NULL || check != NULL ) ) {
		tk_dly_tsk(1000);		/* the window manager's own drawing done */
	}
	if ( i < 1200 && check != NULL ) check(page);
	if ( i < 1200 && shot != NULL && kt_screen() ) {
		(void)kt_shot(shot);
		if ( shot_later != NULL ) {
			tk_dly_tsk(3000);
			(void)kt_shot(shot_later);
		}
	}
	(void)fs_unlink(BRREADY);
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return -1;
	}
	tm_printf((UB*)"  exit code %d\n", psts.exitcd);
	return psts.exitcd;
}

LOCAL struct sockaddr_in	srv_me;

/*
 * The test's HTTP server on the loopback interface, in a task of its own.
 * Each test takes a port of its own: the one before may still be held
 * by its closed connections.
 */
LOCAL BOOL srv_start( UH port )
{
	T_CTSK	ctsk;
	T_CSEM	csem;
	ID	tid;
	ER	er;

	knl_memset(&srv_me, 0, sizeof(srv_me));
	srv_me.sin_family = AF_INET;
	srv_me.sin_port = lwip_htons(port);
	srv_me.sin_addr.s_addr = lwip_htonl(LOOPBACK);
	srv_sock = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( srv_sock < 0 ) return FALSE;
	er = so_bind(srv_sock, (struct sockaddr *)&srv_me, sizeof(srv_me));
	KT_ASSERT_ER(er, E_OK);
	if ( er >= E_OK ) er = so_listen(srv_sock, 4);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)so_close(srv_sock);
		srv_sock = -1;
		return FALSE;
	}
	srv_stop = FALSE;
	srv_seen = 0;
	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem = 1;
	srv_done = tk_cre_sem(&csem);
	KT_ASSERT(srv_done > 0);
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)srv_task;
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = 8192;
	ctsk.assprc  = 0;
	tid = tk_cre_tsk(&ctsk);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 || srv_done <= 0 ) {
		(void)so_close(srv_sock);
		srv_sock = -1;
		return FALSE;
	}
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
	return TRUE;
}

/* The server let go by one more connection of its own; the pages asked for shown */
LOCAL void srv_end( void )
{
	INT	cl, i;

	srv_stop = TRUE;
	cl = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( cl >= 0 ) {
		(void)so_connect(cl, (struct sockaddr *)&srv_me, sizeof(srv_me));
		(void)so_close(cl);
	}
	KT_ASSERT_ER(tk_wai_sem(srv_done, 1, 10000), E_OK);
	(void)so_close(srv_sock);
	srv_sock = -1;
	(void)tk_del_sem(srv_done);
	tm_printf((UB*)"  pages asked for: %d", srv_seen);
	for ( i = 0; i < srv_seen && i < SEEN_MAX; i++ ) {
		tm_printf((UB*)" %s", srv_path[i]);
	}
	tm_printf((UB*)"\n");
}

/* A page object made from the base, set to start at 'url' */
LOCAL BOOL make_page( CONST char *url, TS_UUID *page )
{
	TS_UUID	base;
	char	to[80];
	CONST char *from = "\"https://www.tron.org\"";
	INT	i, n, at, d;
	ID	k;
	ER	er;

	KT_ASSERT_ER(ts_str_to_uuid(BRBASE, &base), E_OK);
	er = ob_cpy_obj(&base, NULL, page);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) return FALSE;
	tm_sprintf((UB *)to, (UB *)"\"%s\"", url);
	n = read_meta(page);
	at = find_text(brmeta, n, from);
	KT_ASSERT(at > 0 && n + 80 < OB_ATR_MAX);
	if ( at <= 0 ) {
		(void)ob_del_obj(page);
		return FALSE;
	}
	/* the address in place of the base's, the rest moved along */
	d = (INT)( knl_strlen(to) - knl_strlen(from) );
	for ( i = n; i >= at + (INT)knl_strlen(from); i-- ) brmeta[i + d] = brmeta[i];
	knl_memcpy(brmeta + at, to, knl_strlen(to));
	n += d;
	k = ob_opn_obj(page, OB_OP_ATRRD | OB_OP_ATRWR);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_set_atr(k, (UB *)brmeta, n), E_OK);
		ob_cls_obj(k);
	}
	return TRUE;
}

LOCAL void test_browser( void )
{
	T_FSTAT	st;
	TS_UUID	page;
	INT	n, at;
	ID	k;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18080/", &page) ) return;
	if ( !srv_start(HTTP_PORT) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}

	KT_ASSERT_EQ(run_browser(&page, "follow\nready=" BRREADY "\nquit=3000\n", "/boot/BROWSER.PPM", NULL), 0);

	srv_end();
	KT_ASSERT_EQ(srv_seen, 3);
	KT_ASSERT(knl_strcmp(srv_path[0], "/") == 0);
	KT_ASSERT(knl_strcmp(srv_path[1], "/go") == 0);
	KT_ASSERT(knl_strcmp(srv_path[2], "/next") == 0);

	/* the page object holds where it went, the way back and the window */
	n = read_meta(&page);
	KT_ASSERT(find_text(brmeta, n, "\"url\": \"http://127.0.0.1:18080/next\"") > 0);
	KT_ASSERT(find_text(brmeta, n, "\"title\": \"次の頁\"") > 0);
	at = find_text(brmeta, n, "\"back\": [");
	KT_ASSERT(at > 0 && find_text(brmeta + at, n - at, "\"http://127.0.0.1:18080/\"") > 0);
	KT_ASSERT(find_text(brmeta, n, "\"width\": 800") > 0);
	KT_ASSERT(find_text(brmeta, n, "\"height\": 560") > 0);
	k = ob_opn_obj(&page, OB_OP_READ);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		SZ	asz = 0;

		brmeta[0] = '\0';
		if ( ob_rea_rec(k, 0, 0, brmeta, 1024, &asz) >= E_OK ) brmeta[asz] = '\0';
		ob_cls_obj(k);
		KT_ASSERT(find_text(brmeta, (INT)asz, "<p>次の頁</p>") > 0);
		KT_ASSERT(find_text(brmeta, (INT)asz, "<p>http://127.0.0.1:18080/next</p>") > 0);
	}
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}

/*
 * The same opened as a user opens it: the desktop is asked to open the
 * page object as a double click does (its request port, 16.5.20), finds
 * ブラウザ in the object's applist and starts it on the object. The page
 * the object holds is read (its title comes back into the object). A
 * second request to open it starts one more, which finds the window
 * showing the object and ends at once, the first staying; deleting the
 * window ends the first.
 *
 * While the browser is read in and started, the desktop goes on: the
 * pointer is moved to and fro while the answer is waited for, and each
 * move has to be on the screen (wm_pointer_last) soon after it was
 * made. The longest wait for one is given back in 'p_lag', in ms.
 */
#define LAG_X		700
#define LAG_Y		500
#define LAG_STEP	24

LOCAL UW lag_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

LOCAL void lag_move( INT x, INT y )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = HID_EV_MOVE;
	e.x = x;
	e.y = y;
	(void)kt_inject(&e);
}

LOCAL ER ask_open( CONST TS_UUID *u, T_DTANS *an, UW *p_lag )
{
	TS_UUID	d, reply;
	T_DTREQ	rq;
	T_OBCRE	c;
	T_DPRECT to;
	SZ	asz = 0;
	ID	k, ka;
	INT	i, x = LAG_X, lx, ly, side;
	UW	sent = 0, worst = 0, waited;
	BOOL	moving = FALSE;
	ER	er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	knl_memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.seq = 19;
	rq.target = *u;
	rq.recno = -1;
	rq.reply = reply;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, &rq, sizeof(rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	for ( i = 0; er >= E_OK && i < 250; i++ ) {
		if ( ob_rea_rec(ka, 0, 0, an, sizeof(*an), &asz) >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq.seq ) {
			er = an->er;
			break;
		}
		if ( p_lag != NULL ) {
			if ( moving ) {
				waited = lag_now() - sent;	/* shown or not, at least this long */
				if ( waited > worst ) worst = waited;
				if ( wm_pointer_last(&lx, &ly, &side) && lx == to.left && ly == to.top ) {
					moving = FALSE;
				}
			}
			if ( !moving ) {
				x = ( x == LAG_X ) ? LAG_X + LAG_STEP : LAG_X;
				wm_pointer_box(x, LAG_Y, &to);
				lag_move(x, LAG_Y);
				sent = lag_now();
				moving = TRUE;
			}
		}
		tk_dly_tsk(20);
	}
	if ( i >= 250 ) er = E_TMOUT;
	if ( ka > 0 ) ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	if ( p_lag != NULL ) *p_lag = worst;
	return er;
}

LOCAL void test_desk( void )
{
	T_FSTAT	st;
	T_DTANS	an;
	T_PSTS	psts;
	TS_UUID	page, win, d;
	T_RPRC	rp;
	BOOL	got = FALSE;
	INT	i, n;
	ID	pid = -1, pid2;
	UW	lag = 0;
	ER	er;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18081/next", &page) ) return;
	if ( !srv_start(HTTP_PORT + 1) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}
	KT_ASSERT_ER(dt_start(), E_OK);
	for ( i = 0; i < 100 && ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK; i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(i < 100);
	tk_dly_tsk(1000);			/* the first cabinet opened, and drawn */

	knl_memset(&an, 0, sizeof(an));
	wm_show_pointer(TRUE);			/* the tests keep it off; this one watches it */
	er = ask_open(&page, &an, &lag);
	wm_show_pointer(FALSE);
	KT_ASSERT_ER(er, E_OK);
	/* the pointer went on following while the browser was read in */
	tm_printf((UB *)"  the pointer followed within %d ms while the browser started\n", (INT)lag);
	KT_ASSERT(lag < 1000);
	if ( er >= E_OK ) pid = knl_prc_of_uuid(&an.proc);
	KT_ASSERT(pid > 0);
	for ( i = 0; pid > 0 && i < 600 && !got; i++ ) {
		n = read_meta(&page);
		got = ( find_text(brmeta, n, "\"title\": \"次の頁\"") > 0 );
		if ( !got ) tk_dly_tsk(100);
	}
	KT_ASSERT(got);
	KT_ASSERT(window_of(&page, &win));
	if ( got && kt_screen() ) {
		tk_dly_tsk(1000);
		(void)kt_shot("/boot/BRDESK.PPM");
	}

	/* open again: the second sees the window there is, and goes; the first stays */
	knl_memset(&an, 0, sizeof(an));
	KT_ASSERT_ER(ask_open(&page, &an, NULL), E_OK);
	pid2 = knl_prc_of_uuid(&an.proc);
	if ( pid2 > 0 && pid2 != pid ) {
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(pid2, &psts, 60000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, 0);
	}
	KT_ASSERT(pid > 0 && ts_ref_prc(pid, &rp) >= E_OK);

	if ( pid > 0 && window_of(&page, &win) ) {
		KT_ASSERT_ER(ob_del_obj(&win), E_OK);
		psts.exitcd = -1;
		er = ts_wai_prc(pid, &psts, 10000);
		KT_ASSERT_ER(er, E_OK);
		if ( er < E_OK ) {
			(void)ts_ter_prc(pid, TS_ABORT_TERM);
			(void)ts_wai_prc(pid, &psts, 5000);
		}
	}
	dt_quit();
	wm_update();
	srv_end();
	KT_ASSERT(srv_seen >= 1 && knl_strcmp(srv_path[0], "/next") == 0);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}

/*
 * HTTPS to the internet, when the machine is on QEMU's user mode network
 * (NET=user): a page over TLS comes and is shown (exit code 0), and one
 * whose certificate has run out is refused (4, the page did not come).
 */
#ifdef KT_NET_USER
#define DHCP_WAIT_S	15

LOCAL void test_https( void )
{
	T_FSTAT	st;
	UW	addr = 0, mask = 0, gw = 0;
	INT	i;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( so_getifaddr(&addr, &mask, &gw) < E_OK ) {
		KT_SKIP("no network stack");
	}
	if ( addr == 0 || ( lwip_ntohl(addr) >> 24 ) != 10 ) {
		KT_ASSERT(so_dhcp_start() >= E_OK);
		for ( i = 0; i < DHCP_WAIT_S * 10; i++ ) {
			if ( so_getifaddr(&addr, &mask, &gw) >= E_OK && ( lwip_ntohl(addr) >> 24 ) == 10 ) break;
			tk_dly_tsk(100);
		}
	}
	KT_ASSERT(( lwip_ntohl(addr) >> 24 ) == 10);
	KT_ASSERT_EQ(run_browser(NULL, "https://example.com/\nready=" BRREADY "\nquit=3000\n",
				 "/boot/HTTPS.PPM", NULL), 0);
	KT_ASSERT_EQ(run_browser(NULL, "https://expired.badssl.com/\nready=" BRREADY "\nquit=500\n",
				 NULL, NULL), 4);
#ifdef KT_BROWSER_BLINK
	{
		/*
		 * Ordinary sites with images, style sheets, scripts and forms:
		 * each must come (exit code 0) and is taken to the screen
		 */
		static CONST char *CONST sites[][2] = {
			{ "https://www.wikipedia.org/", "/boot/SITE1.PPM" },
			{ "https://ja.wikipedia.org/wiki/BTRON", "/boot/SITE2.PPM" },
			{ "https://news.ycombinator.com/", "/boot/SITE3.PPM" },
			{ "https://html.duckduckgo.com/html/?q=TRON", "/boot/SITE4.PPM" },
			{ "https://www.gnu.org/", "/boot/SITE5.PPM" },
		};
		char	lines[160];
		INT	s;

		for ( s = 0; s < (INT)( sizeof(sites) / sizeof(sites[0]) ); s++ ) {
			tm_sprintf((UB *)lines, (UB *)"%s\nready=" BRREADY "\nquit=8000\n", sites[s][0]);
			tm_printf((UB*)"  %s\n", sites[s][0]);
			KT_ASSERT_EQ(run_browser(NULL, lines, sites[s][1], NULL), 0);
		}
	}
#endif
}
#ifdef KT_SITE
/* One site on its own (make ... NET=user KT_SITE=<URL>): it must come, and the screen is taken */
LOCAL void test_site( void )
{
	T_FSTAT	st;
	UW	addr = 0, mask = 0, gw = 0;
	INT	i;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( so_getifaddr(&addr, &mask, &gw) < E_OK ) {
		KT_SKIP("no network stack");
	}
	if ( addr == 0 || ( lwip_ntohl(addr) >> 24 ) != 10 ) {
		KT_ASSERT(so_dhcp_start() >= E_OK);
		for ( i = 0; i < DHCP_WAIT_S * 10; i++ ) {
			if ( so_getifaddr(&addr, &mask, &gw) >= E_OK && ( lwip_ntohl(addr) >> 24 ) == 10 ) break;
			tk_dly_tsk(100);
		}
	}
	tm_printf((UB*)"  %s\n", KT_SITE);
	shot_later = "/boot/SITE2.PPM";
	KT_ASSERT_EQ(run_browser(NULL, KT_SITE "\nready=" BRREADY "\nquit=10000\n", "/boot/SITE.PPM", NULL), 0);
	shot_later = NULL;
}
#endif
#else
LOCAL void test_https( void )
{
	KT_SKIP("not on the user mode network (NET=user)");
}
#endif

/*
 * ブラウザ with Blink (make BROWSER=1 BROWSER_BLINK=1): the page "/b" of
 * the test's server is laid out and painted by Blink, and runs. Blink
 * must fetch the style sheet the page links (the server sees "/b" and
 * then "/b.css"); the box the page's own style places must be on the
 * screen in its colour where the page puts it, and the background the
 * linked sheet gives round it; the title the page object is given must
 * be the one the page's script set: "JS 42", then document.cookie with
 * the cookie the server set with the page and the one the script set
 * ("k=v1; j=2"), then what the script put in localStorage ("L0").
 */
#ifdef KT_BROWSER_BLINK
#define BAR_H		0		/* the page fills the work area; the address is on the tool panel (br_main.cc) */

LOCAL INT	blink_box = -1, blink_bg = -1;	/* pixels found: 1 right, 0 wrong */
LOCAL INT	blink_img[N_IMGS];		/* the images' pixels: 1 right, 0 wrong */
LOCAL INT	blink_canvas = -1;		/* the canvas's */

/* Whether two colours differ by no more than 'tol' in each component */
LOCAL BOOL near_rgb( UW a, UW b, INT tol )
{
	INT	i, d;

	for ( i = 0; i < 24; i += 8 ) {
		d = (INT)( ( a >> i ) & 0xFF ) - (INT)( ( b >> i ) & 0xFF );
		if ( d > tol || d < -tol ) return FALSE;
	}
	return TRUE;
}

LOCAL UW screen_px( INT x, INT y )
{
	T_DISPSPEC	spec;
	UB		*fb;

	if ( ts_disp_ref(&spec) < E_OK || ( fb = (UB *)ts_disp_buffer() ) == NULL ) return 0xFFFFFFFF;
	if ( x < 0 || y < 0 || x >= (INT)spec.width || y >= (INT)spec.height ) return 0xFFFFFFFF;
	return ((UW *)( fb + (UBINT)y * spec.pitch ))[x] & 0x00FFFFFF;
}

LOCAL void blink_check( CONST TS_UUID *page )
{
	TS_UUID		win;
	T_OBWPOS	wp;
	SZ		asz = 0;
	UW		box, bg, px;
	ID		k;
	INT		i;

	for ( i = 0; i < (INT)N_IMGS; i++ ) blink_img[i] = -1;
	blink_canvas = -1;
	if ( !window_of(page, &win) ) return;
	k = ob_opn_obj(&win, OB_OP_READ);
	if ( k <= 0 ) return;
	knl_memset(&wp, 0, sizeof(wp));
	(void)ob_rea_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	ob_cls_obj(k);
	box = screen_px(wp.wleft + 40 + 100, wp.wtop + BAR_H + 150 + 50);
	bg = screen_px(wp.wleft + 400, wp.wtop + BAR_H + 300);
	tm_printf((UB*)"  work area at %d,%d: box 0x%06x, background 0x%06x\n",
		  wp.wleft, wp.wtop, box, bg);
	blink_box = ( box == 0x00C000 );
	blink_bg = ( bg == 0xF0F0FF );
	for ( i = 0; i < (INT)N_IMGS; i++ ) {
		/* the middle of the image, 64 by 64 from (300 + 80 i, 150); JPEG within 8 */
		px = screen_px(wp.wleft + 300 + 80 * i + 32, wp.wtop + BAR_H + 150 + 32);
		tm_printf((UB*)"  image %s: 0x%06x (made of 0x%06x)\n", page_imgs[i].path, px, page_imgs[i].rgb);
		blink_img[i] = near_rgb(px, page_imgs[i].rgb, 8) ? 1 : 0;
	}
	/* what the page's script drew on its canvas (64 by 64 at (620, 150)) */
	px = screen_px(wp.wleft + 620 + 32, wp.wtop + BAR_H + 150 + 32);
	tm_printf((UB*)"  canvas: 0x%06x (drawn 0xfaa000)\n", px);
	blink_canvas = near_rgb(px, 0xFAA000, 4) ? 1 : 0;
}

LOCAL void test_blink( void )
{
	T_FSTAT	st;
	TS_UUID	page;
	INT	n, i;
	BOOL	css = FALSE;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18082/b", &page) ) return;
	if ( !srv_start(HTTP_PORT + 2) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}
	KT_ASSERT_EQ(run_browser(&page, "ready=" BRREADY "\nquit=8000\n", "/boot/BLINK.PPM",
				 blink_check), 0);
	srv_end();
	KT_ASSERT(srv_seen >= 2 && knl_strcmp(srv_path[0], "/b") == 0);
	for ( i = 1; i < srv_seen && i < SEEN_MAX; i++ ) {
		if ( knl_strcmp(srv_path[i], "/b.css") == 0 ) css = TRUE;
	}
	KT_ASSERT(css);
	KT_ASSERT_EQ(blink_box, 1);
	KT_ASSERT_EQ(blink_bg, 1);
	for ( i = 0; i < (INT)N_IMGS; i++ ) {
		KT_ASSERT_EQ(blink_img[i], 1);
	}
	KT_ASSERT_EQ(blink_canvas, 1);
	n = read_meta(&page);
	KT_ASSERT(find_text(brmeta, n, "\"title\": \"JS 42 k=v1; j=2 L0\"") > 0);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}

/*
 * Input given to the page (BROWSER_BLINK=1), as the keyboard and the
 * pointer give it (kt_inject, through 入力): a press in the field of "/f" gives it
 * the focus, "abc" typed and Enter send the form ("/r?q=abc"); the wheel
 * turned over "/r" scrolls it inside Blink (where the view showed its red
 * top, it shows the blue under it); the window's own bar down the right
 * side says the page is taller than the view and scrolled, and a press
 * on its track above the knob brings the red top back (the desktop tells
 * the browser with OB_E_SCROLL); back on "/f", in 日本語 (romaji) "ai"
 * typed goes through the kana-kanji converter, Enter commits "あい" into
 * the field and a second Enter sends it ("/r?q=%E3%81%82%E3%81%84").
 * Deleting the window then ends the browser.
 */
LOCAL INT	in_red = -1, in_blue = -1, in_bar = -1, in_bar_up = -1;

LOCAL void hid_ev( UINT type, UINT code, INT x, INT y, UINT mods, INT dz )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.x = x;
	e.y = y;
	e.mods = mods;
	e.dz = dz;
	(void)kt_inject(&e);
	tk_dly_tsk(80);
}

LOCAL void type_key( UINT code, UINT mods )
{
	hid_ev(HID_EV_KEY_DOWN, code, 0, 0, mods, 0);
	hid_ev(HID_EV_KEY_UP, code, 0, 0, mods, 0);
}

/* Whether the server was asked for 'p' within 'ms' */
LOCAL BOOL asked_for( CONST char *p, INT ms )
{
	INT	i, t;

	for ( t = 0; t < ms; t += 100 ) {
		for ( i = 0; i < srv_seen && i < SEEN_MAX; i++ ) {
			if ( knl_strcmp(srv_path[i], p) == 0 ) return TRUE;
		}
		tk_dly_tsk(100);
	}
	return FALSE;
}

LOCAL BOOL	in_abc, in_kana;

LOCAL void input_check( CONST TS_UUID *page )
{
	TS_UUID		win;
	T_OBWPOS	wp;
	SZ		asz = 0;
	ID		k;
	INT		fx, fy, px, py;
	UW		c;

	if ( !window_of(page, &win) ) return;
	k = ob_opn_obj(&win, OB_OP_READ);
	if ( k <= 0 ) return;
	knl_memset(&wp, 0, sizeof(wp));
	(void)ob_rea_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	ob_cls_obj(k);
	fx = wp.wleft + 100;
	fy = wp.wtop + BAR_H + 115;
	px = wp.wleft + 400;
	py = wp.wtop + BAR_H + 150;

	/* the field focused, "abc" and Enter */
	hid_ev(HID_EV_MOVE, 0, fx, fy, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, fx, fy, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, fx, fy, 0, 0);
	tk_dly_tsk(300);
	type_key(0x04, 0);
	type_key(0x05, 0);
	type_key(0x06, 0);
	type_key(0x28, 0);
	in_abc = asked_for("/r?q=abc", 8000);
	tm_printf((UB*)"  typed and sent: %s\n", in_abc ? "/r?q=abc" : "(nothing)");

	/* the wheel over the page */
	tk_dly_tsk(1500);
	c = screen_px(px, py);
	in_red = near_rgb(c, 0xC80000, 8) ? 1 : 0;
	hid_ev(HID_EV_MOVE, 0, px, py, 0, 0);
	hid_ev(HID_EV_WHEEL, HID_WHEEL_V, px, py, 0, -3);
	tk_dly_tsk(1500);
	tm_printf((UB*)"  before the wheel 0x%06x, after 0x%06x\n", c, screen_px(px, py));
	in_blue = near_rgb(screen_px(px, py), 0x0000C8, 8) ? 1 : 0;

	/* the window's bar: where the view is, and the track above the knob pressed */
	{
		T_OBWBARS	bs;
		INT		bx = wp.wright + 4, by = wp.wtop + 4;

		knl_memset(&bs, 0, sizeof(bs));
		k = ob_opn_obj(&win, OB_OP_READ);
		if ( k > 0 ) {
			(void)ob_rea_rec(k, OB_WR_BARS, 0, &bs, sizeof(bs), &asz);
			ob_cls_obj(k);
		}
		tm_printf((UB*)"  the bar: %d..%d of %d..%d\n", bs.bar[OB_BAR_R].clo, bs.bar[OB_BAR_R].chi,
			  bs.bar[OB_BAR_R].lo, bs.bar[OB_BAR_R].hi);
		in_bar = ( bs.bar[OB_BAR_R].hi > bs.bar[OB_BAR_R].chi && bs.bar[OB_BAR_R].clo > 0 ) ? 1 : 0;
		/* halfway between the track's top and the knob's */
		if ( bs.bar[OB_BAR_R].hi > 0 ) by = wp.wtop + ( wp.wbottom - wp.wtop ) * bs.bar[OB_BAR_R].clo / bs.bar[OB_BAR_R].hi / 2;
		hid_ev(HID_EV_MOVE, 0, bx, by, 0, 0);
		hid_ev(HID_EV_BTN_DOWN, 0, bx, by, 0, 0);
		hid_ev(HID_EV_BTN_UP, 0, bx, by, 0, 0);
		tk_dly_tsk(1500);
		knl_memset(&bs, 0, sizeof(bs));
		k = ob_opn_obj(&win, OB_OP_READ);
		if ( k > 0 ) {
			(void)ob_rea_rec(k, OB_WR_BARS, 0, &bs, sizeof(bs), &asz);
			ob_cls_obj(k);
		}
		tm_printf((UB*)"  after the track pressed: %d..%d, 0x%06x\n", bs.bar[OB_BAR_R].clo,
			  bs.bar[OB_BAR_R].chi, screen_px(px, py));
		in_bar_up = ( bs.bar[OB_BAR_R].clo == 0
			      && near_rgb(screen_px(px, py), 0xC80000, 8) ) ? 1 : 0;
	}

	/* back to the form; 日本語 in romaji */
	type_key(0x50, HID_MOD_LALT);
	tk_dly_tsk(2000);
	hid_ev(HID_EV_MOVE, 0, fx, fy, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, fx, fy, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, fx, fy, 0, 0);
	tk_dly_tsk(300);
	(void)wm_msg_mode(WM_MODE_ROMAN);
	type_key(0x04, 0);
	type_key(0x0C, 0);
	type_key(0x28, 0);
	tk_dly_tsk(300);
	type_key(0x28, 0);
	in_kana = asked_for("/r?q=%E3%81%82%E3%81%84", 8000);
	(void)wm_msg_mode(WM_MODE_ALPH | WM_MODE_ROMAN);
	tm_printf((UB*)"  converted and sent: %s\n", in_kana ? "/r?q=%E3%81%82%E3%81%84" : "(nothing)");
	(void)ob_del_obj(&win);
}

LOCAL void test_blink_input( void )
{
	T_FSTAT	st;
	TS_UUID	page, d;
	INT	i;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18083/f", &page) ) return;
	if ( !srv_start(HTTP_PORT + 3) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}
	/* the desktop takes the input and gives it to the window in front */
	KT_ASSERT_ER(dt_start(), E_OK);
	for ( i = 0; i < 100 && ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK; i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(i < 100);
	tk_dly_tsk(1000);
	in_abc = in_kana = FALSE;
	in_red = in_blue = in_bar = in_bar_up = -1;
	KT_ASSERT_EQ(run_browser(&page, "trace\nready=" BRREADY "\nquit=60000\n", "/boot/BINPUT.PPM",
				 input_check), 0);
	dt_quit();
	wm_update();
	srv_end();
	KT_ASSERT(in_abc);
	KT_ASSERT_EQ(in_red, 1);
	KT_ASSERT_EQ(in_blue, 1);
	KT_ASSERT_EQ(in_bar, 1);
	KT_ASSERT_EQ(in_bar_up, 1);
	KT_ASSERT(in_kana);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}

/*
 * What the engine holds in memory with a large page (BROWSER_BLINK=1):
 * "/big" has 3000 rows of text, links and boxes (about 400 KB) and a
 * script that makes 100,000 objects. The browser prints what its heaps
 * hold before and after collecting its garbage ("mem"); the process's
 * mapped memory is printed while it shows the page. The page must come
 * and its script run ("big 100000 3000").
 */
LOCAL void mem_check( CONST TS_UUID *page )
{
	T_RPRC	rp;
	ID	pid;
	TS_UUID	win;

	(void)page;
	pid = mem_pid;
	if ( pid > 0 && ts_ref_prc(pid, &rp) >= E_OK ) {
		tm_printf((UB*)"  the process maps %d KB\n", (INT)( rp.memsz / 1024 ));
	}
	if ( window_of(page, &win) ) (void)ob_del_obj(&win);
}

LOCAL void test_blink_mem( void )
{
	T_FSTAT	st;
	TS_UUID	page;
	INT	n;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18084/big", &page) ) return;
	if ( !srv_start(HTTP_PORT + 4) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}
	KT_ASSERT_EQ(run_browser(&page, "mem\nready=" BRREADY "\nquit=60000\n", NULL, mem_check), 0);
	srv_end();
	n = read_meta(&page);
	KT_ASSERT(find_text(brmeta, n, "\"title\": \"big 100000 3000\"") > 0);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}

/*
 * A form sent with POST, and what outlives the browser (BROWSER_BLINK=1).
 * "/k" is given a cookie that lasts an hour, puts "yes" in localStorage
 * and sends its form with POST: the server must be asked for "/p" with
 * the form's fields in the body (the Japanese one encoded), and the page
 * object's title is then the page "/p" made of it. A second run of the
 * browser opens "/k2", whose script finds the cookie and the value the
 * first run left in their objects (the learning box's ブラウザのCookie and
 * ブラウザの保存域) and puts them in its title.
 */
LOCAL void test_blink_keep( void )
{
	T_FSTAT	st;
	TS_UUID	page, page2;
	INT	n, i;
	BOOL	posted = FALSE;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18085/k", &page) ) return;
	if ( !make_page("http://127.0.0.1:18085/k2", &page2) ) {
		(void)ob_del_obj(&page);
		return;
	}
	if ( !srv_start(HTTP_PORT + 5) ) {
		(void)ob_del_obj(&page);
		(void)ob_del_obj(&page2);
		KT_SKIP("no network stack");
	}
	srv_body[0] = '\0';
	KT_ASSERT_EQ(run_browser(&page, "ready=" BRREADY "\nquit=6000\n", NULL, NULL), 0);
	for ( i = 0; i < srv_seen && i < SEEN_MAX; i++ ) {
		if ( knl_strcmp(srv_path[i], "/p") == 0 ) posted = TRUE;
	}
	tm_printf((UB*)"  the form sent: %s\n", srv_body);
	KT_ASSERT(posted);
	KT_ASSERT(knl_strcmp(srv_body, "q=abc&r=%E6%97%A5%E6%9C%AC") == 0);
	n = read_meta(&page);
	KT_ASSERT(find_text(brmeta, n, "\"title\": \"posted q=abc&r=%E6%97%A5%E6%9C%AC\"") > 0);

	KT_ASSERT_EQ(run_browser(&page2, "ready=" BRREADY "\nquit=3000\n", "/boot/BKEEP.PPM", NULL), 0);
	srv_end();
	n = read_meta(&page2);
	KT_ASSERT(find_text(brmeta, n, "\"title\": \"kept pk=kept yes\"") > 0);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
	KT_ASSERT_ER(ob_del_obj(&page2), E_OK);
}

/*
 * WebGL and a media element (BROWSER_BLINK=1): "/m" draws with WebGL on
 * the CPU -- the canvas cleared to (0, 128, 255) and a red triangle over
 * its middle -- and opens "/a.wav" with an Audio element; the title must
 * say that the context was made and the player read the sound's length
 * (half a second: "gl c audio 5 ba"), and the screen must show the triangle in
 * the middle of the canvas and the clear colour at its corner.
 */
LOCAL INT	gl_mid = -1, gl_corner = -1, gl_low = -1;

LOCAL void media_check( CONST TS_UUID *page )
{
	TS_UUID		win;
	T_OBWPOS	wp;
	SZ		asz = 0;
	ID		k;
	UW		mid, corner, low;

	if ( !window_of(page, &win) ) return;
	k = ob_opn_obj(&win, OB_OP_READ);
	if ( k <= 0 ) return;
	knl_memset(&wp, 0, sizeof(wp));
	(void)ob_rea_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	ob_cls_obj(k);
	mid = screen_px(wp.wleft + 40 + 32, wp.wtop + BAR_H + 100 + 36);
	corner = screen_px(wp.wleft + 40 + 3, wp.wtop + BAR_H + 100 + 3);
	/* near the triangle's lower left corner: red when it stands the right way up */
	low = screen_px(wp.wleft + 40 + 20, wp.wtop + BAR_H + 100 + 45);
	tm_printf((UB*)"  WebGL: middle 0x%06x, corner 0x%06x, lower left 0x%06x\n", mid, corner, low);
	gl_mid = near_rgb(mid, 0xFF0000, 8) ? 1 : 0;
	gl_corner = near_rgb(corner, 0x0080FF, 8) ? 1 : 0;
	gl_low = near_rgb(low, 0xFF0000, 8) ? 1 : 0;
}

/*
 * The window's menu worked with the pointer (BROWSER_BLINK=1), as a user
 * works it: the right button pressed over the page opens ブラウザのメニュー,
 * and 再読込 chosen in it with the left button reads the page again (the
 * server is asked for "/f" a second time). The screen is taken with the
 * menu open.
 */
LOCAL INT	menu_seen = -1;

LOCAL INT seen_count( CONST char *p )
{
	INT	i, n = 0;

	for ( i = 0; i < srv_seen && i < SEEN_MAX; i++ ) {
		if ( knl_strcmp(srv_path[i], p) == 0 ) n++;
	}
	return n;
}

LOCAL void menu_check( CONST TS_UUID *page )
{
	TS_UUID		win;
	T_OBWPOS	wp;
	SZ		asz = 0;
	ID		k;
	INT		x, y, rh, t;

	if ( !window_of(page, &win) ) return;
	k = ob_opn_obj(&win, OB_OP_READ);
	if ( k <= 0 ) return;
	knl_memset(&wp, 0, sizeof(wp));
	(void)ob_rea_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	ob_cls_obj(k);
	x = wp.wleft + 300;
	y = wp.wtop + BAR_H + 200;
	rh = wm_menu_row_h();
	hid_ev(HID_EV_MOVE, 0, x, y, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 1, x, y, 0, 0);
	hid_ev(HID_EV_BTN_UP, 1, x, y, 0, 0);
	tk_dly_tsk(800);
	if ( kt_screen() ) (void)kt_shot("/boot/BMENU.PPM");
	/* 閉じる, 戻る, 進む, then 再読込: the middle of the fourth row, the menu's top at the pointer */
	hid_ev(HID_EV_MOVE, 0, x + 20, y + 3 * rh + rh / 2, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, x + 20, y + 3 * rh + rh / 2, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, x + 20, y + 3 * rh + rh / 2, 0, 0);
	for ( t = 0; t < 8000 && seen_count("/f") < 2; t += 100 ) tk_dly_tsk(100);
	menu_seen = seen_count("/f");
	tm_printf((UB*)"  row %d: \"/f\" asked for %d times\n", rh, menu_seen);
	(void)ob_del_obj(&win);
}

LOCAL void test_blink_menu( void )
{
	T_FSTAT	st;
	TS_UUID	page, d;
	INT	i;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18087/f", &page) ) return;
	if ( !srv_start(HTTP_PORT + 7) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}
	KT_ASSERT_ER(dt_start(), E_OK);
	for ( i = 0; i < 100 && ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK; i++ ) {
		tk_dly_tsk(100);
	}
	tk_dly_tsk(1000);
	menu_seen = -1;
	KT_ASSERT_EQ(run_browser(&page, "ready=" BRREADY "\nquit=30000\n", NULL, menu_check), 0);
	dt_quit();
	wm_update();
	srv_end();
	KT_ASSERT_EQ(menu_seen, 2);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}

/*
 * The tool panel (BROWSER_BLINK=1), worked with the pointer and the keys
 * as a user works it: a small framed window subordinate to the page's
 * window above it, with the switches ← → ↻ × and the address field.
 * The field pressed, Ctrl+A and an address typed, Enter goes there
 * ("/next"); ← goes back ("/f" again), → forward ("/next" again) and ↻
 * loads it once more. After the page's window is pressed (it comes to the
 * front), the panel is still just in front of it. The screen is taken
 * with the address typed.
 */
LOCAL INT	tool_ok = 0, tool_z = -1;

/* The window at this outer corner that is not 'not' */
LOCAL BOOL window_at( INT left, INT top, CONST TS_UUID *not, TS_UUID *win, T_OBWPOS *wp )
{
	TS_UUID	list[64];
	INT	cnt = 0, i;
	SZ	asz = 0;
	ID	k;

	if ( ob_lst_obj(OB_T_WINDOW, OB_S_WINDOW, NULL, list, 64, &cnt) < E_OK ) return FALSE;
	for ( i = 0; i < cnt && i < 64; i++ ) {
		if ( ts_uuid_cmp(&list[i], not) == 0 ) continue;
		k = ob_opn_obj(&list[i], OB_OP_READ);
		if ( k <= 0 ) continue;
		knl_memset(wp, 0, sizeof(*wp));
		(void)ob_rea_rec(k, OB_WR_PLACE, 0, wp, sizeof(*wp), &asz);
		ob_cls_obj(k);
		if ( wp->left == left && wp->top == top ) {
			*win = list[i];
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL void type_text( CONST char *s )
{
	for ( ; *s != '\0'; s++ ) {
		char	c = *s;

		if ( c >= 'a' && c <= 'z' ) type_key(0x04 + ( c - 'a' ), 0);
		else if ( c >= '1' && c <= '9' ) type_key(0x1E + ( c - '1' ), 0);
		else if ( c == '0' ) type_key(0x27, 0);
		else if ( c == '.' ) type_key(0x37, 0);
		else if ( c == '/' ) type_key(0x38, 0);
		else if ( c == ':' ) type_key(0x34, 0);		/* the JIS keyboard's ':' */
	}
}

LOCAL void tool_check( CONST TS_UUID *page )
{
	TS_UUID		win, tool;
	T_OBWPOS	wp, tp;
	SZ		asz = 0;
	ID		k;
	INT		top, sx, sy, t, step = 0;

	if ( !window_of(page, &win) ) return;
	k = ob_opn_obj(&win, OB_OP_READ);
	if ( k <= 0 ) return;
	knl_memset(&wp, 0, sizeof(wp));
	(void)ob_rea_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	ob_cls_obj(k);
	top = wp.top - ( 48 + 8 );
	if ( top < 30 ) top = 30;
	if ( !window_at(wp.left, top, &win, &tool, &tp) ) {
		tm_printf((UB*)"  no tool panel at %d,%d\n", wp.left, top);
		(void)ob_del_obj(&win);
		return;
	}
	tm_printf((UB*)"  tool panel at %d,%d, work area %d,%d-%d,%d\n", tp.left, tp.top, tp.wleft, tp.wtop,
		  tp.wright, tp.wbottom);
	sy = ( tp.wtop + tp.wbottom ) / 2;

	/* the field: the whole address taken, another typed, Enter */
	hid_ev(HID_EV_MOVE, 0, tp.wleft + 300, sy, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, tp.wleft + 300, sy, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, tp.wleft + 300, sy, 0, 0);
	tk_dly_tsk(300);
	type_key(0x04, HID_MOD_LCTRL);
	type_text("127.0.0.1:18088/next");
	tk_dly_tsk(300);
	if ( kt_screen() ) (void)kt_shot("/boot/BTOOL.PPM");
	type_key(0x28, 0);
	if ( asked_for("/next", 8000) ) step = 1;
	tm_printf((UB*)"  typed and gone: %s\n", step >= 1 ? "/next" : "(nothing)");

	/* ←, →, ↻: the switches 32 apart from 6 in */
	tk_dly_tsk(1500);
	sx = tp.wleft + 6 + 14;
	hid_ev(HID_EV_MOVE, 0, sx, sy, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, sx, sy, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, sx, sy, 0, 0);
	for ( t = 0; t < 8000 && seen_count("/f") < 2; t += 100 ) tk_dly_tsk(100);
	if ( step == 1 && seen_count("/f") == 2 ) step = 2;
	tk_dly_tsk(1500);
	sx = tp.wleft + 6 + 32 + 14;
	hid_ev(HID_EV_MOVE, 0, sx, sy, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, sx, sy, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, sx, sy, 0, 0);
	for ( t = 0; t < 8000 && seen_count("/next") < 2; t += 100 ) tk_dly_tsk(100);
	if ( step == 2 && seen_count("/next") == 2 ) step = 3;
	tk_dly_tsk(1500);
	sx = tp.wleft + 6 + 64 + 14;
	hid_ev(HID_EV_MOVE, 0, sx, sy, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, sx, sy, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, sx, sy, 0, 0);
	for ( t = 0; t < 8000 && seen_count("/next") < 3; t += 100 ) tk_dly_tsk(100);
	if ( step == 3 && seen_count("/next") == 3 ) step = 4;
	tm_printf((UB*)"  back, forward, reload: step %d, \"/f\" %d, \"/next\" %d\n", step, seen_count("/f"),
		  seen_count("/next"));
	tool_ok = step;

	/* the page's window pressed: it comes to the front, and the panel stays just in front of it */
	tk_dly_tsk(1500);
	hid_ev(HID_EV_MOVE, 0, wp.wleft + 400, wp.wtop + 300, 0, 0);
	hid_ev(HID_EV_BTN_DOWN, 0, wp.wleft + 400, wp.wtop + 300, 0, 0);
	hid_ev(HID_EV_BTN_UP, 0, wp.wleft + 400, wp.wtop + 300, 0, 0);
	tk_dly_tsk(500);
	k = ob_opn_obj(&win, OB_OP_READ);
	if ( k > 0 ) {
		(void)ob_rea_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		ob_cls_obj(k);
	}
	k = ob_opn_obj(&tool, OB_OP_READ);
	if ( k > 0 ) {
		(void)ob_rea_rec(k, OB_WR_PLACE, 0, &tp, sizeof(tp), &asz);
		ob_cls_obj(k);
	}
	tm_printf((UB*)"  the page's window z %d, the panel's z %d\n", wp.z, tp.z);
	tool_z = ( tp.z == wp.z - 1 ) ? 1 : 0;
	(void)ob_del_obj(&win);
}

LOCAL void test_blink_tool( void )
{
	T_FSTAT	st;
	TS_UUID	page, d;
	INT	i;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18088/f", &page) ) return;
	if ( !srv_start(HTTP_PORT + 8) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}
	KT_ASSERT_ER(dt_start(), E_OK);
	for ( i = 0; i < 100 && ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK; i++ ) {
		tk_dly_tsk(100);
	}
	tk_dly_tsk(1000);
	tool_ok = 0;
	tool_z = -1;
	KT_ASSERT_EQ(run_browser(&page, "trace\nready=" BRREADY "\nquit=60000\n", NULL, tool_check), 0);
	dt_quit();
	wm_update();
	srv_end();
	KT_ASSERT_EQ(tool_ok, 4);
	KT_ASSERT_EQ(tool_z, 1);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}

LOCAL void test_blink_media( void )
{
	T_FSTAT	st;
	TS_UUID	page;
	INT	n;

	if ( fs_stat(SKPROG, &st) < EX_OK ) {
		KT_SKIP("the engine is not built (make BROWSER=1)");
	}
	if ( !make_page("http://127.0.0.1:18086/m", &page) ) return;
	if ( !srv_start(HTTP_PORT + 6) ) {
		(void)ob_del_obj(&page);
		KT_SKIP("no network stack");
	}
	gl_mid = gl_corner = gl_low = -1;
	KT_ASSERT_EQ(run_browser(&page, "ready=" BRREADY "\nquit=6000\n", "/boot/BMEDIA.PPM", media_check), 0);
	srv_end();
	KT_ASSERT_EQ(gl_mid, 1);
	KT_ASSERT_EQ(gl_corner, 1);
	KT_ASSERT_EQ(gl_low, 1);
	n = read_meta(&page);
	KT_ASSERT(find_text(brmeta, n, "\"title\": \"gl c audio 5 ba\"") > 0);
	KT_ASSERT_EQ(seen_count("/v.webm"), 0);
	KT_ASSERT_ER(ob_del_obj(&page), E_OK);
}
#else
LOCAL void test_blink( void )
{
	KT_SKIP("the browser is built without Blink (BROWSER_BLINK=1)");
}

LOCAL void test_blink_input( void )
{
	KT_SKIP("the browser is built without Blink (BROWSER_BLINK=1)");
}

LOCAL void test_blink_mem( void )
{
	KT_SKIP("the browser is built without Blink (BROWSER_BLINK=1)");
}

LOCAL void test_blink_keep( void )
{
	KT_SKIP("the browser is built without Blink (BROWSER_BLINK=1)");
}

LOCAL void test_blink_media( void )
{
	KT_SKIP("the browser is built without Blink (BROWSER_BLINK=1)");
}

LOCAL void test_blink_menu( void )
{
	KT_SKIP("the browser is built without Blink (BROWSER_BLINK=1)");
}

LOCAL void test_blink_tool( void )
{
	KT_SKIP("the browser is built without Blink (BROWSER_BLINK=1)");
}
#endif

#ifdef KT_V8
/* make ... KT_V8=<name>,<name>: only the tests named run */
LOCAL BOOL v8_want( CONST char *name )
{
	CONST char	*p = KT_V8;
	INT		i;

	while ( *p != '\0' ) {
		for ( i = 0; name[i] != '\0' && p[i] == name[i]; i++ ) ;
		if ( name[i] == '\0' && ( p[i] == ',' || p[i] == '\0' ) ) return TRUE;
		while ( *p != '\0' && *p != ',' ) p++;
		if ( *p == ',' ) p++;
	}
	return FALSE;
}
#define V8_RUN(fn)	do { if ( v8_want(#fn) ) KT_RUN(fn); } while ( 0 )
#else
#define V8_RUN(fn)	KT_RUN(fn)
#endif

EXPORT void ktest_v8( void )
{
#if defined(KT_NET_USER) && defined(KT_SITE)
	KT_RUN(test_site);
	return;
#endif
	V8_RUN(test_present);
	V8_RUN(test_script);
	V8_RUN(test_skia);
	V8_RUN(test_browser);
	V8_RUN(test_blink);
	V8_RUN(test_blink_input);
	V8_RUN(test_blink_mem);
	V8_RUN(test_blink_keep);
	V8_RUN(test_blink_media);
	V8_RUN(test_blink_menu);
	V8_RUN(test_blink_tool);
	V8_RUN(test_desk);
	V8_RUN(test_https);
}
