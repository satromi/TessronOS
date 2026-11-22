/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtm_main.c
 *	実時間メディア: what the player of 実時間制御TAD says, done (design 15.6)
 *
 *	Started by the player of 実時間制御TAD (lib/librtplay, in the program
 *	that plays the record) with an RTM_ARG naming two channels: one it
 *	reads the player's commands from, one it answers on. It has no
 *	window. It ends when the player deletes the channel (the player
 *	closed) or says RTM_QUIT, and when the program it plays for ends
 *	without a word, however it ends: it watches that process (OB_E_EXIT,
 *	told on the channel of its commands), and then deletes the channels
 *	the player made for it, which nobody else will.
 *
 *	A file is read from the object the player names -- a resource beside
 *	its records ("_0_1.mp3"), or a record of an object a file was taken
 *	in as -- by the loader task, decoded, and kept by the medium's
 *	number until the player closes it or opens another in its place.
 */

#include "rtm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

static ID	cmd = -1, ans = -1;
static ID	host_key = -1, host_port = -1;
static MEDIA	*media;		/* a place for each medium of the program, grown as its numbers come */
static INT	cmedia;

/* Room for medium inst (under the mix lock: the loader reads them under it). FALSE when there is no memory */
static BOOL media_room( INT inst )
{
	MEDIA	*nm;
	INT	c;
	BOOL	ok = TRUE;

	mix_lock();
	if ( inst >= cmedia ) {
		c = ( cmedia < 16 ) ? 16 : cmedia;
		while ( c <= inst ) c *= 2;
		nm = (MEDIA *)malloc(sizeof(MEDIA) * (size_t)c);
		if ( nm == NULL ) {
			ok = FALSE;
		} else {
			memset(nm, 0, sizeof(MEDIA) * (size_t)c);
			if ( media != NULL ) {
				memcpy(nm, media, sizeof(MEDIA) * (size_t)cmedia);
				free(media);
			}
			media = nm;
			cmedia = c;
		}
	}
	mix_unlock();
	return ok;
}

/* What the loader is to read */
#define LQ	32
static struct {
	INT	inst, gen, kind;
	RTM_SRC	src;
}		lq[LQ];
static volatile INT	lq_w = 0, lq_r = 0;
static ID	lq_sem = -1, lq_lk = -1;

EXPORT void rtm_log( CONST char *s )
{
	char	b[200];

	snprintf(b, sizeof(b), "rtmedia: %s\n", s);
	tm_putstring((CONST UB *)b);
}

EXPORT void rtm_answer( UINT op, INT inst, INT gen, D v, INT w, INT h )
{
	RTM_ANS	a;
	SZ	asz = 0;

	if ( ans <= 0 ) return;
	memset(&a, 0, sizeof(a));
	a.op = op;
	a.inst = inst;
	a.gen = gen;
	a.v = v;
	a.w = w;
	a.h = h;
	(void)ob_wri_rec(ans, 0, 0, &a, sizeof(a), &asz);
}

/* ---------------------------------------------------------------- reading */

/* All of a resource or a record, as long as it is */
static UB *read_all( CONST RTM_SRC *s, SZ *p_size, ER *p_er )
{
	ID	key = ob_opn_obj(&s->obj, OB_OP_READ);
	UB	*buf = NULL, *nb;
	SZ	cap = 0, got = 0;
	INT	rec = s->recno;

	*p_er = E_OK;
	if ( key < E_OK ) {
		*p_er = E_NOEXS;
		return NULL;
	}
	if ( rec < 0 && s->name[0] == 0 ) {
		/* a file taken in as an object: the largest record holds it */
		T_OBREC	r[32];
		INT	n = 0, i;
		D	best = -1;

		if ( ob_lst_rec(key, r, 32, &n) >= E_OK ) {
			for ( i = 0; i < n; i++ ) {
				if ( r[i].recno > 0 && (D)r[i].size > best ) {
					best = (D)r[i].size;
					rec = r[i].recno;
				}
			}
		}
		if ( rec < 0 ) {
			ob_cls_obj(key);
			*p_er = E_NOEXS;
			return NULL;
		}
	}
	for ( ;; ) {
		SZ	n = 0;
		ER	er;

		if ( got == cap ) {
			cap = ( cap == 0 ) ? 65536 : cap * 2;
			if ( cap > MAX_FILE ) break;
			nb = (UB *)realloc(buf, cap);
			if ( nb == NULL ) break;
			buf = nb;
		}
		er = ( rec >= 0 ) ? ob_rea_rec(key, rec, (D)got, buf + got, cap - got, &n)
				  : ob_rea_res(key, s->name, (D)got, buf + got, cap - got, &n);
		if ( er < E_OK ) {
			if ( got == 0 ) *p_er = E_NOEXS;
			break;
		}
		if ( n <= 0 ) break;
		got += n;
	}
	ob_cls_obj(key);
	if ( got == 0 ) {
		free(buf);
		if ( *p_er == E_OK ) *p_er = E_NOEXS;
		return NULL;
	}
	*p_size = got;
	return buf;
}

static void loader_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		INT	k, inst, gen;
		RTM_SRC	src;
		UB	*data;
		SZ	size = 0;
		ER	er;
		H	*pcm = NULL;
		INT	frames = 0;

		if ( tk_wai_sem(lq_sem, 1, TMO_FEVR) < E_OK ) break;
		(void)tk_wai_sem(lq_lk, 1, TMO_FEVR);
		k = lq_r % LQ;
		inst = lq[k].inst;
		gen = lq[k].gen;
		src = lq[k].src;
		lq_r++;
		(void)tk_sig_sem(lq_lk, 1);

		data = read_all(&src, &size, &er);
		if ( data == NULL ) {
			rtm_answer(RTM_A_ERROR, inst, gen, RT_ERR_NOTFOUND, 0, 0);
			continue;
		}
		er = dec_any(data, size, src.name, src.format, &pcm, &frames);
		free(data);
		if ( er < E_OK ) {
			char	b[160];

			snprintf(b, sizeof(b), "cannot play %s (%d)", (CONST char *)src.name, (INT)er);
			rtm_log(b);
			rtm_answer(RTM_A_ERROR, inst, gen, ( er == E_NOSPT ) ? RT_ERR_UNSUPP : RT_ERR_DECODE, 0, 0);
			continue;
		}
		mix_lock();
		if ( inst >= 0 && inst < cmedia && media[inst].gen == gen && media[inst].loading ) {
			if ( media[inst].pcm != NULL ) free(media[inst].pcm);
			media[inst].pcm = pcm;
			media[inst].frames = frames;
			media[inst].loading = FALSE;
			pcm = NULL;
		}
		mix_unlock();
		if ( pcm != NULL ) {
			free(pcm);		/* asked again or closed meanwhile */
			continue;
		}
		rtm_answer(RTM_A_LOADED, inst, gen, (D)frames * 1000000 / FS, 0, 0);
	}
	tk_ext_tsk();
}

/* ---------------------------------------------------------------- the commands */

static void media_drop( INT inst )
{
	mix_file_ctl(inst, RTM_CLOSE, 0);
	mix_lock();
	if ( inst < 0 || inst >= cmedia ) {
		mix_unlock();
		return;
	}
	if ( media[inst].pcm != NULL ) free(media[inst].pcm);
	media[inst].pcm = NULL;
	media[inst].frames = 0;
	media[inst].used = FALSE;
	media[inst].loading = FALSE;
	mix_unlock();
}

static void do_pcm( CONST RTM_CMD *c )
{
	ID	key = ob_opn_obj(&c->u.pcm.mem, OB_OP_READ);
	UB	*data;
	SZ	size = 0, asz = 0;
	H	*pcm = NULL;
	INT	frames = 0;
	ER	er;
	T_OBREC	r[2];
	INT	n = 0;

	if ( key < E_OK ) return;
	if ( ob_lst_rec(key, r, 2, &n) < E_OK || n < 1 ) {
		ob_cls_obj(key);
		return;
	}
	size = (SZ)r[0].size;
	data = (UB *)malloc(size + 1);
	if ( data == NULL || ob_rea_rec(key, 0, 0, data, size, &asz) < E_OK ) {
		free(data);
		ob_cls_obj(key);
		return;
	}
	ob_cls_obj(key);
	if ( c->u.pcm.wav ) {
		er = dec_any(data, asz, (CONST UB *)"", (CONST UB *)"wav", &pcm, &frames);
	} else {
		er = dec_raw(data, asz, c->u.pcm.rate, c->u.pcm.channels, c->u.pcm.bits, &pcm, &frames);
	}
	free(data);
	if ( er >= E_OK ) mix_once(pcm, frames, (float)( c->v > 0 ? c->v : 1000000 ) / 1000000.0f, c->at);
}

static BOOL command( CONST RTM_CMD *c )
{
	INT	i = c->inst;

	switch ( c->op ) {
	case RTM_OPEN:
		if ( i < 0 || i >= 0x100000 ) break;	/* a number the sender made: indexes of its program */
		if ( !media_room(i) ) {
			rtm_answer(RTM_A_ERROR, i, c->gen, RT_ERR_ABORTED, 0, 0);
			break;
		}
		media_drop(i);
		mix_lock();
		media[i].used = TRUE;
		media[i].gen = c->gen;
		media[i].kind = c->u.open.kind;
		media[i].loading = TRUE;
		mix_unlock();
		(void)tk_wai_sem(lq_lk, 1, TMO_FEVR);
		if ( lq_w - lq_r < LQ ) {
			lq[lq_w % LQ].inst = i;
			lq[lq_w % LQ].gen = c->gen;
			lq[lq_w % LQ].kind = c->u.open.kind;
			lq[lq_w % LQ].src = c->u.open.src;
			lq_w++;
			(void)tk_sig_sem(lq_lk, 1);
			(void)tk_sig_sem(lq_sem, 1);
		} else {
			(void)tk_sig_sem(lq_lk, 1);
			rtm_answer(RTM_A_ERROR, i, c->gen, RT_ERR_ABORTED, 0, 0);
		}
		break;
	case RTM_PLAY:
		if ( i < 0 || i >= cmedia || !media[i].used || media[i].pcm == NULL ) {
			rtm_answer(RTM_A_ERROR, i, c->gen, RT_ERR_ABORTED, 0, 0);
			break;
		}
		mix_play(&media[i], i, c->gen, c->at, c);
		rtm_answer(RTM_A_PLAYING, i, c->gen, 0, 0, 0);
		break;
	case RTM_PAUSE:
		mix_file_ctl(i, RTM_PAUSE, 0);
		rtm_answer(RTM_A_PAUSED, i, c->gen, 0, 0, 0);
		break;
	case RTM_RESUME:
		mix_file_ctl(i, RTM_RESUME, 0);
		rtm_answer(RTM_A_PLAYING, i, c->gen, 0, 0, 0);
		break;
	case RTM_STOP:
	case RTM_SEEK:
	case RTM_VOLUME:
	case RTM_PAN:
	case RTM_RATE:
	case RTM_MUTE:
		mix_file_ctl(i, c->op, c->v);
		break;
	case RTM_CLOSE:
		if ( i >= 0 && i < cmedia ) media_drop(i);
		break;
	case RTM_NOTE:
		mix_note(c->stream, c->u.note.channel, c->u.note.pitch, c->u.note.velocity, c->u.note.dur, c->at);
		break;
	case RTM_MIDI: {
		INT	k;

		for ( k = 0; k < c->u.midi.n && k < 16; k++ ) mix_midi(c->stream, c->u.midi.msg[k], c->at);
		break;
	}
	case RTM_STREAM:
		mix_stream(c->u.stream.number, c->u.stream.device[0] != 0 ? c->u.stream.device : NULL,
			   c->u.stream.params);
		break;
	case RTM_PCM:
		do_pcm(c);
		break;
	case RTM_SILENCE:
		mix_silence();
		break;
	case RTM_QUIT:
		return FALSE;
	default:
		break;
	}
	return TRUE;
}

/*
 * The end of the program the player is in, told on the channel of the
 * commands: a notice whose number stands where a command's op does
 */
static BOOL host_watch( CONST RTM_ARG *a )
{
	static CONST TS_UUID	none;
	T_OBNTF			req;

	if ( memcmp(&a->host, &none, sizeof(none)) == 0 ) return TRUE;
	host_port = ob_opn_obj(&a->cmd, OB_OP_WRITE | OB_O_NOWAIT);
	host_key = ob_opn_obj(&a->host, OB_OP_ATRRD);
	if ( host_key <= 0 ) {
		host_key = -1;
		return FALSE;		/* it has ended already */
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_EXIT;
	req.id = RTM_HOST_ID;
	if ( host_port <= 0 || ob_ntf_evt(host_key, OB_REC_ANY, &req, host_port) <= 0 ) {
		rtm_log("cannot watch the program it plays for");
	}
	return TRUE;
}

int main( void )
{
	RTM_ARG	arg;
	T_CSEM	cs;
	T_CTSK	ct;
	T_RTSK	rt;
	PRI	pri = 10;
	SZ	n;
	ID	ltid;
	BOOL	gone = FALSE;

	rtm_libc_init();
	memset(&arg, 0, sizeof(arg));
	n = ts_get_arg(&arg, sizeof(arg));
	if ( n < (SZ)offsetof(RTM_ARG, host) || arg.magic != RTM_MAGIC ) {
		rtm_log("started without the player's channels");
		return 1;
	}
	cmd = ob_opn_obj(&arg.cmd, OB_OP_READ);
	ans = ob_opn_obj(&arg.reply, OB_OP_WRITE | OB_O_NOWAIT);
	if ( cmd < E_OK || ans < E_OK ) {
		rtm_log("cannot open the player's channels");
		return 1;
	}
	if ( !host_watch(&arg) ) {
		rtm_log("the program it was to play for has ended");
		ob_cls_obj(cmd);
		ob_cls_obj(ans);
		(void)ob_del_obj(&arg.cmd);
		(void)ob_del_obj(&arg.reply);
		return 0;
	}
	if ( tk_ref_tsk(TSK_SELF, &rt) >= E_OK ) pri = rt.tskbpri;
	syn_init();
	if ( mix_start(pri) < E_OK ) rtm_log("no mixer");
	(void)tk_chg_pri(TSK_SELF, pri + 1);

	memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.isemcnt = 0;
	cs.maxsem = LQ;
	lq_sem = tk_cre_sem(&cs);
	cs.isemcnt = 1;
	cs.maxsem = 1;
	lq_lk = tk_cre_sem(&cs);
	memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG3;
	ct.task = (FP)loader_task;
	ct.itskpri = pri + 2;
	ct.stksz = 256 * 1024;
	ltid = tk_cre_tsk(&ct);
	if ( ltid > 0 ) (void)tk_sta_tsk(ltid, 0);

	rtm_answer(RTM_A_READY, -1, 0, 0, 0, 0);
	rtm_log("ready");
	for ( ;; ) {
		RTM_CMD	c;
		SZ	asz = 0;
		ER	er = ob_rea_rec(cmd, 0, 0, &c, sizeof(c), &asz);

		if ( er < E_OK ) break;		/* the channel went: the player has closed */
		if ( asz >= (SZ)sizeof(UINT) && c.op == RTM_HOST_ID ) {
			/* the program it plays for has ended: what it made for this goes too */
			rtm_log("the program it played for has ended");
			gone = TRUE;
			break;
		}
		if ( asz < (SZ)offsetof(RTM_CMD, u) ) continue;
		if ( asz < (SZ)sizeof(c) ) memset((UB *)&c + asz, 0, sizeof(c) - asz);
		if ( !command(&c) ) break;
	}
	mix_stop();
	rtm_log("ended");
	if ( host_key > 0 ) ob_cls_obj(host_key);
	if ( host_port > 0 ) ob_cls_obj(host_port);
	ob_cls_obj(cmd);
	ob_cls_obj(ans);
	if ( gone ) {
		(void)ob_del_obj(&arg.cmd);
		(void)ob_del_obj(&arg.reply);
	}
	return 0;
}
