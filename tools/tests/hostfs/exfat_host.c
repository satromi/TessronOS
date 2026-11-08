/*
 * The exFAT fimp on the host, against an image made by mkfs.exfat: every
 * kind of change, a look back at the data, then the image is left clean
 * for fsck.exfat (hostfs.sh). Usage: exfat_host IMAGE [FILE-TO-READ]
 * A byte-exact pattern lets the test compare what it wrote; the image is
 * then checked with mounting by the standard tools where available.
 */
#include "kernel.h"
#include <sys/machine.h>
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/proc.h>

extern const T_FIMP knl_fimp_exfat;
static FILE *img;
static int fails;
#define CK(c) do { if ( !(c) ) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

ER tk_srea_dev_d(ID dd, D start, void *buf, SZ size, SZ *asize)
{
	(void)dd;
	if ( fseek(img, start * 512, SEEK_SET) != 0 || fread(buf, 1, size, img) != (size_t)size ) return E_IO;
	*asize = size / 512;
	return E_OK;
}
ER tk_swri_dev_d(ID dd, D start, const void *buf, SZ size, SZ *asize)
{
	(void)dd;
	if ( fseek(img, start * 512, SEEK_SET) != 0 || fwrite(buf, 1, size, img) != (size_t)size ) return E_IO;
	*asize = size / 512;
	return E_OK;
}

static T_MOUNT mnt;

static void remount(void)
{
	if ( mnt.exinf != NULL ) CK(knl_fimp_exfat.unmount(&mnt) == EX_OK);
	CK(knl_fimp_exfat.mount(&mnt) == EX_OK);
}

static ER opn(const char *p, UINT fl, T_FILE *f)
{
	memset(f, 0, sizeof(*f));
	f->mount = &mnt;
	f->oflags = fl;
	return knl_fimp_exfat.open(&mnt, p, fl, f);
}

static UB pat(UD off, UD seed) { return (UB)( ( off * 2654435761ULL >> 13 ) ^ seed ^ ( off >> 9 ) ); }

static void wfile(const char *p, UD size, UD seed, SZ chunk)
{
	T_FILE f;
	UB *b = malloc(chunk);
	UD off = 0;

	CK(opn(p, O_RDWR | O_CREAT | O_TRUNC, &f) == EX_OK);
	while ( off < size ) {
		SZ n = chunk, i;
		if ( (UD)n > size - off ) n = (SZ)( size - off );
		for ( i = 0; i < n; i++ ) b[i] = pat(off + i, seed);
		CK(knl_fimp_exfat.write(&f, b, n) == n);
		off += n;
	}
	CK(knl_fimp_exfat.close(&f) == EX_OK);
	free(b);
}

static void rfile(const char *p, UD size, UD seed)
{
	T_FILE f;
	UB *b = malloc(70000);
	UD off = 0;
	int bad = 0;

	CK(opn(p, O_RDONLY, &f) == EX_OK);
	CK(f.size == size);
	for ( ;; ) {
		INT n = knl_fimp_exfat.read(&f, b, 70000 - (off % 3));
		INT i;
		if ( n <= 0 ) break;
		for ( i = 0; i < n; i++ ) if ( b[i] != pat(off + i, seed) ) bad++;
		off += n;
	}
	CK(off == size);
	CK(bad == 0);
	CK(knl_fimp_exfat.close(&f) == EX_OK);
	free(b);
}

static int count_dir(const char *p)
{
	T_FILE f;
	static T_DIRENT d[8];
	int c = 0;
	INT n;

	CK(opn(p, O_RDONLY, &f) == EX_OK);
	while ( ( n = knl_fimp_exfat.getdents(&f, d, 8) ) > 0 ) c += n;
	CK(knl_fimp_exfat.close(&f) == EX_OK);
	return c;
}

int main(int ac, char **av)
{
	T_FSTAT st;
	T_FSSTAT vs;
	T_FILE f;
	char nm[300];
	int i;
	UD fr0;

	if ( ac < 2 ) return 2;
	img = fopen(av[1], "r+b");
	if ( img == NULL ) return 2;
	strcpy((char *)mnt.dev, "img");
	mnt.dd = 1;
	CK(knl_fimp_exfat.mount(&mnt) == EX_OK);
	if ( mnt.exinf == NULL ) return 1;
	if ( ac > 2 ) {		/* files put there by another driver */
		T_FILE g;
		CK(opn(av[2], O_RDONLY, &g) == EX_OK);
		printf("seen size %lu\n", (unsigned long)g.size);
		knl_fimp_exfat.close(&g);
		knl_fimp_exfat.unmount(&mnt);
		return fails != 0;
	}
	CK(knl_fimp_exfat.statvfs(&mnt, &vs) == EX_OK);
	fr0 = vs.bfree;
	printf("volume: %lu clusters of %lu, %lu free\n", (unsigned long)vs.blocks, (unsigned long)vs.bsize, (unsigned long)vs.bfree);

	/* small, across sectors, across clusters, large; sizes on and off the boundaries */
	wfile("/small.txt", 100, 1, 37);
	wfile("/sect.bin", 512, 2, 512);
	wfile("/clus.bin", 4096 * 3 + 1, 3, 1000);
	wfile("/large.bin", 5 * 1024 * 1024 + 123, 4, 65536);
	rfile("/small.txt", 100, 1);
	rfile("/sect.bin", 512, 2);
	rfile("/clus.bin", 4096 * 3 + 1, 3);
	rfile("/large.bin", 5 * 1024 * 1024 + 123, 4);
	remount();
	rfile("/large.bin", 5 * 1024 * 1024 + 123, 4);

	/* directories, nested, and one that must grow beyond a cluster */
	CK(knl_fimp_exfat.mkdir(&mnt, "/dir1") == EX_OK);
	CK(knl_fimp_exfat.mkdir(&mnt, "/dir1") == EX_EXIST);
	CK(knl_fimp_exfat.mkdir(&mnt, "/dir1/sub") == EX_OK);
	CK(knl_fimp_exfat.mkdir(&mnt, "/no/such") == EX_NOENT);
	for ( i = 0; i < 300; i++ ) {
		sprintf(nm, "/dir1/\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB name that is rather long number %d.dat", i);
		wfile(nm, 10 + i, i, 64);
	}
	CK(count_dir("/dir1") == 301);
	sprintf(nm, "/dir1/\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB name that is rather long number %d.dat", 299);
	rfile(nm, 309, 299);

	/* the case of a name */
	CK(knl_fimp_exfat.stat(&mnt, "/SMALL.TXT", &st) == EX_OK);
	CK(st.size == 100);
	CK(opn("/SMALL.TXT", O_RDWR | O_CREAT | O_EXCL, &f) == EX_EXIST);

	/* seek past the end: what lies between reads as zeros */
	CK(opn("/hole.bin", O_RDWR | O_CREAT, &f) == EX_OK);
	f.offset = 100000;
	CK(knl_fimp_exfat.write(&f, "end", 3) == 3);
	f.offset = 0;
	{
		UB b[100003];
		int bad = 0;
		CK(knl_fimp_exfat.read(&f, b, sizeof(b)) == 100003);
		for ( i = 0; i < 100000; i++ ) if ( b[i] != 0 ) bad++;
		CK(bad == 0 && memcmp(b + 100000, "end", 3) == 0);
	}
	CK(knl_fimp_exfat.close(&f) == EX_OK);

	/* longer and shorter: made longer without writing, it reads zeros */
	CK(opn("/clus.bin", O_RDWR, &f) == EX_OK);
	CK(knl_fimp_exfat.truncate(&f, 1000) == EX_OK);
	CK(knl_fimp_exfat.truncate(&f, 200000) == EX_OK);
	f.offset = 1000;
	{
		UB b[5000];
		int bad = 0;
		CK(knl_fimp_exfat.read(&f, b, sizeof(b)) == 5000);
		for ( i = 0; i < 5000; i++ ) if ( b[i] != 0 ) bad++;
		CK(bad == 0);
	}
	CK(knl_fimp_exfat.close(&f) == EX_OK);

	/* fragmented: two files written by turns have to go to chains */
	{
		T_FILE a, b;
		UB buf[4096];
		UD off;
		CK(opn("/frag_a.bin", O_RDWR | O_CREAT, &a) == EX_OK);
		CK(opn("/frag_b.bin", O_RDWR | O_CREAT, &b) == EX_OK);
		for ( off = 0; off < 40 * 4096; off += 4096 ) {
			int k;
			for ( k = 0; k < 4096; k++ ) buf[k] = pat(off + k, 10);
			CK(knl_fimp_exfat.write(&a, buf, 4096) == 4096);
			for ( k = 0; k < 4096; k++ ) buf[k] = pat(off + k, 11);
			CK(knl_fimp_exfat.write(&b, buf, 4096) == 4096);
		}
		CK(knl_fimp_exfat.close(&a) == EX_OK);
		CK(knl_fimp_exfat.close(&b) == EX_OK);
		rfile("/frag_a.bin", 40 * 4096, 10);
		rfile("/frag_b.bin", 40 * 4096, 11);
	}

	/* rename: in place, across directories, over a file, a directory, not into itself */
	CK(knl_fimp_exfat.rename(&mnt, "/sect.bin", "/sect renamed.bin") == EX_OK);
	rfile("/sect renamed.bin", 512, 2);
	CK(knl_fimp_exfat.stat(&mnt, "/sect.bin", &st) == EX_NOENT);
	CK(knl_fimp_exfat.rename(&mnt, "/sect renamed.bin", "/dir1/sub/moved.bin") == EX_OK);
	rfile("/dir1/sub/moved.bin", 512, 2);
	wfile("/over.bin", 9000, 20, 512);
	CK(knl_fimp_exfat.rename(&mnt, "/over.bin", "/small.txt") == EX_OK);
	rfile("/small.txt", 9000, 20);
	CK(knl_fimp_exfat.rename(&mnt, "/small.txt", "/SMALL.txt") == EX_OK);
	CK(knl_fimp_exfat.mkdir(&mnt, "/dir2") == EX_OK);
	CK(knl_fimp_exfat.rename(&mnt, "/dir1", "/dir2/dir1") == EX_OK);
	CK(count_dir("/dir2/dir1") == 301);
	CK(knl_fimp_exfat.rename(&mnt, "/dir2", "/dir2/dir1/x") == EX_INVAL);

	/* removing */
	CK(knl_fimp_exfat.rmdir(&mnt, "/dir2") == EX_NOTEMPTY);
	CK(knl_fimp_exfat.unlink(&mnt, "/dir2") == EX_ISDIR);
	CK(knl_fimp_exfat.unlink(&mnt, "/frag_a.bin") == EX_OK);
	CK(opn("/frag_b.bin", O_RDONLY, &f) == EX_OK);
	CK(knl_fimp_exfat.unlink(&mnt, "/frag_b.bin") == EX_BUSY);
	CK(knl_fimp_exfat.close(&f) == EX_OK);
	CK(knl_fimp_exfat.utime(&mnt, "/large.bin", 1000000000) == EX_OK);
	CK(knl_fimp_exfat.stat(&mnt, "/large.bin", &st) == EX_OK);
	CK(st.mtime >= 1000000000 - 2 && st.mtime <= 1000000000);
	CK(knl_fimp_exfat.statvfs(&mnt, &vs) == EX_OK);
	CK(vs.bfree < fr0);
	remount();
	{
		T_FSSTAT again;
		CK(knl_fimp_exfat.statvfs(&mnt, &again) == EX_OK);
		CK(again.bfree == vs.bfree);	/* counted anew: the count kept was true */
	}
	rfile("/frag_b.bin", 40 * 4096, 11);
	CK(knl_fimp_exfat.sync(&mnt) == EX_OK);

	/* a volume filled up: what fits, EX_NOSPC after, and nothing lost */
	{
		T_FILE g;
		UB *big = malloc(1 << 20);
		INT r;
		UD tot = 0;
		memset(big, 0x5A, 1 << 20);
		CK(opn("/full.bin", O_RDWR | O_CREAT, &g) == EX_OK);
		while ( ( r = knl_fimp_exfat.write(&g, big, 1 << 20) ) == 1 << 20 ) tot += r;
		if ( r > 0 ) {
			tot += r;
			r = knl_fimp_exfat.write(&g, big, 1 << 20);
		}
		CK(r == EX_NOSPC);
		CK(g.size == tot);
		CK(knl_fimp_exfat.close(&g) == EX_OK);
		CK(knl_fimp_exfat.statvfs(&mnt, &vs) == EX_OK);
		CK(vs.bfree == 0);
		CK(knl_fimp_exfat.unlink(&mnt, "/full.bin") == EX_OK);
		free(big);
	}
	rfile("/large.bin", 5 * 1024 * 1024 + 123, 4);
	CK(knl_fimp_exfat.unmount(&mnt) == EX_OK);

	/* a volume that says it was not closed is served for reading, and left as it is */
	{
		UB fl[2], fl2[2];
		T_FILE g;
		fseek(img, 106, SEEK_SET);
		fread(fl, 1, 2, img);
		fl[0] |= 2;
		fseek(img, 106, SEEK_SET);
		fwrite(fl, 1, 2, img);
		CK(knl_fimp_exfat.mount(&mnt) == EX_OK);
		rfile("/large.bin", 5 * 1024 * 1024 + 123, 4);
		CK(opn("/nope", O_RDWR | O_CREAT, &g) == EX_ROFS);
		CK(knl_fimp_exfat.mkdir(&mnt, "/nope") == EX_ROFS);
		CK(knl_fimp_exfat.unmount(&mnt) == EX_OK);
		fseek(img, 106, SEEK_SET);
		fread(fl2, 1, 2, img);
		CK(fl2[0] == fl[0]);
		fl[0] &= ~2;
		fseek(img, 106, SEEK_SET);
		fwrite(fl, 1, 2, img);
	}
	fclose(img);
	printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
	return fails != 0;
}
