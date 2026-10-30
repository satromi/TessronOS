/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	paths.h
 *	The places of the system's files, as the object world shows them
 *	(design 5.8.1): the commands are in /bin, the devices in /dev, the
 *	temporary files in /tmp, each a plain object linked from "/".
 */

#ifndef _PATHS_H_
#define _PATHS_H_

#define _PATH_DEFPATH	"/bin"
#define _PATH_STDPATH	"/bin"
#define _PATH_BSHELL	"/bin/sh"
#define _PATH_CSHELL	"/bin/tsh"
#define _PATH_DEV	"/dev/"
#define _PATH_DEVNULL	"/dev/null"
#define _PATH_DEVZERO	"/dev/zero"
#define _PATH_TTY	"/dev/tty"
#define _PATH_CONSOLE	"/dev/tty"
#define _PATH_TMP	"/tmp/"
#define _PATH_VARTMP	"/tmp/"
#define _PATH_VARRUN	"/tmp/"
#define _PATH_VI	"/bin/vi"
#define _PATH_MORE	"/bin/more"
#define _PATH_LESS	"/bin/less"
#define _PATH_PAGER	"/bin/less"
#define _PATH_MAN	"/bin/man"
#define _PATH_PASSWD	"/etc/passwd"
#define _PATH_GROUP	"/etc/group"
#define _PATH_SHELLS	"/etc/shells"
#define _PATH_MAILDIR	"/tmp"
#define _PATH_NOLOGIN	"/etc/nologin"
#define _PATH_SENDMAIL	"/bin/sendmail"
#define _PATH_LOCALE	"/etc/locale"
#define _PATH_HOME	"/home"		/* the object home, which links to the cabinet the desktop opens with */
#define _PATH_DEFTAPE	"/dev/tape"	/* there is no tape: tar is given -f */

#endif /* _PATHS_H_ */
