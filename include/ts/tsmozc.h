/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsmozc.h
 *	The kana-kanji conversion engine (mozc), as C
 *
 *	The engine is built apart (tools/mozc/build.sh) into one object that
 *	holds mozc, the libraries under it and its own C and C++ runtimes,
 *	and shows only these functions. It speaks the BTRON conversion
 *	server's protocol inside (a key goes in, the composition comes out);
 *	here that protocol is given in UTF-8, with the clauses as byte
 *	offsets into the text.
 *
 *	The engine is used from one task only, and not at the same time
 *	from two: application/kconv runs it in its own and hands requests to
 *	it.
 *
 *	The header is plain C with no system types, so that the engine's
 *	side and the system's side can both include it.
 */

#ifndef __TS_TSMOZC_H__
#define __TS_TSMOZC_H__

#ifdef __cplusplus
extern "C" {
#endif

#define TSMOZC_TEXT_MAX		1024	/* bytes of the composition (UTF-8) */
#define TSMOZC_CL_MAX		64	/* clauses in it */

/* What a key did: tsmozc_out.flags */
#define TSMOZC_OUT		0x01	/* clauses were committed */
#define TSMOZC_CNV		0x02	/* the composition changed */
#define TSMOZC_CAR		0x04	/* the caret moved */
#define TSMOZC_CL		0x08	/* the clause being converted changed */
#define TSMOZC_LIST		0x10	/* the candidates are to be shown */
#define TSMOZC_LISTCHG		0x20	/* ... and they are not the ones shown */

/* tsmozc_key's answer for a key the engine leaves to the application */
#define TSMOZC_NOTMINE		1

/*
 * The composition after a key: the clauses already committed at its
 * head (n_out of them), then those still being composed. cl[k] is where
 * clause k starts in text, cl[n_cl] where the last one ends.
 */
typedef struct {
	int		n_out;		/* committed clauses */
	int		n_cl;		/* clauses in all */
	int		clause;		/* the one being converted */
	int		yomi;		/* it is shown as its reading */
	int		caret;		/* the caret, bytes into text */
	int		sel;		/* the candidate chosen, from 1, or 0 */
	unsigned int	flags;		/* TSMOZC_* */
	int		cl[TSMOZC_CL_MAX + 1];
	char		text[TSMOZC_TEXT_MAX];
} tsmozc_out;

/*
 * Keys are BTRON key codes: a character as its TRON code (ASCII letters
 * and signs as themselves, kana as JIS X 0208), or one of these, which
 * are the codes the keys of the conversion's key assignment have.
 */
#define TSMOZC_K_IEND		0x0004	/* 入力終: 右Ctrl, F10 */
#define TSMOZC_K_BS		0x0008	/* 一字消: Backspace */
#define TSMOZC_K_TAB		0x0009	/* Tab */
#define TSMOZC_K_NL		0x000a	/* 改段落: Enter */
#define TSMOZC_K_CR		0x000d	/* 改行: Shift+Enter */
#define TSMOZC_K_CAN		0x0018	/* 取消: Esc, F9 */
#define TSMOZC_K_ASSIST		0x001b	/* 補助: Alt+英数, F11 */
#define TSMOZC_K_ESC		TSMOZC_K_ASSIST
#define TSMOZC_K_CNV		0x001e	/* 変換 */
#define TSMOZC_K_RCNV		0x001f	/* 逆変換: Shift+変換 */
#define TSMOZC_K_DEL		0x007f	/* 削除: Delete */
#define TSMOZC_K_UP		0x0100	/* the cursor keys */
#define TSMOZC_K_DOWN		0x0101
#define TSMOZC_K_RIGHT		0x0102
#define TSMOZC_K_LEFT		0x0103
#define TSMOZC_K_SC_U		0x0104	/* Alt+the cursor keys */
#define TSMOZC_K_SC_D		0x0105
#define TSMOZC_K_SC_R		0x0106
#define TSMOZC_K_SC_L		0x0107
#define TSMOZC_K_PG_U		0x010c	/* Page Up */
#define TSMOZC_K_PG_D		0x010d	/* Page Down */
#define TSMOZC_K_HIRA		0x1151	/* 無変換 */
#define TSMOZC_K_KATA		0x1152	/* Shift+無変換 */
#define TSMOZC_K_PF1		0x1161	/* F1 (to F8: 0x1168) */
#define TSMOZC_K_HOME		0x1245	/* Home */
#define TSMOZC_K_LIST_PREV	0x1246	/* the candidate list a page back */
#define TSMOZC_K_LIST_NEXT	0x1247	/* ... and a page on */
#define TSMOZC_K_END		0x125e	/* End */
#define TSMOZC_K_SPACE		0x2121	/* 空白 */

/* Key state bits (BTRON ES_*): the locks of the keyboard's input mode */
#define TSMOZC_S_ALPH		0x00000004	/* 英語 (英数): letters as they are */
#define TSMOZC_S_KANA		0x00000008	/* カタカナ (Shift+ひらがなカタカナ) */
#define TSMOZC_S_SHIFT		0x00000010
#define TSMOZC_S_CTRL		0x00000080
#define TSMOZC_S_HAN		0x00010000	/* 半角 (半角/全角) */

/* Session modes */
#define TSMOZC_M_ROMAN		0x0001	/* romaji is typed (else kana keys) */

/*
 * The engine made from its dictionary: 0, or below 0 when it could not
 * be. The dictionary is mozc's data set, whole, in memory that stays
 * while the engine runs; the engine carries none of its own.
 */
int tsmozc_start(const void *dict, unsigned long size);

/* What the engine has to say about itself (profile, settings ignored) */
const char *tsmozc_note(void);

/* A session: its number, or below 0 */
int tsmozc_open(int mode);

/* How a session's letters are typed: TSMOZC_M_ROMAN, or kana keys */
int tsmozc_input(int kid, int mode);
void tsmozc_close(int kid);

/*
 * A key into a session: the flags of what it did, TSMOZC_NOTMINE for a
 * key it did not take, or below 0.
 */
int tsmozc_key(int kid, unsigned int code, unsigned int stat, tsmozc_out *out);

/* Candidate n (from 1) of the list shown chosen and committed */
int tsmozc_choose(int kid, int n, tsmozc_out *out);

/*
 * The candidates shown, UTF-8 one after another each ending in 0: how
 * many, and the number of the first (from 1) and the total. Answers the
 * chosen one (from 1), or below 0 when no list is shown.
 */
int tsmozc_list(int kid, char *buf, int size, int *p_count, int *p_first,
		int *p_total);

/* What has been learnt, written down */
void tsmozc_flush(void);

/*
 * What the engine asks of the system (application/kconv supplies them).
 * Its C library's system calls are built on these: memory, the clock,
 * files, and where its output and its end go. Paths are as the engine
 * names them; the system decides where they are. Every call answers
 * below 0 on failure.
 */
#define TSMOZC_O_WRITE		0x01	/* for writing too */
#define TSMOZC_O_CREATE		0x02
#define TSMOZC_O_TRUNC		0x04
#define TSMOZC_O_APPEND		0x08
#define TSMOZC_O_EXCL		0x10

void *tsmozc_host_sbrk(long incr);		/* (void *)-1 when full */
long long tsmozc_host_clock(int monotonic);	/* nanoseconds */
void tsmozc_host_sleep(long long ns);
void tsmozc_host_log(const char *s, int len);
void tsmozc_host_exit(int code);		/* does not return */
int tsmozc_host_open(const char *path, int flags);
int tsmozc_host_close(int fd);
int tsmozc_host_read(int fd, void *buf, int len);
int tsmozc_host_write(int fd, const void *buf, int len);
long long tsmozc_host_lseek(int fd, long long off, int whence);
/* size, whether a directory, modification time in seconds */
int tsmozc_host_stat(const char *path, long long *p_size, int *p_dir,
		     long long *p_mtime);
int tsmozc_host_fstat(int fd, long long *p_size, int *p_dir,
		      long long *p_mtime);
int tsmozc_host_unlink(const char *path);
int tsmozc_host_rename(const char *from, const char *to);
int tsmozc_host_mkdir(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSMOZC_H__ */
