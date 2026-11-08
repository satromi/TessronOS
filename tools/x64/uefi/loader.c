/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	loader.c
 *	The UEFI loader of TessronOS on x64 (\EFI\BOOT\BOOTX64.EFI).
 *
 *	It reads the kernel (tessronos.elf) and the one line of start-up
 *	settings (cmdline.txt) from the volume it was itself read from (its
 *	own directory, \EFI\TESSRON\, then the root; in each the name, then
 *	the name with .new, then with .old), sets the screen up
 *	through the Graphics Output Protocol,
 *	notes what the kernel needs of the firmware (the memory map, the
 *	ACPI and SMBIOS tables, a seed for the random numbers, which
 *	partition it came from), builds the page tables of the kernel's
 *	address space, leaves the boot services and jumps to the kernel
 *	with the physical address of a T_BOOTINFO
 *	(kernel/sysdepend/cpu/core/x86_64/bootinfo.h) in rdi.
 *
 *	A machine without a serial port is debugged from a photograph of
 *	its screen and from files: once the screen is set up, what the
 *	loader says is drawn on it in a font large enough to read on a
 *	photograph, with the step under way in a band at the top; the
 *	kernel's console goes on below (fbcon.c). Before the boot services
 *	are left it writes, on the volume it came from, \EFI\TESSRON\
 *	LOADER.TXT (what it said), HW.TXT (the processor, the screen modes,
 *	the memory map, the ACPI tables, SMBIOS, the PCI functions and the
 *	USB devices the firmware enumerated, with all their configurations)
 *	and the ACPI tables themselves under ACPI\ (DSDT.BIN, SSDT1.BIN...),
 *	so that one start of a machine tells what its drivers need.
 *	"hwreport=0" in cmdline.txt leaves the report out.
 *
 *	The firmware's runtime services are not used afterwards: the kernel
 *	reads the clock from the CMOS and powers off through ACPI. A boot
 *	entry システム登録 asks for (\EFI\TESSRON\BOOTREG.TXT) is written
 *	here, while the boot services are there (boot_entry_request).
 */

#include "efi.h"
#include "../../../kernel/sysdepend/cpu/core/x86_64/bootinfo.h"

#define LOCAL		static
typedef unsigned char	UB;
#include "../../../kernel/sysdepend/x64/confont.h"

/* What the loader allocates for the kernel is of this type, one of the OS's own */
#define MEM_TESSRON		0x80000001u

#define KIMG_VA_BASE		0xFFFFFFFF80000000ull
#define KVA_BASE		0xFFFF800000000000ull
#define DEV_VA_BASE		0xFFFFC00000000000ull

#define PG_P			0x001ull
#define PG_RW			0x002ull
#define PG_US			0x004ull
#define PG_PWT			0x008ull
#define PG_PCD			0x010ull
#define PG_A			0x020ull
#define PG_D			0x040ull
#define PG_PS			0x080ull
#define PG_G			0x100ull
#define PG_NX			0x8000000000000000ull
#define PG_TABLE		( PG_P | PG_RW | PG_US )

#define MSR_EFER		0xc0000080u
#define MSR_PAT			0x00000277u
#define EFER_NXE		0x800ull
#define PAT_VALUE		0x0007010600070106ull	/* WB, WC, UC-, UC, WB, WC, UC-, UC */

#define GB			0x40000000ull
#define MB2			0x200000ull

static EFI_SYSTEM_TABLE		*ST;
static EFI_BOOT_SERVICES	*BS;
static EFI_HANDLE		Image;
static EFI_FILE_PROTOCOL	*Root;
static T_BOOTINFO		*BI;

typedef __builtin_va_list	va_list;
#define va_start(a, l)		__builtin_va_start(a, l)
#define va_arg(a, t)		__builtin_va_arg(a, t)
#define va_end(a)		__builtin_va_end(a)

/* ------------------------------------------------------------------ helpers */

void *memset( void *d, int c, UINTN n )
{
	UINT8	*p = d;

	while ( n-- > 0 ) *p++ = (UINT8)c;
	return d;
}

void *memcpy( void *d, const void *s, UINTN n )
{
	UINT8		*p = d;
	const UINT8	*q = s;

	while ( n-- > 0 ) *p++ = *q++;
	return d;
}

int memcmp( const void *a, const void *b, UINTN n )
{
	const UINT8	*p = a, *q = b;

	for ( ; n > 0; n--, p++, q++ ) {
		if ( *p != *q ) return ( *p < *q ) ? -1 : 1;
	}
	return 0;
}

static int guid_eq( const EFI_GUID *a, const EFI_GUID *b )
{
	const UINT8	*p = (const UINT8 *)a, *q = (const UINT8 *)b;
	int		i;

	for ( i = 0; i < 16; i++ ) {
		if ( p[i] != q[i] ) return 0;
	}
	return 1;
}

static int has_n( const char *s, UINTN n, const char *w )
{
	UINTN	i, k;

	for ( i = 0; i < n; i++ ) {
		if ( i > 0 && s[i - 1] != ' ' ) continue;
		for ( k = 0; w[k] != '\0' && i + k < n && s[i + k] == w[k]; k++ ) ;
		if ( w[k] == '\0' && ( i + k == n || s[i + k] == ' ' || s[i + k] == '\r' || s[i + k] == '\n' ) ) {
			return 1;
		}
	}
	return 0;
}

static int has( const char *s, const char *w )
{
	int	i;

	for ( ; *s != '\0'; s++ ) {
		for ( i = 0; w[i] != '\0' && s[i] == w[i]; i++ ) ;
		if ( w[i] == '\0' ) return 1;
	}
	return 0;
}

static inline void outb( UINT16 port, UINT8 v )
{
	__asm__ volatile ( "outb %0, %1" :: "a"(v), "Nd"(port) );
}

static inline UINT8 inb( UINT16 port )
{
	UINT8	v;

	__asm__ volatile ( "inb %1, %0" : "=a"(v) : "Nd"(port) );
	return v;
}

static inline UINT64 rdmsr( UINT32 msr )
{
	UINT32	lo, hi;

	__asm__ volatile ( "rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) );
	return ( (UINT64)hi << 32 ) | lo;
}

static inline void wrmsr( UINT32 msr, UINT64 v )
{
	__asm__ volatile ( "wrmsr" :: "c"(msr), "a"((UINT32)v), "d"((UINT32)( v >> 32 )) );
}

static inline void cpuid2( UINT32 leaf, UINT32 sub, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d )
{
	__asm__ volatile ( "cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub) );
}

static inline void cpuid( UINT32 leaf, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d )
{
	cpuid2(leaf, 0, a, b, c, d);
}

/*
 * COM1, once the boot services are left (before, the firmware's console
 * goes there itself where there is one). Where there is none the port
 * reads as all ones and the wait for room ends at once; it is bounded
 * besides.
 */
static int	com1_ok = 0;

static void com1_init( void )
{
	outb(0x3f8 + 1, 0x00);		/* no interrupts */
	outb(0x3f8 + 3, 0x80);		/* divisor latch */
	outb(0x3f8 + 0, 0x01);		/* 115200 */
	outb(0x3f8 + 1, 0x00);
	outb(0x3f8 + 3, 0x03);		/* 8N1 */
	outb(0x3f8 + 2, 0xc7);		/* FIFO on, cleared */
	outb(0x3f8 + 4, 0x03);		/* DTR, RTS */
	com1_ok = ( inb(0x3f8 + 5) != 0xff );
}

static void com1_putc( char c )
{
	int	n = 100000;

	if ( !com1_ok ) return;
	while ( ( inb(0x3f8 + 5) & 0x20 ) == 0 && --n > 0 ) {
		;
	}
	outb(0x3f8, (UINT8)c);
}

/* ------------------------------------------------------------------ text into a buffer */

typedef struct {
	char	*p;
	UINTN	len, cap;
} TBUF;

static void tb_putc( TBUF *t, char c )
{
	if ( t->p != 0 && t->len + 1 < t->cap ) {
		t->p[t->len++] = c;
		t->p[t->len] = '\0';
	}
}

static void tb_puts( TBUF *t, const char *s )
{
	while ( *s != '\0' ) tb_putc(t, *s++);
}

static void tb_num( TBUF *t, UINT64 v, int base, int width, char pad )
{
	char	b[24];
	int	i = 0;

	do {
		b[i++] = "0123456789abcdef"[v % (UINT64)base];
		v /= (UINT64)base;
	} while ( v != 0 && i < 23 );
	while ( i < width && i < 23 ) b[i++] = pad;
	while ( i > 0 ) tb_putc(t, b[--i]);
}

/*
 * A small printf: %s %c %d %u %x with a width ("%08x"), and the l
 * prefix for 64 bits ("%016lx", "%lu")
 */
static void tb_vfmt( TBUF *t, const char *f, va_list ap )
{
	for ( ; *f != '\0'; f++ ) {
		int	width = 0, l = 0;
		char	pad = ' ';
		UINT64	v;

		if ( *f != '%' ) {
			tb_putc(t, *f);
			continue;
		}
		f++;
		if ( *f == '0' ) {
			pad = '0';
			f++;
		}
		while ( *f >= '0' && *f <= '9' ) width = width * 10 + ( *f++ - '0' );
		while ( *f == 'l' ) {
			l = 1;
			f++;
		}
		switch ( *f ) {
		case 's': {
			const char	*s = va_arg(ap, const char *);
			int		n = 0;

			if ( s == 0 ) s = "(null)";
			tb_puts(t, s);
			while ( s[n] != '\0' ) n++;
			while ( n++ < width ) tb_putc(t, ' ');
			break;
		}
		case 'c':
			tb_putc(t, (char)va_arg(ap, int));
			break;
		case 'd': {
			INT64	d = l ? va_arg(ap, INT64) : (INT64)va_arg(ap, int);

			if ( d < 0 ) {
				tb_putc(t, '-');
				d = -d;
			}
			tb_num(t, (UINT64)d, 10, width, pad);
			break;
		}
		case 'u':
		case 'x':
			v = l ? va_arg(ap, UINT64) : (UINT64)va_arg(ap, UINT32);
			tb_num(t, v, ( *f == 'x' ) ? 16 : 10, width, pad);
			break;
		case '%':
			tb_putc(t, '%');
			break;
		default:
			tb_putc(t, '?');
			break;
		}
	}
}

static void tb_fmt( TBUF *t, const char *f, ... )
{
	va_list	ap;

	va_start(ap, f);
	tb_vfmt(t, f, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ the screen console */

/*
 * The screen once the loader has set its mode: the console's font drawn
 * con_scale times larger on a wide screen, the top row a band that says
 * the step under way, the lines below it from the top down; when they
 * reach the foot the next goes to the top again, the row after it kept
 * clear so that the newest can be found.
 */
static volatile UINT32	*con_fb = 0;
static UINT32		con_w, con_h, con_pitch, con_scale = 1, con_cols, con_rows, con_cx, con_cy;
static UINT32		con_fg, con_bg, con_band, con_red;
static T_BIFB		*con_info;

static UINT32 rgb( UINT32 r, UINT32 g, UINT32 b )
{
	UINT32	v = 0;
	int	s;

	if ( con_info->format == BI_FB_BGRX ) return ( r << 16 ) | ( g << 8 ) | b;
	if ( con_info->format == BI_FB_RGBX ) return ( b << 16 ) | ( g << 8 ) | r;
	/* masks: the top eight bits of each channel into its mask */
	for ( s = 0; s < 32 && ( ( con_info->mask_r >> s ) & 1 ) == 0; s++ ) ;
	v |= ( ( r << s ) >> 0 ) & con_info->mask_r;
	for ( s = 0; s < 32 && ( ( con_info->mask_g >> s ) & 1 ) == 0; s++ ) ;
	v |= ( g << s ) & con_info->mask_g;
	for ( s = 0; s < 32 && ( ( con_info->mask_b >> s ) & 1 ) == 0; s++ ) ;
	v |= ( b << s ) & con_info->mask_b;
	return v;
}

static void con_cell( UINT32 col, UINT32 row, char c, UINT32 fg, UINT32 bg )
{
	const UB	*g;
	UINT32		y, x, cw = CONFONT_W * con_scale, ch = CONFONT_H * con_scale;

	if ( (UB)c < CONFONT_FIRST || (UB)c > CONFONT_LAST ) c = '?';
	g = confont[(UB)c - CONFONT_FIRST];
	for ( y = 0; y < ch; y++ ) {
		volatile UINT32	*p = con_fb + (UINT64)( row * ch + y ) * con_pitch + col * cw;
		UB		bits = g[y / con_scale];

		for ( x = 0; x < cw; x++ ) {
			p[x] = ( bits & ( 0x80 >> ( x / con_scale ) ) ) ? fg : bg;
		}
	}
}

static void con_clear_row( UINT32 row, UINT32 bg )
{
	UINT32	c;

	for ( c = 0; c < con_cols; c++ ) con_cell(c, row, ' ', bg, bg);
}

static void con_newline( void )
{
	con_cx = 0;
	if ( ++con_cy >= con_rows ) con_cy = 1;
	con_clear_row(con_cy, con_bg);
	con_clear_row(( con_cy + 1 < con_rows ) ? con_cy + 1 : 1, con_bg);
}

static void con_putc( char c, UINT32 fg )
{
	if ( con_fb == 0 ) return;
	if ( c == '\n' ) {
		con_newline();
		return;
	}
	if ( c == '\r' ) return;
	con_cell(con_cx, con_cy, c, fg, con_bg);
	if ( ++con_cx >= con_cols ) con_newline();
}

/* The band at the top: the step under way */
static void stage( const char *s )
{
	TBUF	t;
	char	b[200];
	UINT32	i;

	t.p = b;
	t.len = 0;
	t.cap = sizeof(b);
	b[0] = '\0';
	tb_fmt(&t, " TessronOS loader: %s", s);
	if ( con_fb == 0 ) return;
	for ( i = 0; i < con_cols; i++ ) {
		con_cell(i, 0, ( i < t.len ) ? b[i] : ' ', con_bg, con_band);
	}
}

/* ------------------------------------------------------------------ the marks of the start */

static T_BIMARK	*mark_page = 0;
static void say( const char *f, ... );
static EFI_STATUS keep_pages( EFI_ALLOCATE_TYPE how, UINTN n, EFI_PHYSICAL_ADDRESS *a );

/* Mark k (BI_MK_*) in its colour at the foot of the screen, and in the page of marks */
static void mark( UINT32 k, UINT32 r, UINT32 g, UINT32 b )
{
	UINT32	x, y, c, x0 = 8 + ( k - 1 ) * 30, y0;

	if ( mark_page != 0 ) mark_page->last = k;
	if ( con_fb == 0 || con_h < 40 || x0 + 24 > con_w ) return;
	c = rgb(r, g, b);
	y0 = con_h - 32;
	for ( y = 0; y < 24; y++ ) {
		for ( x = 0; x < 24; x++ ) {
			con_fb[(UINT64)( y0 + y ) * con_pitch + x0 + x] = c;
		}
	}
}

/*
 * The page of marks, at BI_MARK_PA when the firmware lets it be taken:
 * what the last start wrote there, if anything, said, and the page made
 * this start's
 */
static void mark_start( void )
{
	EFI_PHYSICAL_ADDRESS	a = BI_MARK_PA;
	T_BIMARK		*m;

	if ( EFI_ERROR(keep_pages(AllocateAddress, 1, &a)) ) {
		say("TessronOS loader: the page of marks at 0x%lx is taken: no marks kept across a reset\n",
		    (UINT64)BI_MARK_PA);
		return;
	}
	m = (T_BIMARK *)a;
	if ( m->magic == BI_MARK_MAGIC ) {
		say("TessronOS loader: the last start (%u) got to mark %u", m->runs, m->last);
		if ( m->vec != 0xffffffffu ) {
			say(", exception %u at RIP 0x%lx, CR2 0x%lx, error 0x%lx", m->vec, m->rip, m->cr2, m->err);
		}
		if ( m->starting != 0 ) {
			say(", while starting the processor of APIC %u", m->starting - 1);
		}
		say("\n");
		m->runs++;
	} else {
		memset(m, 0, sizeof(*m));
		m->magic = BI_MARK_MAGIC;
		m->runs = 1;
	}
	m->last = 0;
	m->vec = 0xffffffffu;
	m->starting = 0;
	mark_page = m;
	BI->mark_pa = (UINT64)a;
}

static void con_start( T_BIFB *fb )
{
	UINT32	y, x;

	if ( fb->pa == 0 || fb->format == BI_FB_NONE || fb->width < 320 ) return;
	con_info = fb;
	con_fb = (volatile UINT32 *)fb->pa;
	con_w = fb->width;
	con_h = fb->height;
	con_pitch = fb->stride / 4;
	con_scale = ( con_w >= 1600 && con_h >= 900 ) ? 2 : 1;
	con_cols = con_w / ( CONFONT_W * con_scale );
	con_rows = con_h / ( CONFONT_H * con_scale );
	con_fg = rgb(0xe0, 0xe0, 0xe0);
	con_bg = rgb(0, 0, 0);
	con_band = rgb(0x60, 0xc0, 0xff);
	con_red = rgb(0xff, 0x50, 0x50);
	for ( y = 0; y < con_h; y++ ) {
		for ( x = 0; x < con_w; x++ ) con_fb[(UINT64)y * con_pitch + x] = con_bg;
	}
	con_cx = 0;
	con_cy = 1;
}

/* ------------------------------------------------------------------ what the loader says */

static int	booted_out = 0;		/* the boot services have been left */
static char	log_mem[64 * 1024];
static TBUF	logt = { log_mem, 0, sizeof(log_mem) };

static void out_text( const char *s, UINT32 fg )
{
	CHAR16	buf[128];
	int	n = 0;

	tb_puts(&logt, s);
	for ( ; *s != '\0'; s++ ) {
		if ( booted_out ) {
			if ( *s == '\n' ) com1_putc('\r');
			com1_putc(*s);
		}
		if ( con_fb != 0 ) {
			con_putc(*s, fg);
			continue;
		}
		if ( booted_out ) continue;
		if ( *s == '\n' ) buf[n++] = L'\r';
		buf[n++] = (CHAR16)(UINT8)*s;
		if ( n >= 120 ) {
			buf[n] = 0;
			ST->ConOut->OutputString(ST->ConOut, buf);
			n = 0;
		}
	}
	if ( n > 0 && !booted_out && con_fb == 0 ) {
		buf[n] = 0;
		ST->ConOut->OutputString(ST->ConOut, buf);
	}
}

static void say_v( UINT32 fg, const char *f, va_list ap )
{
	char	b[512];
	TBUF	t = { b, 0, sizeof(b) };

	b[0] = '\0';
	tb_vfmt(&t, f, ap);
	out_text(b, fg);
}

static void say( const char *f, ... )
{
	va_list	ap;

	va_start(ap, f);
	say_v(con_fg, f, ap);
	va_end(ap);
}

static void write_logs( void );

/* The loader cannot go on: said in red, the log written where it can be, and stopped */
static void stop( const char *why, EFI_STATUS st )
{
	stage("STOPPED");
	if ( con_fb != 0 ) con_putc('\n', con_red);
	out_text("TessronOS loader: STOPPED: ", con_red);
	out_text(why, con_red);
	if ( st != 0 ) {
		char	b[40];
		TBUF	t = { b, 0, sizeof(b) };

		tb_fmt(&t, " (status 0x%016lx)", st);
		out_text(b, con_red);
	}
	out_text("\n", con_red);
	if ( !booted_out ) write_logs();
	for ( ;; ) {
		__asm__ volatile ( "hlt" );
	}
}

/*
 * The memory the kernel keeps (its image and heap, the T_BOOTINFO, the
 * page tables, the pages below 1MB, the page of marks) is asked for as
 * EfiLoaderData, a type every firmware knows, and remembered here: the
 * kernel is told of it as its own (BI_MEM_KERNEL) when the memory map is
 * converted. Some firmware mishandles the types the specification leaves
 * to OS loaders (0x80000000 up, MEM_TESSRON) and does not come back from
 * ExitBootServices; "memtype=custom" in the settings asks for that type
 * again, to tell the two apart on a machine.
 */
#define KEPT_MAX	16

static UINT32	kept_type = EfiLoaderData;
static UINT64	kept_pa[KEPT_MAX], kept_size[KEPT_MAX];
static UINT32	kept_n = 0;

static void keep( UINT64 pa, UINT64 size )
{
	UINT32	i, j;

	if ( kept_n >= KEPT_MAX ) stop("too many pieces of memory kept for the kernel", 0);
	for ( i = 0; i < kept_n && kept_pa[i] < pa; i++ ) ;
	for ( j = kept_n; j > i; j-- ) {
		kept_pa[j] = kept_pa[j - 1];
		kept_size[j] = kept_size[j - 1];
	}
	kept_pa[i] = pa;
	kept_size[i] = size;
	kept_n++;
}

/* Pages the kernel keeps: allocated, zeroed and remembered */
static EFI_STATUS keep_pages( EFI_ALLOCATE_TYPE how, UINTN n, EFI_PHYSICAL_ADDRESS *a )
{
	EFI_STATUS	st = BS->AllocatePages(how, kept_type, n, a);

	if ( !EFI_ERROR(st) ) {
		keep(*a, (UINT64)n * EFI_PAGE_SIZE);
	}
	return st;
}

static void *alloc_pages( UINTN n, UINT32 type )
{
	EFI_PHYSICAL_ADDRESS	a = 0;
	EFI_STATUS		st;

	(void)type;
	st = keep_pages(AllocateAnyPages, n, &a);
	if ( EFI_ERROR(st) ) {
		say("TessronOS loader: no room for %lu pages\n", (UINT64)n);
		stop("out of memory", st);
	}
	memset((void *)a, 0, n * EFI_PAGE_SIZE);
	return (void *)a;
}

static void *alloc_pool( UINTN n )
{
	void	*p = 0;

	if ( EFI_ERROR(BS->AllocatePool(EfiLoaderData, n, &p)) ) return 0;
	return p;
}

/* ------------------------------------------------------------------ files */

#define OWN_DIR_MAX	96
static CHAR16	own_dir[OWN_DIR_MAX];	/* "\EFI\BOOT\": where the loader's file is, "" not known */

static EFI_FILE_PROTOCOL *open_root( void )
{
	EFI_GUID			li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_GUID			fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_GUID			dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
	EFI_LOADED_IMAGE_PROTOCOL	*li;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL	*fs;
	EFI_DEVICE_PATH_PROTOCOL	*dp;
	EFI_FILE_PROTOCOL		*root;
	EFI_STATUS			st;

	st = BS->HandleProtocol(Image, &li_guid, (void **)&li);
	if ( EFI_ERROR(st) ) stop("no loaded image protocol", st);

	/* the partition the loader came from: its GPT unique GUID, for the kernel to find its disk by */
	if ( !EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &dp_guid, (void **)&dp)) ) {
		int	n;

		for ( n = 0; n < 64 && dp->Type != DP_TYPE_END; n++ ) {
			UINT16	len = (UINT16)( dp->Length[0] | ( dp->Length[1] << 8 ) );

			if ( dp->Type == DP_TYPE_MEDIA && dp->SubType == DP_MEDIA_HARDDRIVE ) {
				HARDDRIVE_DEVICE_PATH	*hd = (HARDDRIVE_DEVICE_PATH *)dp;

				BI->esp_part = hd->PartitionNumber;
				if ( hd->SignatureType == 2 ) {
					memcpy(BI->esp_guid, hd->Signature, 16);
					BI->flags |= BI_F_ESP_GUID;
				}
			}
			say("  device path node %u.%u (%u bytes)\n", dp->Type, dp->SubType, len);
			if ( len < 4 ) break;
			dp = (EFI_DEVICE_PATH_PROTOCOL *)( (UINT8 *)dp + len );
		}
	}

	/* the directory of the loader's own file: the kernel and the settings are looked for there first */
	own_dir[0] = 0;
	if ( li->FilePath != 0 ) {
		UINTN	k = 0, j, cut = 0;
		int	n;

		dp = (EFI_DEVICE_PATH_PROTOCOL *)li->FilePath;
		for ( n = 0; n < 16 && dp->Type != DP_TYPE_END; n++ ) {
			UINT16	len = (UINT16)( dp->Length[0] | ( dp->Length[1] << 8 ) );

			if ( dp->Type == DP_TYPE_MEDIA && dp->SubType == DP_MEDIA_FILEPATH ) {
				CHAR16	*s = (CHAR16 *)( (UINT8 *)dp + 4 );

				for ( j = 0; j < ( len - 4u ) / 2 && s[j] != 0 && k < OWN_DIR_MAX - 1; j++ ) {
					own_dir[k++] = s[j];
				}
			}
			if ( len < 4 ) break;
			dp = (EFI_DEVICE_PATH_PROTOCOL *)( (UINT8 *)dp + len );
		}
		for ( j = 0; j < k; j++ ) {
			if ( own_dir[j] == L'\\' ) cut = j + 1;
		}
		own_dir[cut] = 0;
	}

	st = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
	if ( EFI_ERROR(st) ) stop("the volume the loader came from has no file system", st);
	st = fs->OpenVolume(fs, &root);
	if ( EFI_ERROR(st) ) stop("cannot open the volume", st);
	return root;
}

/* A whole file read into memory the boot services give; NULL when there is no such file */
static UINT8 *read_file( EFI_FILE_PROTOCOL *root, CHAR16 *name, UINTN *p_size )
{
	EFI_FILE_PROTOCOL	*f;
	UINT64			size = 0;
	UINT8			*buf;
	UINTN			n;
	EFI_STATUS		st;

	st = root->Open(root, &f, name, EFI_FILE_MODE_READ, 0);
	if ( EFI_ERROR(st) ) return 0;
	f->SetPosition(f, 0xFFFFFFFFFFFFFFFFull);	/* to the end: its size */
	f->GetPosition(f, &size);
	f->SetPosition(f, 0);
	st = BS->AllocatePool(EfiLoaderData, size + 1, (void **)&buf);
	if ( EFI_ERROR(st) ) stop("out of memory for a file", st);
	n = size;
	st = f->Read(f, &n, buf);
	f->Close(f);
	if ( EFI_ERROR(st) || n != size ) stop("cannot read a file", st);
	buf[size] = 0;
	*p_size = size;
	return buf;
}

/* A directory, made when there is none */
static EFI_FILE_PROTOCOL *open_dir( EFI_FILE_PROTOCOL *at, CHAR16 *name )
{
	EFI_FILE_PROTOCOL	*d;

	if ( at == 0 ) return 0;
	if ( EFI_ERROR(at->Open(at, &d, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
				EFI_FILE_DIRECTORY)) ) {
		return 0;
	}
	return d;
}

/* A file written anew: one there before is deleted first, so that none of it is left over */
static int write_file( EFI_FILE_PROTOCOL *dir, CHAR16 *name, const void *data, UINTN len )
{
	EFI_FILE_PROTOCOL	*f;
	UINTN			n = len;
	EFI_STATUS		st;

	if ( dir == 0 ) return 0;
	if ( !EFI_ERROR(dir->Open(dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) ) {
		f->Delete(f);
	}
	st = dir->Open(dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	if ( EFI_ERROR(st) ) return 0;
	st = f->Write(f, &n, (void *)data);
	f->Flush(f);
	f->Close(f);
	return !EFI_ERROR(st) && n == len;
}

static EFI_FILE_PROTOCOL	*ts_dir = 0, *acpi_dir = 0;
static EFI_FILE_PROTOCOL	*log_root = 0, *log_dir = 0, *log_acpi = 0;
static int			no_tslog = 0;		/* tslog=0 */

/* How many bytes of a device path come before its hard drive node; -1 when it has none */
static int dp_disk_len( EFI_DEVICE_PATH_PROTOCOL *dp )
{
	UINT8	*p0 = (UINT8 *)dp;
	int	n;

	for ( n = 0; n < 64 && dp->Type != DP_TYPE_END; n++ ) {
		UINT16	len = (UINT16)( dp->Length[0] | ( dp->Length[1] << 8 ) );

		if ( dp->Type == DP_TYPE_MEDIA && dp->SubType == DP_MEDIA_HARDDRIVE ) {
			return (int)( (UINT8 *)dp - p0 );
		}
		if ( len < 4 ) break;
		dp = (EFI_DEVICE_PATH_PROTOCOL *)( (UINT8 *)dp + len );
	}
	return -1;
}

/*
 * The volume labelled TSLOG on the disk the loader came from (tools/mksd.py
 * --log-mb), its root; 0 when the firmware shows no such volume. Windows
 * shows that partition as a drive of its own on a removable disk, where it
 * does not show the EFI system partition, so the records are written there
 * too, to be copied with Explorer.
 */
static EFI_FILE_PROTOCOL *open_log_volume( void )
{
	EFI_GUID			li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_GUID			fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_GUID			dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
	EFI_GUID			fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
	EFI_LOADED_IMAGE_PROTOCOL	*li;
	EFI_DEVICE_PATH_PROTOCOL	*mine, *dp;
	EFI_HANDLE			*h = 0;
	UINTN				n = 0, i;
	int				plen;
	EFI_FILE_PROTOCOL		*found = 0;

	if ( EFI_ERROR(BS->HandleProtocol(Image, &li_guid, (void **)&li))
	  || EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &dp_guid, (void **)&mine)) ) {
		return 0;
	}
	plen = dp_disk_len(mine);
	if ( plen < 0 || EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &fs_guid, 0, &n, &h)) ) {
		return 0;
	}
	for ( i = 0; i < n && found == 0; i++ ) {
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL	*fs;
		EFI_FILE_PROTOCOL		*root;
		UINT64				info[64];
		UINTN				isz = sizeof(info);
		EFI_FILE_SYSTEM_INFO		*fsi = (EFI_FILE_SYSTEM_INFO *)info;
		static const CHAR16		want[] = L"TSLOG";
		int				k;

		if ( h[i] == li->DeviceHandle
		  || EFI_ERROR(BS->HandleProtocol(h[i], &dp_guid, (void **)&dp))
		  || dp_disk_len(dp) != plen || memcmp(dp, mine, (UINTN)plen) != 0
		  || EFI_ERROR(BS->HandleProtocol(h[i], &fs_guid, (void **)&fs))
		  || EFI_ERROR(fs->OpenVolume(fs, &root)) ) {
			continue;
		}
		if ( !EFI_ERROR(root->GetInfo(root, &fsi_guid, &isz, info)) ) {
			for ( k = 0; want[k] != 0 && fsi->VolumeLabel[k] == want[k]; k++ ) ;
			if ( want[k] == 0 && fsi->VolumeLabel[k] == 0 ) {
				found = root;
				continue;
			}
		}
		root->Close(root);
	}
	if ( h != 0 ) BS->FreePool(h);
	return found;
}

static void open_dirs( void )
{
	EFI_FILE_PROTOCOL	*efi;

	efi = open_dir(Root, L"\\EFI");
	ts_dir = open_dir(efi, L"TESSRON");
	acpi_dir = open_dir(ts_dir, L"ACPI");
	if ( efi != 0 ) efi->Close(efi);
	if ( log_dir == 0 && !no_tslog ) {
		if ( log_root == 0 ) log_root = open_log_volume();
		log_dir = open_dir(log_root, L"TESSRON");
		log_acpi = open_dir(log_dir, L"ACPI");
		say("TessronOS loader: the records go to \\EFI\\TESSRON%s\n",
		    ( log_dir != 0 ) ? " and to TESSRON of the volume TSLOG" : " (no volume TSLOG)");
	}
}

/* A record written to \EFI\TESSRON (acpi: its ACPI) and to the same place of the volume TSLOG */
static int write_log( int acpi, CHAR16 *name, const void *data, UINTN len )
{
	int	ok = write_file(acpi ? acpi_dir : ts_dir, name, data, len);

	(void)write_file(acpi ? log_acpi : log_dir, name, data, len);
	return ok;
}

static void close_log_dirs( void )
{
	if ( log_acpi != 0 ) log_acpi->Close(log_acpi);
	if ( log_dir != 0 ) log_dir->Close(log_dir);
	if ( log_root != 0 ) log_root->Close(log_root);
	log_root = log_dir = log_acpi = 0;
}

static void write_logs( void )
{
	if ( ts_dir == 0 && Root != 0 ) open_dirs();
	(void)write_log(0, L"LOADER.TXT", log_mem, logt.len);
}

/* A path of the volume as text that say() can show */
static void path_text( const CHAR16 *p, char *out, UINTN max )
{
	UINTN	i;

	for ( i = 0; i < max - 1 && p[i] != 0; i++ ) out[i] = ( p[i] < 0x80 ) ? (char)p[i] : '?';
	out[i] = 0;
}

/*
 * A file of the system read from the first place that has it: the
 * loader's own directory (\EFI\TESSRON\ of a disk the system was put
 * on beside another), \EFI\TESSRON\ (the disk's own fallback loader in
 * \EFI\BOOT\), and the root (the disk image written to a USB memory).
 *
 * In each place NAME, then NAME.new, then NAME.old. The system update
 * (application/desktop/dtsysup.c) puts a new file in place by renaming
 * NAME to NAME.old and then NAME.new to NAME; a cut in the power between
 * the two renames leaves NAME.new, written whole and synced before the
 * first rename, and NAME.old, the one before it.
 */
static UINT8 *read_near( CHAR16 *name, UINTN *p_size )
{
	static CHAR16	ts[] = L"\\EFI\\TESSRON\\";
	static CHAR16	*tails[3] = { L"", L".new", L".old" };
	CHAR16		path[OWN_DIR_MAX + 40];
	CHAR16		*dirs[3];
	char		t[OWN_DIR_MAX + 40];
	UINT8		*b;
	UINTN		i, j, k, n;

	dirs[0] = own_dir;
	dirs[1] = ts;
	dirs[2] = L"\\";
	for ( i = 0; i < 3; i++ ) {
		if ( dirs[i][0] == 0 ) continue;
		for ( j = 0; j < 3; j++ ) {
			for ( k = 0; dirs[i][k] != 0 && k < OWN_DIR_MAX; k++ ) path[k] = dirs[i][k];
			for ( n = 0; name[n] != 0 && k < OWN_DIR_MAX + 30; n++ ) path[k++] = name[n];
			for ( n = 0; tails[j][n] != 0; n++ ) path[k++] = tails[j][n];
			path[k] = 0;
			b = read_file(Root, path, p_size);
			if ( b != 0 ) {
				path_text(path, t, sizeof(t));
				say("TessronOS loader: %s (%lu bytes)\n", t, (UINT64)*p_size);
				return b;
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ the firmware's boot entry */

/*
 * システム登録 (application/sysinst) leaves \EFI\TESSRON\BOOTREG.TXT on
 * the ESP the system was started from when it has put the system on an
 * internal disk: the partition of that disk's ESP and the loader's file
 * there. The kernel does not keep the runtime services, so the entry is
 * written here, while the boot services are there: Boot#### (the one
 * that names the same file already, else the first number free), the
 * number put in BootOrder (first when the whole disk was taken, last
 * beside another system), BootNext, and the machine started again, to
 * come up from the internal disk once. The request is deleted before
 * anything is written, so that a firmware that refuses does not bring
 * it back at every start.
 */
#define REQ_MAX		1024

static UINT64 req_num( const char *s, UINTN n, const char *key )
{
	UINTN	i, k, m = 0;
	UINT64	v = 0;

	while ( key[m] != 0 ) m++;
	for ( i = 0; i + m < n; i++ ) {
		if ( ( i > 0 && s[i - 1] != '\n' ) || memcmp(s + i, key, m) != 0 ) continue;
		for ( k = i + m; k < n && s[k] >= '0' && s[k] <= '9'; k++ ) v = v * 10 + (UINT64)( s[k] - '0' );
		return v;
	}
	return ~0ull;
}

/* The text after key= to the end of its line, at most max - 1 of it; 0 when there is none */
static UINTN req_str( const char *s, UINTN n, const char *key, char *out, UINTN max )
{
	UINTN	i, k, m = 0, j = 0;

	while ( key[m] != 0 ) m++;
	for ( i = 0; i + m < n; i++ ) {
		if ( ( i > 0 && s[i - 1] != '\n' ) || memcmp(s + i, key, m) != 0 ) continue;
		for ( k = i + m; k < n && s[k] != '\r' && s[k] != '\n' && j < max - 1; k++ ) out[j++] = s[k];
		break;
	}
	out[j] = 0;
	return j;
}

static int hex_val( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

static void boot_name( CHAR16 *v, UINT32 num )
{
	static const char	hx[] = "0123456789ABCDEF";
	int			i;

	v[0] = 'B'; v[1] = 'o'; v[2] = 'o'; v[3] = 't';
	for ( i = 0; i < 4; i++ ) v[4 + i] = (CHAR16)hx[( num >> ( 12 - 4 * i ) ) & 15];
	v[8] = 0;
}

static void boot_entry_request( void )
{
	static EFI_GUID		gv = EFI_GLOBAL_VARIABLE_GUID;
	static UINT8		opt[512], old[512];
	static UINT16		order[256];
	EFI_RUNTIME_SERVICES	*rt = (EFI_RUNTIME_SERVICES *)ST->RuntimeServices;
	EFI_FILE_PROTOCOL	*f;
	HARDDRIVE_DEVICE_PATH	*hd;
	EFI_DEVICE_PATH_PROTOCOL *fp, *end;
	CHAR16			var[9];
	UINT8			*req;
	char			s[REQ_MAX], path[128], uq[40];
	UINT64			part, start, size;
	UINTN			n = 0, i, plen, dpl, olen, sz, norder = 0;
	UINT32			attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS
				     | EFI_VARIABLE_RUNTIME_ACCESS, num = 0x10000, a;
	int			first;
	EFI_STATUS		st;

	if ( ts_dir == 0 || rt == 0 || rt->SetVariable == 0 || rt->GetVariable == 0 ) {
		return;
	}
	req = read_file(ts_dir, L"BOOTREG.TXT", &n);
	if ( req == 0 ) {
		return;
	}
	if ( !EFI_ERROR(ts_dir->Open(ts_dir, &f, L"BOOTREG.TXT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) ) {
		f->Delete(f);
	}
	if ( n > REQ_MAX - 1 ) n = REQ_MAX - 1;
	memcpy(s, req, n);
	s[n] = 0;
	part = req_num(s, n, "part=");
	start = req_num(s, n, "start=");
	size = req_num(s, n, "size=");
	first = ( req_num(s, n, "first=") == 1 );
	plen = req_str(s, n, "path=", path, sizeof(path));
	if ( req_str(s, n, "uniq=", uq, sizeof(uq)) != 32 || part == ~0ull || start == ~0ull || size == ~0ull
	  || plen == 0 || path[0] != '\\' ) {
		say("TessronOS loader: the boot entry request is not one that can be followed\n");
		return;
	}

	/* the load option: active, its device path's length, the name, and the path */
	memset(opt, 0, sizeof(opt));
	*(UINT32 *)opt = LOAD_OPTION_ACTIVE;
	{
		static const char	name[] = "TessronOS";
		CHAR16			*d = (CHAR16 *)( opt + 6 );

		for ( i = 0; name[i] != 0; i++ ) d[i] = (CHAR16)name[i];
		d[i] = 0;
		olen = 6 + ( i + 1 ) * 2;
	}
	hd = (HARDDRIVE_DEVICE_PATH *)( opt + olen );
	hd->Header.Type = DP_TYPE_MEDIA;
	hd->Header.SubType = DP_MEDIA_HARDDRIVE;
	hd->Header.Length[0] = (UINT8)sizeof(*hd);
	hd->PartitionNumber = (UINT32)part;
	hd->PartitionStart = start;
	hd->PartitionSize = size;
	for ( i = 0; i < 16; i++ ) {
		int	h1 = hex_val(uq[2 * i]), h2 = hex_val(uq[2 * i + 1]);

		if ( h1 < 0 || h2 < 0 ) return;
		hd->Signature[i] = (UINT8)( h1 * 16 + h2 );
	}
	hd->MBRType = 2;
	hd->SignatureType = 2;
	fp = (EFI_DEVICE_PATH_PROTOCOL *)( (UINT8 *)hd + sizeof(*hd) );
	fp->Type = DP_TYPE_MEDIA;
	fp->SubType = DP_MEDIA_FILEPATH;
	for ( i = 0; i < plen; i++ ) ( (CHAR16 *)( (UINT8 *)fp + 4 ) )[i] = (CHAR16)(UINT8)path[i];
	( (CHAR16 *)( (UINT8 *)fp + 4 ) )[plen] = 0;
	fp->Length[0] = (UINT8)( 4 + ( plen + 1 ) * 2 );
	fp->Length[1] = (UINT8)( ( 4 + ( plen + 1 ) * 2 ) >> 8 );
	end = (EFI_DEVICE_PATH_PROTOCOL *)( (UINT8 *)fp + 4 + ( plen + 1 ) * 2 );
	end->Type = DP_TYPE_END;
	end->SubType = 0xff;
	end->Length[0] = 4;
	dpl = sizeof(*hd) + 4 + ( plen + 1 ) * 2 + 4;
	*(UINT16 *)( opt + 4 ) = (UINT16)dpl;
	olen += dpl;

	/* the entry that names this already, else the first number no entry has */
	sz = sizeof(order);
	if ( EFI_ERROR(rt->GetVariable(L"BootOrder", &gv, &a, &sz, order)) ) sz = 0;
	norder = sz / 2;
	for ( i = 0; i < norder && num == 0x10000; i++ ) {
		UINTN	osz = sizeof(old), dl, at = 6;

		boot_name(var, order[i]);
		if ( EFI_ERROR(rt->GetVariable(var, &gv, &a, &osz, old)) || osz < 8 ) continue;
		dl = *(UINT16 *)( old + 4 );
		/* past the description, a string of CHAR16 to its 0: the device path */
		while ( at + 1 < osz && ( old[at] != 0 || old[at + 1] != 0 ) ) at += 2;
		at += 2;
		if ( dl == dpl && at + dl <= osz && memcmp(old + at, opt + olen - dpl, dpl) == 0 ) {
			num = order[i];
		}
	}
	for ( a = 0; num == 0x10000 && a < 0x10000; a++ ) {
		UINTN	osz = sizeof(old);

		boot_name(var, a);
		st = rt->GetVariable(var, &gv, 0, &osz, old);
		if ( st == EFI_NOT_FOUND ) num = a;
	}
	if ( num == 0x10000 ) {
		say("TessronOS loader: no number free for a boot entry\n");
		return;
	}
	boot_name(var, num);
	st = rt->SetVariable(var, &gv, attr, olen, opt);
	say("TessronOS loader: boot entry Boot%04x \"TessronOS\": partition %lu at %lu (%lu sectors), %s: 0x%lx\n",
	    num, part, start, size, path, (UINT64)st);
	if ( EFI_ERROR(st) ) {
		return;
	}

	/*
	 * In the order: first when asked; else where it is already (an entry
	 * in the order is never moved down: a system put there again keeps
	 * the place the person or an earlier request gave it), last when new.
	 * Then the next start from it.
	 */
	{
		UINTN	k = 0;
		UINT16	nord[257];
		int	there = 0;

		for ( i = 0; i < norder; i++ ) {
			if ( order[i] == num ) there = 1;
		}
		if ( first ) nord[k++] = (UINT16)num;
		for ( i = 0; i < norder; i++ ) {
			if ( order[i] != num || ( there && !first ) ) nord[k++] = order[i];
		}
		if ( !first && !there ) nord[k++] = (UINT16)num;
		st = rt->SetVariable(L"BootOrder", &gv, attr, k * 2, nord);
		say("TessronOS loader: BootOrder of %lu entries, TessronOS %s: 0x%lx\n", (UINT64)k,
		    first ? "first" : there ? "where it was" : "last", (UINT64)st);
		nord[0] = (UINT16)num;
		st = rt->SetVariable(L"BootNext", &gv, attr, 2, nord);
		say("TessronOS loader: BootNext Boot%04x: 0x%lx; the machine starts again\n", num, (UINT64)st);
	}
	write_logs();
	close_log_dirs();
	BS->Stall(500000);
	rt->ResetSystem(EfiResetCold, 0, 0, 0);
}

/* ------------------------------------------------------------------ ELF */

typedef struct {
	UINT8	e_ident[16];
	UINT16	e_type;
	UINT16	e_machine;
	UINT32	e_version;
	UINT64	e_entry;
	UINT64	e_phoff;
	UINT64	e_shoff;
	UINT32	e_flags;
	UINT16	e_ehsize;
	UINT16	e_phentsize;
	UINT16	e_phnum;
	UINT16	e_shentsize;
	UINT16	e_shnum;
	UINT16	e_shstrndx;
} ELF64_EHDR;

typedef struct {
	UINT32	p_type;
	UINT32	p_flags;
	UINT64	p_offset;
	UINT64	p_vaddr;
	UINT64	p_paddr;
	UINT64	p_filesz;
	UINT64	p_memsz;
	UINT64	p_align;
} ELF64_PHDR;

#define PT_LOAD		1
#define PF_X		1
#define PF_W		2
#define EM_X86_64	62

/* ------------------------------------------------------------------ the memory map */

static UINT32 bi_type( UINT32 efi )
{
	switch ( efi ) {
	case EfiLoaderCode:
	case EfiLoaderData:
	case EfiBootServicesCode:
	case EfiBootServicesData:
	case EfiConventionalMemory:
		return BI_MEM_FREE;
	case EfiACPIReclaimMemory:
		return BI_MEM_ACPI;
	case EfiACPIMemoryNVS:
		return BI_MEM_NVS;
	case EfiMemoryMappedIO:
	case EfiMemoryMappedIOPortSpace:
		return BI_MEM_MMIO;
	case EfiUnusableMemory:
		return BI_MEM_BAD;
	case MEM_TESSRON:
		return BI_MEM_KERNEL;
	default:
		return BI_MEM_RESERVED;
	}
}

/* RAM the firmware or the kernel uses: the linear map covers it */
static int is_ram( UINT32 efi )
{
	switch ( efi ) {
	case EfiLoaderCode:
	case EfiLoaderData:
	case EfiBootServicesCode:
	case EfiBootServicesData:
	case EfiRuntimeServicesCode:
	case EfiRuntimeServicesData:
	case EfiConventionalMemory:
	case EfiACPIReclaimMemory:
	case EfiACPIMemoryNVS:
	case MEM_TESSRON:
		return 1;
	default:
		return 0;
	}
}

static UINT8	*mmap_buf;
static UINTN	mmap_cap, mmap_size, mmap_key, mmap_dsize;

static void get_map( void )
{
	UINT32		ver;
	EFI_STATUS	st;

	mmap_size = mmap_cap;
	st = BS->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf, &mmap_key, &mmap_dsize, &ver);
	if ( EFI_ERROR(st) ) stop("cannot get the memory map", st);
}

static void alloc_map( void )
{
	UINTN		size = 0, key, dsize;
	UINT32		ver;
	EFI_STATUS	st;

	st = BS->GetMemoryMap(&size, 0, &key, &dsize, &ver);
	if ( st != EFI_BUFFER_TOO_SMALL ) stop("cannot size the memory map", st);
	/* room for what allocating this and the rest adds */
	mmap_cap = size + 256 * dsize;
	st = BS->AllocatePool(EfiLoaderData, mmap_cap, (void **)&mmap_buf);
	if ( EFI_ERROR(st) ) stop("out of memory for the memory map", st);
}

static UINT64 ram_top( void )
{
	UINT64	top = 0;
	UINTN	off;

	get_map();
	for ( off = 0; off + mmap_dsize <= mmap_size; off += mmap_dsize ) {
		EFI_MEMORY_DESCRIPTOR	*d = (EFI_MEMORY_DESCRIPTOR *)( mmap_buf + off );
		UINT64			e = d->PhysicalStart + d->NumberOfPages * EFI_PAGE_SIZE;

		if ( is_ram(d->Type) && e > top ) top = e;
	}
	return top;
}

/* After leaving the boot services: the firmware's map in the kernel's terms */
/* A piece of the converted map, joined to the last when it goes on from it with the same kind */
static void map_add( T_BOOTINFO *bi, UINT32 *pn, UINT64 s, UINT64 size, UINT32 t )
{
	UINT32	n = *pn;

	if ( size == 0 ) return;
	if ( n > 0 && bi->mem[n - 1].type == t && bi->mem[n - 1].start + bi->mem[n - 1].size == s ) {
		bi->mem[n - 1].size += size;
		return;
	}
	if ( n >= BI_MAX_MEM ) {
		return;			/* more kinds than room: the kernel sees what fits */
	}
	bi->mem[n].start = s;
	bi->mem[n].size = size;
	bi->mem[n].type = t;
	bi->mem[n].pad = 0;
	*pn = n + 1;
}

static void convert_map( T_BOOTINFO *bi )
{
	UINTN	off;
	UINT32	n = 0, k;

	for ( off = 0; off + mmap_dsize <= mmap_size; off += mmap_dsize ) {
		EFI_MEMORY_DESCRIPTOR	*d = (EFI_MEMORY_DESCRIPTOR *)( mmap_buf + off );
		UINT64			s = d->PhysicalStart, size = d->NumberOfPages * EFI_PAGE_SIZE;
		UINT64			e = s + size, cur = s;
		UINT32			t = bi_type(d->Type);

		if ( size == 0 ) continue;
		/* the pieces kept for the kernel, sorted, as its own; the rest as the firmware says */
		for ( k = 0; k < kept_n; k++ ) {
			UINT64	ks = kept_pa[k], ke = kept_pa[k] + kept_size[k];

			if ( ke <= cur || ks >= e ) continue;
			if ( ks > cur ) map_add(bi, &n, cur, ks - cur, t);
			if ( ks < cur ) ks = cur;
			if ( ke > e ) ke = e;
			map_add(bi, &n, ks, ke - ks, BI_MEM_KERNEL);
			cur = ke;
		}
		map_add(bi, &n, cur, e - cur, t);
	}
	bi->nmem = n;
}

/* ------------------------------------------------------------------ page tables */

static UINT64	*pt_pool;
static UINTN	pt_used, pt_cap;

static UINT64 *pt_new( void )
{
	UINT64	*p;

	if ( pt_used >= pt_cap ) stop("page tables: out of pool", 0);
	p = pt_pool + pt_used * 512;
	pt_used++;
	return p;
}

#define IDX(va, s)	( ( (va) >> (s) ) & 511 )

/* The table under entry e of tbl, made when there is none */
static UINT64 *pt_next( UINT64 *tbl, UINTN i )
{
	if ( ( tbl[i] & PG_P ) == 0 ) {
		tbl[i] = (UINT64)pt_new() | PG_TABLE;
	}
	return (UINT64 *)( tbl[i] & 0x000FFFFFFFFFF000ull );
}

/* [va, va + size) to [pa, ...) in large pages (1GB where allowed, else 2MB) */
static void map_large( UINT64 *pml4, UINT64 va, UINT64 pa, UINT64 size, UINT64 attr, int gb )
{
	UINT64	end = va + size;

	while ( va < end ) {
		UINT64	*pdpt = pt_next(pml4, IDX(va, 39));

		if ( gb && ( va & ( GB - 1 ) ) == 0 && ( pa & ( GB - 1 ) ) == 0 && end - va >= GB ) {
			pdpt[IDX(va, 30)] = pa | attr | PG_PS;
			va += GB;
			pa += GB;
		} else {
			UINT64	*pd = pt_next(pdpt, IDX(va, 30));

			pd[IDX(va, 21)] = pa | attr | PG_PS;
			va += MB2;
			pa += MB2;
		}
	}
}

/* 4KB pages */
static void map_pages( UINT64 *pml4, UINT64 va, UINT64 pa, UINT64 size, UINT64 attr )
{
	UINT64	end = va + size;

	for ( ; va < end; va += EFI_PAGE_SIZE, pa += EFI_PAGE_SIZE ) {
		UINT64	*pdpt = pt_next(pml4, IDX(va, 39));
		UINT64	*pd = pt_next(pdpt, IDX(va, 30));
		UINT64	*pt = pt_next(pd, IDX(va, 21));

		pt[IDX(va, 12)] = pa | attr;
	}
}

/* ------------------------------------------------------------------ the screen */

static UINT32 parse_uint( const char **pp )
{
	const char	*p = *pp;
	UINT32		v = 0;

	while ( *p >= '0' && *p <= '9' ) v = v * 10 + (UINT32)( *p++ - '0' );
	*pp = p;
	return v;
}

/* "video=WxH" of the start-up settings, when there */
static void want_video( const char *cmd, UINT32 *w, UINT32 *h )
{
	const char	*p;

	*w = *h = 0;
	for ( p = cmd; *p != '\0'; p++ ) {
		if ( p[0] == 'v' && p[1] == 'i' && p[2] == 'd' && p[3] == 'e' && p[4] == 'o' && p[5] == '=' ) {
			p += 6;
			*w = parse_uint(&p);
			if ( *p == 'x' ) {
				p++;
				*h = parse_uint(&p);
			}
			return;
		}
	}
}

static EFI_GRAPHICS_OUTPUT_PROTOCOL	*Gop = 0;

static int model_video( T_BOOTINFO *bi, UINT32 *w, UINT32 *h );

/*
 * The mode the kernel draws in: the one the settings name, else the one
 * the machine is known to want (model_video), else the largest that fits
 * 1920x1280 (the panel of the Surface Go 2), of the modes whose pixels
 * can be written directly
 */
static void setup_screen( T_BOOTINFO *bi )
{
	EFI_GUID				guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GRAPHICS_OUTPUT_PROTOCOL		*gop;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION	*info;
	EFI_STATUS				st;
	UINT32					m, best = 0xffffffff, ww, wh;
	UINT64					best_area = 0;
	UINTN					sz;
	int					by_model = 0;

	st = BS->LocateProtocol(&guid, 0, (void **)&gop);
	if ( EFI_ERROR(st) ) {
		say("TessronOS loader: no graphics output\n");
		return;
	}
	Gop = gop;
	want_video(bi->cmdline, &ww, &wh);
	if ( ww == 0 ) by_model = model_video(bi, &ww, &wh);
again:
	for ( m = 0; m < gop->Mode->MaxMode; m++ ) {
		UINT64	area;

		if ( EFI_ERROR(gop->QueryMode(gop, m, &sz, &info)) ) continue;
		if ( info->PixelFormat == PixelBltOnly ) continue;
		if ( ww != 0 ) {
			if ( info->HorizontalResolution == ww && info->VerticalResolution == wh ) {
				best = m;
				break;
			}
			continue;
		}
		if ( info->HorizontalResolution > 1920 || info->VerticalResolution > 1280 ) continue;
		area = (UINT64)info->HorizontalResolution * info->VerticalResolution;
		if ( area > best_area ) {
			best_area = area;
			best = m;
		}
	}
	if ( best == 0xffffffff && by_model ) {
		by_model = 0;			/* the model's mode is not offered: the usual choice */
		ww = wh = 0;
		goto again;
	}
	if ( has(bi->cmdline, "keepmode=1") ) {
		best = 0xffffffff;		/* the mode the firmware set, left as it is */
	}
	if ( best != 0xffffffff && best != gop->Mode->Mode ) {
		st = gop->SetMode(gop, best);
		if ( EFI_ERROR(st) ) say("TessronOS loader: the screen mode was not set\n");
	}
	info = gop->Mode->Info;
	if ( info->PixelFormat == PixelBltOnly ) {
		say("TessronOS loader: the screen cannot be written directly\n");
		return;
	}
	bi->fb.pa = gop->Mode->FrameBufferBase;
	bi->fb.size = gop->Mode->FrameBufferSize;
	bi->fb.width = info->HorizontalResolution;
	bi->fb.height = info->VerticalResolution;
	bi->fb.stride = info->PixelsPerScanLine * 4;
	bi->fb.format = ( info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ) ? BI_FB_BGRX
		      : ( info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ) ? BI_FB_RGBX
		      : BI_FB_MASK;
	bi->fb.mask_r = info->PixelInformation.RedMask;
	bi->fb.mask_g = info->PixelInformation.GreenMask;
	bi->fb.mask_b = info->PixelInformation.BlueMask;
	bi->fb.mask_x = info->PixelInformation.ReservedMask;

	/* from here on what the loader says is drawn on this screen */
	con_start(&bi->fb);
	stage("the screen");
	say("TessronOS loader (log so far in \\EFI\\TESSRON\\LOADER.TXT)\n");
	say("TessronOS loader: screen %ux%u (mode %u of %u) at 0x%lx, %u bytes a row, format %u\n",
	    bi->fb.width, bi->fb.height, gop->Mode->Mode, gop->Mode->MaxMode, bi->fb.pa, bi->fb.stride,
	    bi->fb.format);
	if ( by_model ) say("TessronOS loader: %ux%u, the mode this machine is given unless video= says\n", ww, wh);
	mark_start();
}

/* ------------------------------------------------------------------ the firmware's tables */

static void find_tables( T_BOOTINFO *bi )
{
	EFI_GUID	acpi20 = EFI_ACPI_20_TABLE_GUID, acpi10 = EFI_ACPI_10_TABLE_GUID;
	EFI_GUID	smb = EFI_SMBIOS_TABLE_GUID, smb3 = EFI_SMBIOS3_TABLE_GUID;
	UINTN		i;

	for ( i = 0; i < ST->NumberOfTableEntries; i++ ) {
		EFI_CONFIGURATION_TABLE	*t = &ST->ConfigurationTable[i];
		UINT64			a = (UINT64)t->VendorTable;

		if ( guid_eq(&t->VendorGuid, &acpi20) ) {
			bi->rsdp_pa = a;
		} else if ( guid_eq(&t->VendorGuid, &acpi10) && bi->rsdp_pa == 0 ) {
			bi->rsdp_pa = a;
		} else if ( guid_eq(&t->VendorGuid, &smb) ) {
			bi->smbios_pa = a;
		} else if ( guid_eq(&t->VendorGuid, &smb3) ) {
			bi->smbios3_pa = a;
		}
	}
}

static void get_seed( T_BOOTINFO *bi )
{
	EFI_GUID		guid = EFI_RNG_PROTOCOL_GUID;
	EFI_RNG_PROTOCOL	*rng;

	if ( EFI_ERROR(BS->LocateProtocol(&guid, 0, (void **)&rng)) ) return;
	if ( !EFI_ERROR(rng->GetRNG(rng, 0, BI_RNG_LEN, bi->rng)) ) {
		bi->rng_len = BI_RNG_LEN;
	}
}

/*
 * The firmware's clock: its time said in LOADER.TXT, and the zone it
 * gives the CMOS clock handed over when it gives one (most PCs answer
 * EFI_UNSPECIFIED_TIMEZONE: the time is a local time, of a zone not
 * said, and the kernel keeps to the setting in the calendar).
 */
static void get_clock( T_BOOTINFO *bi )
{
	EFI_RUNTIME_SERVICES	*rt = (EFI_RUNTIME_SERVICES *)ST->RuntimeServices;
	EFI_TIME		t;
	EFI_STATUS		s;

	memset(&t, 0, sizeof(t));
	if ( rt == 0 || rt->GetTime == 0 ) return;
	s = rt->GetTime(&t, 0);
	if ( EFI_ERROR(s) ) {
		say("[loader] firmware clock: GetTime refused (0x%lx)\n", (UINT64)s);
		return;
	}
	say("[loader] firmware clock %04u-%02u-%02u %02u:%02u:%02u, TimeZone ", (UINT32)t.Year,
	    (UINT32)t.Month, (UINT32)t.Day, (UINT32)t.Hour, (UINT32)t.Minute, (UINT32)t.Second);
	if ( t.TimeZone >= -1440 && t.TimeZone <= 1440 ) {
		say("%d min (UTC = the clock + TimeZone), Daylight %u\n", (int)t.TimeZone, (UINT32)t.Daylight);
		bi->rtc_tz = t.TimeZone;
		bi->flags |= BI_F_RTC_TZ;
	} else {
		say("unspecified (0x%x), Daylight %u\n", (UINT32)(UINT16)t.TimeZone, (UINT32)t.Daylight);
	}
}

/* ------------------------------------------------------------------ the report of the machine */

static TBUF	rpt;

static UINT32 rd32( const void *p )
{
	const UINT8	*b = p;

	return (UINT32)b[0] | ( (UINT32)b[1] << 8 ) | ( (UINT32)b[2] << 16 ) | ( (UINT32)b[3] << 24 );
}

static UINT64 rd64( const void *p )
{
	return (UINT64)rd32(p) | ( (UINT64)rd32((const UINT8 *)p + 4) << 32 );
}

static void rpt_hex( const UINT8 *p, UINTN n, const char *indent )
{
	UINTN	i;

	for ( i = 0; i < n; i++ ) {
		if ( ( i & 15 ) == 0 ) tb_fmt(&rpt, "%s%04x:", indent, (UINT32)i);
		tb_fmt(&rpt, " %02x", p[i]);
		if ( ( i & 15 ) == 15 || i + 1 == n ) tb_putc(&rpt, '\n');
	}
}

static void rpt_cpu( void )
{
	UINT32	a, b, c, d, max, emax, l, s;
	UINT32	f1c = 0, f1d = 0, f7b = 0, f7c = 0, f7d = 0, e1d = 0, e7d = 0, xsz = 0, xmax = 0;
	char	brand[49];

	tb_fmt(&rpt, "\n[cpuid] leaf.sub: eax ebx ecx edx\n");
	cpuid(0, &max, &b, &c, &d);
	for ( l = 0; l <= max && l < 0x40; l++ ) {
		for ( s = 0; s < 16; s++ ) {
			cpuid2(l, s, &a, &b, &c, &d);
			if ( s > 0 && a == 0 && b == 0 && c == 0 && d == 0 ) break;
			tb_fmt(&rpt, "  %08x.%u: %08x %08x %08x %08x\n", l, s, a, b, c, d);
			if ( l == 1 ) {
				f1c = c;
				f1d = d;
			}
			if ( l == 7 && s == 0 ) {
				f7b = b;
				f7c = c;
				f7d = d;
			}
			if ( l == 0xd && s == 0 ) {
				xsz = b;
				xmax = c;
			}
			/* only the leaves that have sub-leaves are asked more than once */
			if ( l != 4 && l != 7 && l != 0xb && l != 0xd && l != 0xf && l != 0x10 && l != 0x12
			  && l != 0x14 && l != 0x17 && l != 0x18 && l != 0x1f ) break;
		}
	}
	cpuid(0x80000000, &emax, &b, &c, &d);
	for ( l = 0x80000000; l <= emax && l < 0x80000040; l++ ) {
		cpuid(l, &a, &b, &c, &d);
		tb_fmt(&rpt, "  %08x.0: %08x %08x %08x %08x\n", l, a, b, c, d);
		if ( l == 0x80000001 ) e1d = d;
		if ( l == 0x80000007 ) e7d = d;
	}
	memset(brand, 0, sizeof(brand));
	if ( emax >= 0x80000004 ) {
		for ( l = 0; l < 3; l++ ) {
			cpuid(0x80000002 + l, &a, &b, &c, &d);
			memcpy(brand + l * 16, &a, 4);
			memcpy(brand + l * 16 + 4, &b, 4);
			memcpy(brand + l * 16 + 8, &c, 4);
			memcpy(brand + l * 16 + 12, &d, 4);
		}
	}
	tb_fmt(&rpt, "[cpu] \"%s\"\n", brand);
	tb_fmt(&rpt, "  SSE4.2 %u POPCNT %u AVX %u AVX2 %u XSAVE %u OSXSAVE %u XSAVE area %u (largest %u)\n",
	       ( f1c >> 20 ) & 1, ( f1c >> 23 ) & 1, ( f1c >> 28 ) & 1, ( f7b >> 5 ) & 1, ( f1c >> 26 ) & 1,
	       ( f1c >> 27 ) & 1, xsz, xmax);
	tb_fmt(&rpt, "  TSC deadline %u invariant TSC %u x2APIC %u PCID %u INVPCID %u FSGSBASE %u SMEP %u SMAP %u\n",
	       ( f1c >> 24 ) & 1, ( e7d >> 8 ) & 1, ( f1c >> 21 ) & 1, ( f1c >> 17 ) & 1, ( f7b >> 10 ) & 1,
	       f7b & 1, ( f7b >> 7 ) & 1, ( f7b >> 20 ) & 1);
	tb_fmt(&rpt, "  RDRAND %u RDSEED %u 1GB pages %u NX %u UMIP %u PKU %u MD_CLEAR %u HWP %u APIC %u\n",
	       ( f1c >> 30 ) & 1, ( f7b >> 18 ) & 1, ( e1d >> 26 ) & 1, ( e1d >> 20 ) & 1, ( f7c >> 2 ) & 1,
	       ( f7c >> 3 ) & 1, ( f7d >> 10 ) & 1, 0u, ( f1d >> 9 ) & 1);
	if ( max >= 6 ) {
		cpuid(6, &a, &b, &c, &d);
		tb_fmt(&rpt, "  leaf 6: HWP %u, digital temperature %u, turbo %u\n", ( a >> 7 ) & 1, a & 1, ( a >> 1 ) & 1);
	}
	say("  processor: %s; AVX2 %u, TSC deadline %u, x2APIC %u, PCID %u\n", brand, ( f7b >> 5 ) & 1,
	    ( f1c >> 24 ) & 1, ( f1c >> 21 ) & 1, ( f1c >> 17 ) & 1);
}

static void rpt_gop( void )
{
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION	*info;
	UINTN					sz;
	UINT32					m;

	tb_fmt(&rpt, "\n[screen modes] (format 0 RGBX, 1 BGRX, 2 masks, 3 blt only)\n");
	if ( Gop == 0 ) {
		tb_fmt(&rpt, "  no graphics output\n");
		return;
	}
	for ( m = 0; m < Gop->Mode->MaxMode; m++ ) {
		if ( EFI_ERROR(Gop->QueryMode(Gop, m, &sz, &info)) ) continue;
		tb_fmt(&rpt, "  mode %u: %ux%u format %u, %u pixels a row%s\n", m, info->HorizontalResolution,
		       info->VerticalResolution, (UINT32)info->PixelFormat, info->PixelsPerScanLine,
		       ( m == Gop->Mode->Mode ) ? " (in use)" : "");
	}
	tb_fmt(&rpt, "  frame buffer 0x%lx, %lu bytes\n", Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);
}

static void rpt_map( void )
{
	UINTN	off;
	UINT64	free = 0;

	get_map();
	tb_fmt(&rpt, "\n[memory map] type start pages attributes\n");
	for ( off = 0; off + mmap_dsize <= mmap_size; off += mmap_dsize ) {
		EFI_MEMORY_DESCRIPTOR	*d = (EFI_MEMORY_DESCRIPTOR *)( mmap_buf + off );

		tb_fmt(&rpt, "  %2u %016lx %8lu %016lx\n", d->Type, d->PhysicalStart, d->NumberOfPages, d->Attribute);
		if ( bi_type(d->Type) == BI_MEM_FREE ) free += d->NumberOfPages;
	}
	say("  memory: %lu MB free for the kernel\n", free >> 8);
}

/* Each ACPI table as it is, to a file of its signature (SSDT1.BIN, SSDT2.BIN, ...) */
static UINT32	acpi_nsig = 0;
static struct {
	char	sig[4];
	UINT32	n;
} acpi_sigs[64];

static void dump_table( UINT64 pa, const char *why )
{
	const UINT8	*t = (const UINT8 *)pa;
	UINT32		len, i, n = 0;
	CHAR16		name[16];
	int		k = 0, multi;

	if ( pa == 0 ) return;
	len = ( t[0] == 'F' && t[1] == 'A' && t[2] == 'C' && t[3] == 'S' ) ? rd32(t + 4) : rd32(t + 4);
	if ( len < 8 || len > 16 * 1024 * 1024 ) {
		tb_fmt(&rpt, "  %s at 0x%lx: a length of %u, not read\n", why, pa, len);
		return;
	}
	for ( i = 0; i < acpi_nsig; i++ ) {
		if ( acpi_sigs[i].sig[0] == t[0] && acpi_sigs[i].sig[1] == t[1] && acpi_sigs[i].sig[2] == t[2]
		  && acpi_sigs[i].sig[3] == t[3] ) {
			n = ++acpi_sigs[i].n;
			break;
		}
	}
	if ( i == acpi_nsig && acpi_nsig < 64 ) {
		memcpy(acpi_sigs[acpi_nsig].sig, t, 4);
		acpi_sigs[acpi_nsig++].n = n = 1;
	}
	multi = ( t[0] == 'S' && t[1] == 'S' && t[2] == 'D' && t[3] == 'T' ) || n > 1;
	for ( i = 0; i < 4; i++ ) {
		UINT8	c = t[i];

		name[k++] = ( ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ) ? c : '_';
	}
	if ( multi ) {
		if ( n >= 10 ) name[k++] = (CHAR16)( '0' + n / 10 );
		name[k++] = (CHAR16)( '0' + n % 10 );
	}
	name[k++] = '.';
	name[k++] = 'B';
	name[k++] = 'I';
	name[k++] = 'N';
	name[k] = 0;
	if ( t[0] == 'F' && t[1] == 'A' && t[2] == 'C' && t[3] == 'S' ) {
		tb_fmt(&rpt, "  FACS at 0x%lx, %u bytes\n", pa, len);
	} else {
		char	oem[7], oemt[9];

		memcpy(oem, t + 10, 6);
		oem[6] = '\0';
		memcpy(oemt, t + 16, 8);
		oemt[8] = '\0';
		for ( i = 0; i < 6; i++ ) if ( oem[i] < 0x20 || oem[i] > 0x7e ) oem[i] = '.';
		for ( i = 0; i < 8; i++ ) if ( oemt[i] < 0x20 || oemt[i] > 0x7e ) oemt[i] = '.';
		tb_fmt(&rpt, "  %c%c%c%c at 0x%lx, %u bytes, revision %u, \"%s\" \"%s\" %08x\n",
		       t[0], t[1], t[2], t[3], pa, len, t[8], oem, oemt, rd32(t + 24));
	}
	if ( !write_log(1, name, t, len) ) tb_fmt(&rpt, "    (not written)\n");
}

static UINT64	mcfg_base[8];
static UINT32	mcfg_bus0[8], mcfg_bus1[8], mcfg_seg[8], mcfg_n = 0;

static void rpt_acpi( T_BOOTINFO *bi )
{
	const UINT8	*rsdp = (const UINT8 *)bi->rsdp_pa, *x;
	UINT32		i, n, esz;
	UINT64		root;

	tb_fmt(&rpt, "\n[acpi] (the tables are in \\EFI\\TESSRON\\ACPI)\n");
	if ( rsdp == 0 ) {
		tb_fmt(&rpt, "  no RSDP\n");
		say("  ACPI: no RSDP\n");
		return;
	}
	tb_fmt(&rpt, "  RSDP at 0x%lx, revision %u\n", bi->rsdp_pa, rsdp[15]);
	if ( rsdp[15] >= 2 && rd64(rsdp + 24) != 0 ) {
		root = rd64(rsdp + 24);
		esz = 8;
	} else {
		root = rd32(rsdp + 16);
		esz = 4;
	}
	x = (const UINT8 *)root;
	dump_table(root, "the root table");
	n = ( rd32(x + 4) - 36 ) / esz;
	for ( i = 0; i < n; i++ ) {
		UINT64		pa = ( esz == 8 ) ? rd64(x + 36 + i * 8) : rd32(x + 36 + i * 4);
		const UINT8	*t = (const UINT8 *)pa;

		if ( pa == 0 ) continue;
		dump_table(pa, "a table");
		if ( t[0] == 'F' && t[1] == 'A' && t[2] == 'C' && t[3] == 'P' ) {
			UINT32	flen = rd32(t + 4);
			UINT64	dsdt = ( flen >= 148 && rd64(t + 140) != 0 ) ? rd64(t + 140) : rd32(t + 40);
			UINT64	facs = ( flen >= 140 && rd64(t + 132) != 0 ) ? rd64(t + 132) : rd32(t + 36);

			dump_table(dsdt, "DSDT");
			dump_table(facs, "FACS");
			tb_fmt(&rpt, "    FADT flags %08x, reset register space %u at 0x%lx value %02x, PM1a %x, "
			       "SCI %u, boot architecture %04x\n", rd32(t + 112), t[116], rd64(t + 120), t[128],
			       rd32(t + 64), (UINT32)( t[46] | ( t[47] << 8 ) ), (UINT32)( t[109] | ( t[110] << 8 ) ));
		}
		if ( t[0] == 'M' && t[1] == 'C' && t[2] == 'F' && t[3] == 'G' ) {
			UINT32	off;

			for ( off = 44; off + 16 <= rd32(t + 4) && mcfg_n < 8; off += 16 ) {
				mcfg_base[mcfg_n] = rd64(t + off);
				mcfg_seg[mcfg_n] = (UINT32)( t[off + 8] | ( t[off + 9] << 8 ) );
				mcfg_bus0[mcfg_n] = t[off + 10];
				mcfg_bus1[mcfg_n] = t[off + 11];
				mcfg_n++;
			}
		}
	}
	say("  ACPI: %u tables written to \\EFI\\TESSRON\\ACPI\n", n);
}

/* The strings after an SMBIOS structure: the i-th, 1 the first */
static const char *smb_str( const UINT8 *s, UINT8 i )
{
	const char	*p = (const char *)s + s[1];

	if ( i == 0 ) return "";
	while ( --i > 0 && *p != '\0' ) {
		while ( *p != '\0' ) p++;
		p++;
	}
	return p;
}

/* The system's (type 1) structure in SMBIOS; 0 if there is none */
static const UINT8 *smb_system( T_BOOTINFO *bi )
{
	const UINT8	*e, *p, *end;
	UINT64		tbl;
	UINT32		len;

	if ( bi->smbios3_pa != 0 ) {
		e = (const UINT8 *)bi->smbios3_pa;
		tbl = rd64(e + 16);
		len = rd32(e + 12);
	} else if ( bi->smbios_pa != 0 ) {
		e = (const UINT8 *)bi->smbios_pa;
		tbl = rd32(e + 0x18);
		len = (UINT32)( e[0x16] | ( e[0x17] << 8 ) );
	} else {
		return 0;
	}
	if ( len > 1024 * 1024 ) len = 1024 * 1024;
	p = (const UINT8 *)tbl;
	end = p + len;
	while ( p + 4 <= end && p[0] != 127 ) {
		const UINT8	*q = p + p[1];

		if ( p[1] < 4 ) break;
		if ( p[0] == 1 && p[1] >= 8 ) return p;
		while ( q + 1 < end && ( q[0] != 0 || q[1] != 0 ) ) q++;
		p = q + 2;
	}
	return 0;
}

static int starts( const char *s, const char *pre )
{
	while ( *pre != '\0' ) {
		if ( *s++ != *pre++ ) return 0;
	}
	return 1;
}

/*
 * The screen mode a known machine is started in when video= is not
 * given. The ThinkPad X1 Yoga of the 3rd generation (machine types
 * 20LD-20LG) has a 2560x1440 panel and its firmware offers 1600x1200,
 * stretched over the panel: chosen so that text and the desktop are
 * not drawn too small
 */
static int model_video( T_BOOTINFO *bi, UINT32 *w, UINT32 *h )
{
	const UINT8	*p = smb_system(bi);
	const char	*maker, *prod;

	if ( p == 0 ) return 0;
	maker = smb_str(p, p[4]);
	prod = smb_str(p, p[5]);
	if ( starts(maker, "LENOVO")
	  && ( starts(prod, "20LD") || starts(prod, "20LE") || starts(prod, "20LF") || starts(prod, "20LG") ) ) {
		*w = 1600;
		*h = 1200;
		return 1;
	}
	return 0;
}

static void rpt_smbios( T_BOOTINFO *bi )
{
	const UINT8	*e, *p, *end;
	UINT64		tbl;
	UINT32		len;

	tb_fmt(&rpt, "\n[smbios]\n");
	if ( bi->smbios3_pa != 0 ) {
		e = (const UINT8 *)bi->smbios3_pa;
		tbl = rd64(e + 16);
		len = rd32(e + 12);
	} else if ( bi->smbios_pa != 0 ) {
		e = (const UINT8 *)bi->smbios_pa;
		tbl = rd32(e + 0x18);
		len = (UINT32)( e[0x16] | ( e[0x17] << 8 ) );
	} else {
		tb_fmt(&rpt, "  none\n");
		return;
	}
	if ( len > 1024 * 1024 ) len = 1024 * 1024;
	(void)write_log(0, L"SMBIOS.BIN", (const void *)tbl, len);
	p = (const UINT8 *)tbl;
	end = p + len;
	while ( p + 4 <= end && p[0] != 127 ) {
		const UINT8	*q = p + p[1];

		if ( p[1] < 4 ) break;
		if ( p[0] == 0 ) {
			tb_fmt(&rpt, "  firmware: \"%s\" \"%s\" \"%s\"\n", smb_str(p, p[4]), smb_str(p, p[5]), smb_str(p, p[8]));
		} else if ( p[0] == 1 ) {
			tb_fmt(&rpt, "  system: \"%s\" \"%s\" \"%s\" SKU \"%s\" family \"%s\"\n", smb_str(p, p[4]),
			       smb_str(p, p[5]), smb_str(p, p[6]), ( p[1] > 0x19 ) ? smb_str(p, p[0x19]) : "",
			       ( p[1] > 0x1a ) ? smb_str(p, p[0x1a]) : "");
			say("  machine: %s %s (%s)\n", smb_str(p, p[4]), smb_str(p, p[5]),
			    ( p[1] > 0x19 ) ? smb_str(p, p[0x19]) : "");
		} else if ( p[0] == 2 ) {
			tb_fmt(&rpt, "  board: \"%s\" \"%s\" \"%s\"\n", smb_str(p, p[4]), smb_str(p, p[5]), smb_str(p, p[6]));
		} else if ( p[0] == 17 && p[1] >= 0x15 ) {
			tb_fmt(&rpt, "  memory device: %u MB, \"%s\" \"%s\"\n", (UINT32)( p[0x0c] | ( p[0x0d] << 8 ) ),
			       smb_str(p, p[0x10]), ( p[1] > 0x17 ) ? smb_str(p, p[0x17]) : "");
		}
		/* past the strings: to the double zero */
		while ( q + 1 < end && ( q[0] != 0 || q[1] != 0 ) ) q++;
		p = q + 2;
	}
}

static const char *pci_class( UINT32 cls, UINT32 sub )
{
	switch ( ( cls << 8 ) | sub ) {
	case 0x0106: return "SATA";
	case 0x0108: return "NVMe";
	case 0x0200: return "Ethernet";
	case 0x0280: return "network (Wi-Fi)";
	case 0x0300: return "VGA";
	case 0x0380: return "display";
	case 0x0401: return "audio";
	case 0x0403: return "HD audio";
	case 0x0480: return "multimedia (camera)";
	case 0x0600: return "host bridge";
	case 0x0601: return "ISA bridge";
	case 0x0604: return "PCI bridge";
	case 0x0805: return "SD host";
	case 0x0880: return "system (LPSS, GPIO...)";
	case 0x0c03: return "USB";
	case 0x0c05: return "SMBus";
	case 0x0c80: return "serial bus (I2C, SPI)";
	case 0x1180: return "signal processing (thermal)";
	case 0x0780: return "communication (ME, UART)";
	case 0x0700: return "serial";
	default: return "";
	}
}

/* Every function of the PCI Express configuration space, as the firmware left it (nothing is written) */
static void rpt_pci( void )
{
	UINT32	i, bus, dev, fn, nfn = 0;

	tb_fmt(&rpt, "\n[pci] seg:bus:dev.fn vendor:device class.sub.if rev header subsystem; BARs; capabilities\n");
	for ( i = 0; i < mcfg_n; i++ ) {
		tb_fmt(&rpt, "  ECAM 0x%lx segment %u buses %u-%u\n", mcfg_base[i], mcfg_seg[i], mcfg_bus0[i], mcfg_bus1[i]);
		for ( bus = mcfg_bus0[i]; bus <= mcfg_bus1[i]; bus++ ) {
			for ( dev = 0; dev < 32; dev++ ) {
				for ( fn = 0; fn < 8; fn++ ) {
					volatile UINT32	*c = (volatile UINT32 *)( mcfg_base[i]
							     + ( ( (UINT64)( bus - mcfg_bus0[i] ) << 20 ) | ( dev << 15 ) | ( fn << 12 ) ) );
					UINT32		id = c[0], cl, hdr, b, nb, cap, k = 0;

					if ( ( id & 0xffff ) == 0xffff || id == 0 ) {
						if ( fn == 0 ) break;
						continue;
					}
					cl = c[2];
					hdr = ( c[3] >> 16 ) & 0xff;
					nfn++;
					tb_fmt(&rpt, "  %04x:%02x:%02x.%u %04x:%04x %02x.%02x.%02x rev %02x hdr %02x",
					       mcfg_seg[i], bus, dev, fn, id & 0xffff, id >> 16, cl >> 24, ( cl >> 16 ) & 0xff,
					       ( cl >> 8 ) & 0xff, cl & 0xff, hdr);
					if ( ( hdr & 0x7f ) == 0 ) {
						tb_fmt(&rpt, " sub %04x:%04x", c[11] & 0xffff, c[11] >> 16);
					} else if ( ( hdr & 0x7f ) == 1 ) {
						tb_fmt(&rpt, " buses %u-%u", ( c[6] >> 8 ) & 0xff, ( c[6] >> 16 ) & 0xff);
					}
					tb_fmt(&rpt, " %s\n    BAR", pci_class(cl >> 24, ( cl >> 16 ) & 0xff));
					nb = ( ( hdr & 0x7f ) == 0 ) ? 6 : ( ( hdr & 0x7f ) == 1 ) ? 2 : 0;
					for ( b = 0; b < nb; b++ ) {
						UINT32	v = c[4 + b];

						if ( ( v & 7 ) == 4 && b + 1 < nb ) {
							tb_fmt(&rpt, " %u:%08x%08x", b, c[5 + b], v & ~0xfu);
							b++;
						} else if ( v != 0 ) {
							tb_fmt(&rpt, " %u:%08x", b, v);
						}
					}
					tb_fmt(&rpt, "; irq %u pin %u; caps", c[15] & 0xff, ( c[15] >> 8 ) & 0xff);
					cap = ( ( c[1] >> 16 ) & 0x10 ) ? ( c[13] & 0xfc ) : 0;
					while ( cap >= 0x40 && k++ < 48 ) {
						UINT32	w = c[cap / 4];

						tb_fmt(&rpt, " %02x", w & 0xff);
						cap = ( w >> 8 ) & 0xfc;
					}
					tb_putc(&rpt, '\n');
					if ( fn == 0 && ( hdr & 0x80 ) == 0 ) break;
				}
			}
		}
	}
	say("  PCI: %u functions\n", nfn);
}

/* The USB devices the firmware enumerated, every configuration of each, as their descriptors say */
static void rpt_usb_strings( EFI_USB_IO_PROTOCOL *u, UINT8 i, const char *what )
{
	CHAR16	*s = 0;
	char	b[128];
	int	k;

	if ( i == 0 || EFI_ERROR(u->UsbGetStringDescriptor(u, 0x0409, i, &s)) || s == 0 ) return;
	for ( k = 0; k < 127 && s[k] != 0; k++ ) b[k] = ( s[k] >= 0x20 && s[k] < 0x7f ) ? (char)s[k] : '?';
	b[k] = '\0';
	tb_fmt(&rpt, " %s \"%s\"", what, b);
	BS->FreePool(s);
}

static void rpt_config( const UINT8 *c, UINT32 total )
{
	UINT32	off = 0;

	while ( off + 2 <= total && c[off] >= 2 && off + c[off] <= total ) {
		const UINT8	*d = c + off;

		switch ( d[1] ) {
		case 2:
			tb_fmt(&rpt, "      configuration %u: %u interfaces, attributes %02x, %u mA\n", d[5], d[4], d[7], d[8] * 2);
			break;
		case 4:
			tb_fmt(&rpt, "        interface %u alt %u: class %02x.%02x.%02x, %u endpoints\n",
			       d[2], d[3], d[5], d[6], d[7], d[4]);
			break;
		case 5:
			tb_fmt(&rpt, "          endpoint %02x: %s, %u bytes, interval %u\n", d[2],
			       ( ( d[3] & 3 ) == 0 ) ? "control" : ( ( d[3] & 3 ) == 1 ) ? "isochronous"
			       : ( ( d[3] & 3 ) == 2 ) ? "bulk" : "interrupt",
			       (UINT32)( d[4] | ( d[5] << 8 ) ) & 0x7ff, d[6]);
			break;
		case 0x21:
			tb_fmt(&rpt, "          HID: report descriptor %u bytes\n", (UINT32)( d[7] | ( d[8] << 8 ) ));
			break;
		case 0x24:
			tb_fmt(&rpt, "          class-specific %02x (%u bytes)\n", d[2], d[0]);
			break;
		case 0x0b:
			tb_fmt(&rpt, "        association: interfaces %u-%u class %02x.%02x\n", d[2], d[2] + d[3] - 1, d[4], d[5]);
			break;
		default:
			break;
		}
		off += d[0];
	}
}

static void rpt_usb( const char *cmd )
{
	EFI_GUID		guid = EFI_USB_IO_PROTOCOL_GUID;
	EFI_HANDLE		*h = 0;
	UINTN			n = 0, i;
	UINT8			*buf;

	tb_fmt(&rpt, "\n[usb] as the firmware enumerated them: one entry for each interface it gave a handle\n");
	/* the firmware may have left devices nobody asked for unconnected: every controller connected, all the way down */
	if ( !has(cmd, "connect=0") && !EFI_ERROR(BS->LocateHandleBuffer(AllHandles, 0, 0, &n, &h)) ) {
		for ( i = 0; i < n; i++ ) (void)BS->ConnectController(h[i], 0, 0, 1);
		BS->FreePool(h);
		h = 0;
		n = 0;
	}
	if ( EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &guid, 0, &n, &h)) ) {
		tb_fmt(&rpt, "  none\n");
		say("  USB: none enumerated by the firmware\n");
		return;
	}
	buf = alloc_pool(4096);
	for ( i = 0; i < n; i++ ) {
		EFI_USB_IO_PROTOCOL		*u;
		EFI_USB_DEVICE_DESCRIPTOR	dd;
		EFI_USB_INTERFACE_DESCRIPTOR	id;
		UINT32				cf, st;

		if ( EFI_ERROR(BS->HandleProtocol(h[i], &guid, (void **)&u)) ) continue;
		if ( EFI_ERROR(u->UsbGetDeviceDescriptor(u, &dd)) ) continue;
		memset(&id, 0, sizeof(id));
		(void)u->UsbGetInterfaceDescriptor(u, &id);
		tb_fmt(&rpt, "  %04x:%04x usb %x.%02x class %02x.%02x.%02x bcdDevice %04x, %u configurations;"
		       " this handle: interface %u class %02x.%02x.%02x\n    ", dd.IdVendor, dd.IdProduct,
		       dd.BcdUSB >> 8, dd.BcdUSB & 0xff, dd.DeviceClass, dd.DeviceSubClass, dd.DeviceProtocol,
		       dd.BcdDevice, dd.NumConfigurations, id.InterfaceNumber, id.InterfaceClass,
		       id.InterfaceSubClass, id.InterfaceProtocol);
		rpt_usb_strings(u, dd.StrManufacturer, "maker");
		rpt_usb_strings(u, dd.StrProduct, "product");
		tb_putc(&rpt, '\n');
		say("  USB %04x:%04x interface %u class %02x.%02x.%02x\n", dd.IdVendor, dd.IdProduct, id.InterfaceNumber,
		    id.InterfaceClass, id.InterfaceSubClass, id.InterfaceProtocol);
		if ( buf == 0 ) continue;
		for ( cf = 0; cf < dd.NumConfigurations && cf < 8; cf++ ) {
			EFI_USB_DEVICE_REQUEST	r;
			UINT32			total;

			r.RequestType = 0x80;
			r.Request = 6;			/* GET_DESCRIPTOR */
			r.Value = (UINT16)( 0x0200 | cf );
			r.Index = 0;
			r.Length = 9;
			st = 0;
			if ( EFI_ERROR(u->UsbControlTransfer(u, &r, EfiUsbDataIn, 1000, buf, 9, &st)) ) {
				tb_fmt(&rpt, "    configuration index %u: not read (%x)\n", cf, st);
				continue;
			}
			total = (UINT32)( buf[2] | ( buf[3] << 8 ) );
			if ( total > 4096 ) total = 4096;
			r.Length = (UINT16)total;
			if ( EFI_ERROR(u->UsbControlTransfer(u, &r, EfiUsbDataIn, 1000, buf, total, &st)) ) {
				tb_fmt(&rpt, "    configuration index %u: %u bytes not read (%x)\n", cf, total, st);
				continue;
			}
			tb_fmt(&rpt, "    configuration index %u, %u bytes:\n", cf, total);
			rpt_config(buf, total);
			rpt_hex(buf, total, "      ");
		}
	}
	BS->FreePool(h);
}

static void report( T_BOOTINFO *bi )
{
	rpt.cap = 2 * 1024 * 1024;
	rpt.p = alloc_pool(rpt.cap);
	rpt.len = 0;
	if ( rpt.p == 0 ) {
		say("  no memory for the report\n");
		return;
	}
	rpt.p[0] = '\0';
	tb_fmt(&rpt, "TessronOS: the machine as the loader found it\n\n[firmware] \"%s\" revision %08x, UEFI %u.%u\n",
	       bi->fw_vendor, ST->FirmwareRevision, ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xffff);
	tb_fmt(&rpt, "[loader] partition %u of its disk%s", bi->esp_part,
	       ( bi->flags & BI_F_ESP_GUID ) ? ", GUID" : " (no GPT GUID)");
	if ( bi->flags & BI_F_ESP_GUID ) {
		UINT32	i;

		for ( i = 0; i < 16; i++ ) tb_fmt(&rpt, " %02x", bi->esp_guid[i]);
	}
#ifdef USE_TESTAGENT
	{
		static char	shown[BI_CMDLINE_LEN];

		bi_cmdline_hide_key(bi->cmdline, shown);
		tb_fmt(&rpt, "\n[settings] \"%s\"\n", shown);
	}
#else
	tb_fmt(&rpt, "\n[settings] \"%s\"\n", bi->cmdline);
#endif
	stage("report: processor");
	rpt_cpu();
	stage("report: screen modes");
	rpt_gop();
	stage("report: memory map");
	rpt_map();
	stage("report: SMBIOS");
	rpt_smbios(bi);
	stage("report: ACPI tables");
	rpt_acpi(bi);
	stage("report: PCI");
	rpt_pci();
	(void)write_log(0, L"HW.TXT", rpt.p, rpt.len);	/* what there is so far, in case USB stops it */
	stage("report: USB");
	rpt_usb(bi->cmdline);
	if ( write_log(0, L"HW.TXT", rpt.p, rpt.len) ) {
		say("  the report: \\EFI\\TESSRON\\HW.TXT, %lu bytes\n", (UINT64)rpt.len);
	} else {
		say("  the report could not be written\n");
	}
}

/* ------------------------------------------------------------------ main */

EFI_STATUS EFIAPI efi_main( EFI_HANDLE image, EFI_SYSTEM_TABLE *st )
{
	T_BOOTINFO		*bi;
	UINT8			*elf, *cmd, *kimg;
	UINTN			elf_size, cmd_size, i;
	ELF64_EHDR		*eh;
	ELF64_PHDR		*ph;
	UINT64			lo = ~0ull, hi = 0, kva = 0, span, top, heap, dmap, kpa;
	UINT64			*pml4, tramp = 0x9f000;
	T_KHDR			*kh;
	UINT32			a, b, c, d;
	int			gb;
	EFI_STATUS		s;

	ST = st;
	BS = st->BootServices;
	Image = image;
	BS->SetWatchdogTimer(0, 0, 0, 0);
	com1_init();
	log_mem[0] = '\0';
	say("\nTessronOS loader\n");

	bi = alloc_pages(( sizeof(T_BOOTINFO) + EFI_PAGE_SIZE - 1 ) / EFI_PAGE_SIZE, MEM_TESSRON);
	BI = bi;
	bi->magic = BI_MAGIC;
	bi->version = BI_VERSION;
	bi->size = sizeof(T_BOOTINFO);
	for ( i = 0; i < sizeof(bi->fw_vendor) - 1 && st->FirmwareVendor[i] != 0; i++ ) {
		CHAR16	ch = st->FirmwareVendor[i];

		bi->fw_vendor[i] = ( ch < 0x80 ) ? (char)ch : '?';
	}

	Root = open_root();
	cmd = read_near(L"cmdline.txt", &cmd_size);
	/*
	 * The settings may also be written in cmdline.txt of the volume
	 * TSLOG, which Windows shows (the EFI system partition it does not,
	 * on a removable disk): when it is there, it is taken instead
	 */
	log_root = open_log_volume();
	if ( log_root != 0 ) {
		UINTN	lsize = 0;
		UINT8	*lcmd = read_file(log_root, L"\\cmdline.txt", &lsize);

		if ( lcmd != 0 ) {
			cmd = lcmd;
			cmd_size = lsize;
		}
	}
	if ( cmd != 0 && has_n((const char *)cmd, cmd_size, "tslog=0") && log_root != 0 ) {
		log_root->Close(log_root);	/* its settings read; no records written there */
		log_root = 0;
		no_tslog = 1;
	}
	if ( cmd != 0 ) {
		for ( i = 0; i < cmd_size && i < BI_CMDLINE_LEN - 1 && cmd[i] != '\n' && cmd[i] != '\r'; i++ ) {
			bi->cmdline[i] = (char)cmd[i];
		}
	}

	if ( has(bi->cmdline, "memtype=custom") ) kept_type = MEM_TESSRON;

	/* the screen first: from here on what is said can be read off a photograph */
	find_tables(bi);
	setup_screen(bi);
	stage("the files");
#ifdef USE_TESTAGENT
	{
		static char	shown[BI_CMDLINE_LEN];

		bi_cmdline_hide_key(bi->cmdline, shown);
		say("TessronOS loader: firmware \"%s\" %08x, UEFI %u.%u; partition %u%s; settings \"%s\"\n",
		    bi->fw_vendor, st->FirmwareRevision, st->Hdr.Revision >> 16, st->Hdr.Revision & 0xffff,
		    bi->esp_part, ( bi->flags & BI_F_ESP_GUID ) ? " (GPT)" : " (no GPT GUID)", shown);
	}
#else
	say("TessronOS loader: firmware \"%s\" %08x, UEFI %u.%u; partition %u%s; settings \"%s\"\n",
	    bi->fw_vendor, st->FirmwareRevision, st->Hdr.Revision >> 16, st->Hdr.Revision & 0xffff,
	    bi->esp_part, ( bi->flags & BI_F_ESP_GUID ) ? " (GPT)" : " (no GPT GUID)", bi->cmdline);
#endif
	open_dirs();
	if ( ts_dir == 0 ) say("TessronOS loader: \\EFI\\TESSRON cannot be made: no files will be written\n");
	boot_entry_request();		/* システム登録 asked for the firmware's entry of an internal disk */
	write_logs();

	elf = read_near(L"tessronos.elf", &elf_size);
	if ( elf == 0 ) stop("no tessronos.elf", 0);

	/* the kernel: a 64 bit executable for this machine */
	stage("the kernel");
	eh = (ELF64_EHDR *)elf;
	if ( elf_size < sizeof(*eh) || eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E'
	  || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' || eh->e_ident[4] != 2
	  || eh->e_machine != EM_X86_64 || eh->e_type != 2 ) {
		stop("tessronos.elf is not an x86-64 kernel", 0);
	}
	/*
	 * The image is laid out by the segments' physical addresses (the
	 * linker's load addresses), which keep the kernel's sections and
	 * the user section of the system process (linked at a low address
	 * of the process space, design 6.2.2) in one run; it is placed
	 * wherever there is room, and only the kernel's segments are mapped
	 * at their virtual addresses
	 */
	ph = (ELF64_PHDR *)( elf + eh->e_phoff );
	for ( i = 0; i < eh->e_phnum; i++ ) {
		if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 ) continue;
		if ( ph[i].p_paddr < lo ) {
			lo = ph[i].p_paddr;
			kva = ph[i].p_vaddr;
		}
		if ( ph[i].p_paddr + ph[i].p_memsz > hi ) hi = ph[i].p_paddr + ph[i].p_memsz;
	}
	if ( kva < KIMG_VA_BASE || ( lo & ( EFI_PAGE_SIZE - 1 ) ) != 0 || hi <= lo ) {
		stop("tessronos.elf is not linked where the kernel goes", 0);
	}
	span = ( hi - lo + EFI_PAGE_SIZE - 1 ) & ~(UINT64)( EFI_PAGE_SIZE - 1 );

	/* how much RAM there is decides the database and the linear map */
	alloc_map();
	top = ram_top();
	cpuid(0x80000001, &a, &b, &c, &d);
	gb = ( d >> 26 ) & 1;			/* 1GB pages */
	dmap = ( top + GB - 1 ) & ~( GB - 1 );
	if ( dmap < 4 * GB ) dmap = 4 * GB;

	/* the image, then what it wants after it, in one run */
	for ( i = 0; i < eh->e_phnum; i++ ) {
		if ( ph[i].p_type == PT_LOAD && ph[i].p_paddr == lo && ph[i].p_filesz >= sizeof(T_KHDR) ) break;
	}
	if ( i == eh->e_phnum ) stop("tessronos.elf has no header", 0);
	kh = (T_KHDR *)( elf + ph[i].p_offset );
	if ( kh->magic != KHDR_MAGIC ) stop("tessronos.elf has no TessronOS header", 0);
	heap = kh->heap + kh->db_per_page * ( top / EFI_PAGE_SIZE );
	heap = ( heap + MB2 - 1 ) & ~( MB2 - 1 );
	kimg = alloc_pages(( span + heap ) / EFI_PAGE_SIZE, MEM_TESSRON);
	kpa = (UINT64)kimg;
	for ( i = 0; i < eh->e_phnum; i++ ) {
		if ( ph[i].p_type != PT_LOAD ) continue;
		memcpy(kimg + ( ph[i].p_paddr - lo ), elf + ph[i].p_offset, ph[i].p_filesz);
	}
	bi->kimg_pa = kpa;
	bi->kimg_va = kva;
	bi->kimg_size = span;
	bi->heap_pa = kpa + span;
	bi->heap_size = heap;
	bi->ram_top = top;
	bi->dmap_size = dmap;
	bi->flags |= gb ? BI_F_1GPAGE : 0;
	say("TessronOS loader: kernel 0x%lx at 0x%lx, %lu KB + %lu MB; RAM to 0x%lx\n",
	    kva, kpa, span >> 10, heap >> 20, top);

	find_tables(bi);
	get_seed(bi);
	get_clock(bi);

	/* what the machine is, to files, before anything of the kernel's runs */
	if ( !has(bi->cmdline, "hwreport=0") && ts_dir != 0 ) {
		report(bi);
	}
	write_logs();

	/* four pages below 1MB for the other processors to start in: the code, and its page tables */
	stage("page tables");
	s = keep_pages(AllocateMaxAddress, 4, &tramp);
	if ( EFI_ERROR(s) || tramp < 0x1000 ) stop("no pages below 1MB", s);
	bi->tramp_pa = tramp;

	/*
	 * The page tables: every kernel entry of the PML4 with a table under
	 * it from the start, so that a process space can share them by
	 * copying the upper half; the linear map and the device window in
	 * large pages; the kernel image page by page, as its segments say;
	 * and the identity map of RAM in the lower half, which only the jump
	 * to the kernel uses
	 */
	pt_cap = 1 + 256 + 2 * ( dmap / GB ) * ( gb ? 0 : 1 ) + ( dmap / ( 512 * GB ) + 1 ) + 8
	       + ( span / MB2 + 2 ) + 4
	       + ( bi->fb.size / GB + 2 );		/* the screen's pages above 4GB */
	pt_pool = alloc_pages(pt_cap, MEM_TESSRON);
	pt_used = 0;
	pml4 = pt_new();
	for ( i = 256; i < 512; i++ ) {
		pml4[i] = (UINT64)pt_new() | PG_P | PG_RW;
	}
	map_large(pml4, KVA_BASE, 0, dmap, PG_P | PG_RW | PG_A | PG_D | PG_G | PG_NX, gb);
	map_large(pml4, DEV_VA_BASE, 0, 4 * GB, PG_P | PG_RW | PG_A | PG_D | PG_G | PG_NX | PG_PCD | PG_PWT, 0);
	/*
	 * A screen above 4GB (a PC whose firmware put the display's 64 bit
	 * BAR high) in the device window too, write-combining (PAT entry 1),
	 * so that the kernel's first line goes to it before its own tables
	 */
	if ( bi->fb.pa != 0 && bi->fb.pa + bi->fb.size > 4 * GB ) {
		UINT64	a = bi->fb.pa & ~(UINT64)( MB2 - 1 );
		UINT64	e = ( bi->fb.pa + bi->fb.size + MB2 - 1 ) & ~(UINT64)( MB2 - 1 );

		if ( a < 4 * GB ) a = 4 * GB;
		map_large(pml4, DEV_VA_BASE + a, a, e - a, PG_P | PG_RW | PG_A | PG_D | PG_G | PG_NX | PG_PWT, 0);
		say("TessronOS loader: the screen at 0x%lx is above 4GB: mapped for the kernel\n", bi->fb.pa);
	}
	for ( i = 0; i < eh->e_phnum; i++ ) {
		UINT64	va, size, attr = PG_P | PG_A | PG_G;

		if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 || ph[i].p_vaddr < KIMG_VA_BASE ) continue;
		va = ph[i].p_vaddr & ~(UINT64)( EFI_PAGE_SIZE - 1 );
		size = ( ( ph[i].p_vaddr + ph[i].p_memsz + EFI_PAGE_SIZE - 1 ) & ~(UINT64)( EFI_PAGE_SIZE - 1 ) ) - va;
		if ( ph[i].p_flags & PF_W ) attr |= PG_RW | PG_D;
		if ( ( ph[i].p_flags & PF_X ) == 0 ) attr |= PG_NX;
		map_pages(pml4, va, kpa + ( ( ph[i].p_paddr & ~(UINT64)( EFI_PAGE_SIZE - 1 ) ) - lo ), size, attr);
	}
	map_large(pml4, 0, 0, dmap, PG_P | PG_RW | PG_A | PG_D, gb);
	bi->pml4_pa = (UINT64)pml4;

	/* the last of what is said before the boot services are left, to the log file */
	stage("leaving the boot services");
	say("TessronOS loader: leaving the boot services; the kernel's console follows\n");
	if ( !has(bi->cmdline, "lastlog=0") ) write_logs();
	mark(BI_MK_LOGS, 0x20, 0x60, 0xff);
	if ( ts_dir != 0 ) ts_dir->Close(ts_dir);
	if ( acpi_dir != 0 ) acpi_dir->Close(acpi_dir);
	ts_dir = acpi_dir = 0;
	close_log_dirs();
	mark(BI_MK_CLOSED, 0x00, 0xe0, 0xe0);

	/* the boot services left: nothing allocates after the map is taken */
	get_map();
	mark(BI_MK_MAP, 0x00, 0xc0, 0x00);
	s = BS->ExitBootServices(image, mmap_key);
	if ( EFI_ERROR(s) ) {
		mark(BI_MK_EXIT1, 0xff, 0xff, 0x00);
		get_map();
		s = BS->ExitBootServices(image, mmap_key);
		if ( EFI_ERROR(s) ) stop("cannot leave the boot services", s);
		mark(BI_MK_EXIT2, 0xff, 0x90, 0x00);
	} else {
		mark(BI_MK_EXIT1, 0xa0, 0xff, 0x40);
	}
	booted_out = 1;
	convert_map(bi);
	mark(BI_MK_CONV, 0xff, 0x00, 0xff);
	stage("to the kernel");
	say("TessronOS loader: to the kernel\n");
	bi->con_scale = con_scale;
	bi->con_y = ( con_fb != 0 ) ? ( con_cy + 1 ) * CONFONT_H * con_scale : 0;

	__asm__ volatile ( "cli" );
	wrmsr(MSR_PAT, PAT_VALUE);
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
	mark(BI_MK_JUMP, 0x80, 0x40, 0xc0);
	__asm__ volatile (
		"mov	%0, %%cr3\n\t"
		"mov	%1, %%rdi\n\t"
		"jmp	*%2"
		:: "r"((UINT64)pml4), "r"((UINT64)bi), "r"(eh->e_entry) : "rdi", "memory");
	return EFI_LOAD_ERROR;
}
