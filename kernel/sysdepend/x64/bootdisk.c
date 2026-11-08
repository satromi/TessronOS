/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bootdisk.c (x64 PC)
 *	The disk the machine was started from, found by the partition the
 *	UEFI loader came from: the loader hands over that partition's GPT
 *	unique GUID (T_BOOTINFO.esp_guid), and the disks are read until one
 *	has a partition of that GUID. On a PC the disk started from may be a
 *	USB memory while another disk (the internal SSD) holds another
 *	system: only the disk started from is taken, its EFI system
 *	partition as /boot and its first TessronOS partition as the system
 *	volume (CNF_OB_SYSVOL names knl_sysvol_name). When no disk has the
 *	partition, neither is taken, and nothing of any disk is written.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include "../cpu/core/x86_64/bootinfo.h"

EXPORT unsigned char	knl_sysvol_name[L_DEVNM + 1] = "";	/* the system volume, "" for none */
EXPORT unsigned char	knl_bootvol_name[L_DEVNM + 1] = "";	/* the EFI system partition, "" for none */
EXPORT unsigned char	knl_logvol_name[L_DEVNM + 1] = "";	/* the records' partition TSLOG, "" for none */
EXPORT unsigned char	knl_bootdisk_note[256] = "";		/* what was found, for the boot log */

IMPORT CONST UB	knl_tsfs_type_guid[16];

/* The disks a PC starts from: the NVMe SSDs, the SATA ones, then USB memories */
LOCAL CONST char * CONST cands[] = {
	"nvmea", "nvmeb", "nvmec", "nvmed",
	"sataa", "satab", "satac", "satad",
	"sda",
	"uda", "udb", "udc", "udd", "ude", "udf", "udg", "udh",
	NULL
};

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL UD le64( CONST UB *p )
{
	return (UD)le32(p) | ( (UD)le32(p + 4) << 32 );
}

LOCAL BOOL same16( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

/* The basic data type (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7) as it lies in a GPT entry */
LOCAL CONST UB	basic_data[16] = {
	0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
};

/* An entry's name (UTF-16LE at 56) is "TSLOG" */
LOCAL BOOL named_tslog( CONST UB *e )
{
	static CONST char	w[] = "TSLOG";
	INT			k;

	for ( k = 0; w[k] != '\0'; k++ ) {
		if ( e[56 + k * 2] != (UB)w[k] || e[57 + k * 2] != 0 ) return FALSE;
	}
	return (BOOL)( e[56 + k * 2] == 0 && e[57 + k * 2] == 0 );
}

LOCAL BOOL zero16( CONST UB *a )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( a[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/*
 * The GPT of a disk read: the subunit (as part.c counts them, 0 the
 * first partition) of the partition with the GUID, and of its first
 * TessronOS partition; -1 for none. Only read.
 */
LOCAL ER scan( CONST char *disk, CONST UB *guid, INT *p_esp, INT *p_sys, INT *p_log )
{
	DiskInfo	info;
	UB		*s = NULL;
	SZ		asz;
	ID		dd;
	UD		entlba, nsect;
	UW		nent, entsz, bs, i, unit = 0, per;
	ER		er = E_OK;

	*p_esp = *p_sys = *p_log = -1;
	dd = tk_opn_dev((CONST UB *)disk, TD_READ);
	if ( dd < E_OK ) {
		return dd;
	}
	if ( tk_srea_dev(dd, TDN_DISKINFO, &info, sizeof(info), &asz) < E_OK || info.blocksize < 512
	  || info.blocksize > 4096 ) {
		er = E_NOSPT;
		goto out;
	}
	bs = (UW)info.blocksize;
	nsect = (UD)info.blockcont_d;
	s = (UB *)Kmalloc(bs);
	if ( s == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	if ( tk_srea_dev(dd, 1, s, bs, &asz) < E_OK ) {
		er = E_IO;
		goto out;
	}
	if ( s[0] != 'E' || s[1] != 'F' || s[2] != 'I' || s[3] != ' ' || s[4] != 'P' || s[5] != 'A'
	  || s[6] != 'R' || s[7] != 'T' ) {
		er = E_NOEXS;			/* no GPT */
		goto out;
	}
	entlba = le64(s + 72);
	nent = le32(s + 80);
	entsz = le32(s + 84);
	if ( entsz < 128 || entsz > bs || ( bs % entsz ) != 0 || nent > 1024 || entlba >= nsect ) {
		er = E_OBJ;
		goto out;
	}
	per = bs / entsz;
	for ( i = 0; i < nent; i++ ) {
		UB	*e;

		if ( ( i % per ) == 0 && tk_srea_dev_d(dd, (D)( entlba + i / per ), s, bs, &asz) < E_OK ) {
			er = E_IO;
			goto out;
		}
		e = s + ( i % per ) * entsz;
		if ( zero16(e) || le64(e + 40) < le64(e + 32) || le64(e + 40) >= nsect ) {
			continue;
		}
		if ( guid != NULL && same16(e + 16, guid) ) *p_esp = (INT)unit;
		if ( *p_sys < 0 && same16(e, knl_tsfs_type_guid) ) *p_sys = (INT)unit;
		if ( *p_log < 0 && same16(e, basic_data) && named_tslog(e) ) *p_log = (INT)unit;
		unit++;
	}
out:
	if ( s != NULL ) Kfree(s);
	tk_cls_dev(dd, 0);
	return er;
}

LOCAL void name_of( UB *out, CONST char *disk, INT unit )
{
	INT	n = 0;

	while ( disk[n] != '\0' && n < L_DEVNM - 3 ) {
		out[n] = (UB)disk[n];
		n++;
	}
	if ( unit >= 10 ) out[n++] = (UB)( '0' + unit / 10 );
	out[n++] = (UB)( '0' + unit % 10 );
	out[n] = '\0';
}

/*
 * Find the disk started from, waiting up to tmo ms for it to come (a USB
 * memory comes some time after its controller); TRUE when found
 */
EXPORT BOOL knl_bootdisk_find( CONST T_BOOTINFO *bi, TMO tmo )
{
	INT	waited = 0, i, esp, sys, log;
	ER	er;

	knl_sysvol_name[0] = knl_bootvol_name[0] = knl_logvol_name[0] = '\0';
	if ( ( bi->flags & BI_F_ESP_GUID ) == 0 ) {
		tm_printf((UB *)"bootdisk: the loader did not say which GPT partition it came from:"
			  " no system volume, /boot not mounted\n");
		tm_sprintf((UB *)knl_bootdisk_note, (UB *)"the loader gave no GUID");
		return FALSE;
	}
	for ( ;; ) {
		for ( i = 0; cands[i] != NULL; i++ ) {
			er = scan(cands[i], bi->esp_guid, &esp, &sys, &log);
			if ( er < E_OK || esp < 0 ) {
				continue;
			}
			name_of(knl_bootvol_name, cands[i], esp);
			if ( sys >= 0 ) name_of(knl_sysvol_name, cands[i], sys);
			if ( log >= 0 ) name_of(knl_logvol_name, cands[i], log);
			tm_printf((UB *)"bootdisk: started from %s: EFI system partition %s, system volume %s, records %s\n",
				  cands[i], knl_bootvol_name, ( sys >= 0 ) ? (char *)knl_sysvol_name : "(none)",
				  ( log >= 0 ) ? (char *)knl_logvol_name : "(none)");
			tm_sprintf((UB *)knl_bootdisk_note, (UB *)"disk %s, ESP %s, system volume %s, after %d ms",
				   cands[i], knl_bootvol_name, ( sys >= 0 ) ? (char *)knl_sysvol_name : "(none)", waited);
			return TRUE;
		}
		if ( waited >= tmo ) {
			break;
		}
		tk_dly_tsk(250);
		waited += 250;
	}
	tm_printf((UB *)"bootdisk: no disk has the partition the loader came from (after %d ms):"
		  " no system volume, /boot not mounted\n", waited);
	tm_sprintf((UB *)knl_bootdisk_note, (UB *)"not found after %d ms", waited);
	return FALSE;
}

IMPORT CONST T_BOOTINFO	*knl_bootinfo;

/*
 * The disk started from looked for again after it was taken out: put
 * back, it may be known by another name (udb for what was uda). TRUE
 * when it is there, the partitions' names set to where they are now.
 * Says nothing while it is not there, since it is asked for often.
 */
EXPORT BOOL knl_bootdisk_again( void )
{
	CONST T_BOOTINFO	*bi = knl_bootinfo;
	INT			i, esp, sys, log;

	if ( bi == NULL || ( bi->flags & BI_F_ESP_GUID ) == 0 ) {
		return FALSE;
	}
	for ( i = 0; cands[i] != NULL; i++ ) {
		if ( scan(cands[i], bi->esp_guid, &esp, &sys, &log) < E_OK || esp < 0 ) {
			continue;
		}
		name_of(knl_bootvol_name, cands[i], esp);
		if ( sys >= 0 ) name_of(knl_sysvol_name, cands[i], sys);
		if ( log >= 0 ) {
			name_of(knl_logvol_name, cands[i], log);
		} else {
			knl_logvol_name[0] = '\0';
		}
		tm_printf((UB *)"bootdisk: the disk started from is back as %s: EFI system partition %s,"
			  " system volume %s, records %s\n", cands[i], knl_bootvol_name,
			  ( sys >= 0 ) ? (char *)knl_sysvol_name : "(none)",
			  ( log >= 0 ) ? (char *)knl_logvol_name : "(none)");
		return TRUE;
	}
	return FALSE;
}

#endif /* X64 */
