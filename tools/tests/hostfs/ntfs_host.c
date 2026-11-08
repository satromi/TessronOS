/*
 * The NTFS fimp on the host. A small script of commands against an image
 * (ntfs_host.sh makes the image and checks it with the ntfs-3g tools):
 *   ntfs_host IMAGE ls DIR         the names, one to a line
 *   ntfs_host IMAGE cat FILE       the data, to standard output
 *   ntfs_host IMAGE poke FILE OFF SEED LEN   a pattern written in place
 *   ntfs_host IMAGE refuse FILE    every change that must be refused is
 *   ntfs_host IMAGE ro             says "ro" or "rw"
 */
#include "kernel.h"
#include <sys/machine.h>
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/proc.h>

extern const T_FIMP knl_fimp_ntfs;
static FILE *img;
static T_MOUNT mnt;

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

static ER opn(const char *p, UINT fl, T_FILE *f)
{
	memset(f, 0, sizeof(*f));
	f->mount = &mnt;
	f->oflags = fl;
	return knl_fimp_ntfs.open(&mnt, p, fl, f);
}

int main(int ac, char **av)
{
	T_FILE f;
	ER er;
	int rc = 0;

	if ( ac < 3 ) return 2;
	img = fopen(av[1], "r+b");
	if ( img == NULL ) return 2;
	strcpy((char *)mnt.dev, "img");
	mnt.dd = 1;
	er = knl_fimp_ntfs.mount(&mnt);
	if ( er < 0 ) { fprintf(stderr, "mount: %d\n", er); return 1; }

	if ( strcmp(av[2], "ro") == 0 ) {
		er = knl_fimp_ntfs.mkdir(&mnt, "/x");
		printf("%s\n", er == EX_ROFS ? "ro" : "rw");
	} else if ( strcmp(av[2], "ls") == 0 ) {
		static T_DIRENT d[5];
		INT n, i;
		er = opn(av[3], O_RDONLY, &f);
		if ( er < 0 ) { fprintf(stderr, "open %s: %d\n", av[3], er); return 1; }
		while ( ( n = knl_fimp_ntfs.getdents(&f, d, 5) ) > 0 ) {
			for ( i = 0; i < n; i++ ) printf("%s\n", d[i].name);
		}
		if ( n < 0 ) { fprintf(stderr, "getdents: %d\n", n); rc = 1; }
		knl_fimp_ntfs.close(&f);
	} else if ( strcmp(av[2], "cat") == 0 ) {
		static UB b[70001];
		INT n;
		er = opn(av[3], O_RDONLY, &f);
		if ( er < 0 ) { fprintf(stderr, "open %s: %d\n", av[3], er); return 1; }
		/* a length that keeps offsets off the sectors now and then */
		while ( ( n = knl_fimp_ntfs.read(&f, b, 70001) ) > 0 ) fwrite(b, 1, n, stdout);
		if ( n < 0 ) { fprintf(stderr, "read: %d\n", n); rc = 1; }
		knl_fimp_ntfs.close(&f);
	} else if ( strcmp(av[2], "poke") == 0 ) {
		UD off = strtoull(av[4], NULL, 0), seed = strtoull(av[5], NULL, 0), i;
		SZ len = strtol(av[6], NULL, 0);
		UB *b = malloc(len);
		for ( i = 0; i < (UD)len; i++ ) b[i] = (UB)( ( off + i ) * 2654435761ULL >> 13 ^ seed );
		er = opn(av[3], O_RDWR, &f);
		if ( er < 0 ) { fprintf(stderr, "open %s: %d\n", av[3], er); return 1; }
		f.offset = off;
		{
			INT w = knl_fimp_ntfs.write(&f, b, len);
			printf("wrote %d\n", w);
			if ( w != len ) rc = 1;
		}
		if ( knl_fimp_ntfs.close(&f) < 0 ) rc = 1;
		free(b);
	} else if ( strcmp(av[2], "refuse") == 0 ) {
		T_FILE g;
		UB b[10] = {0};
#define MUST(e, want) do { ER r_ = (e); if ( r_ != (want) ) { printf("%s gave %d\n", #e, r_); rc = 1; } } while (0)
		MUST(knl_fimp_ntfs.mkdir(&mnt, "/newdir"), EX_NOTSUP);
		MUST(knl_fimp_ntfs.unlink(&mnt, av[3]), EX_NOTSUP);
		MUST(knl_fimp_ntfs.rename(&mnt, av[3], "/renamed"), EX_NOTSUP);
		MUST(opn("/brandnew", O_RDWR | O_CREAT, &g), EX_NOTSUP);
		MUST(opn(av[3], O_RDWR | O_TRUNC, &g), EX_NOTSUP);
		er = opn(av[3], O_RDWR, &f);
		if ( er < 0 ) { fprintf(stderr, "open %s: %d\n", av[3], er); return 1; }
		f.offset = f.size;
		MUST(knl_fimp_ntfs.write(&f, b, 10), EX_NOTSUP);
		MUST(knl_fimp_ntfs.truncate(&f, 1), EX_NOTSUP);
		knl_fimp_ntfs.close(&f);
		printf("refused\n");
	}
	knl_fimp_ntfs.unmount(&mnt);
	fclose(img);
	return rc;
}
