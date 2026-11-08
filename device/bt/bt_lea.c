/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_lea.c
 *	Sound to LE Audio earphones (design 6.8): this end the unicast
 *	client of BAP, the source of its streams, and an output of the sound
 *	device like any other (sndhw.h).
 *
 *	The devices are a group: one device, or the two of a pair of
 *	earphones that CSIS says are one set. Once a device is ready
 *	(bt_bap.c), the group's streams are planned: two ASEs of one device
 *	for its left and right, one ASE of two channels, one ASE alone, or an
 *	ASE of each of the pair. Each stream is a CIS of the one CIG. They are
 *	given the same configuration of LC3, the first of 48, 32, 24 and 16
 *	kHz in 10 ms frames, at the bit rates BAP's settings 48_2, 32_2,
 *	24_2 and 16_2 have, that all of them take. They are configured
 *	(Config Codec), the CIG set up as their QoS preferences say, and their
 *	QoS configured: the group is then an output. When the sound device
 *	plays on it, the ASEs are enabled, their CISes created and their data
 *	paths set up; once the servers say they are streaming, an SDU goes on
 *	each CIS every 10 ms. When it stops, the ASEs are disabled and the
 *	CISes let go, the CIG staying for the next time.
 *
 *	As with A2DP, the sound device calls this backend with the USB
 *	manager's lock held on its own task, which must not do the controller
 *	task's work. LC3 computes in floating point, which the kernel does not
 *	use: the frames are encoded by a task of their own made with TA_FPU,
 *	the only one that calls the codec (lib/liblc3, built with the floating
 *	point registers). The three meet in two rings of one writer and one
 *	reader each: the poll mixes the PCM due by the clock into the first,
 *	the encoder makes the SDUs of each frame into the second, and the
 *	controller's task sends them as the controller has room for ISO
 *	packets.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "../snd/sndhw.h"
#include "btint.h"
#include "bt_lea.h"
#include "../../lib/liblc3/include/lc3.h"

#define SDU_US		10000		/* an SDU each frame of 10 ms */
#define CIG_ID		0
#define NPCM		6		/* frames mixed and not encoded yet: 60 ms */
#define NSDU		6
#define POLL_FRAMES	4		/* the most mixed in one look, not to hold the manager's lock long */
#define LEAD_FRAMES	2		/* mixed ahead when a stream starts */
#define ENC_TSK_PRI	20
#define ENC_STK		( 24 * 1024 )
#define FRAME_MAX	LC3_MAX_FRAME_SAMPLES
#define LAT_MIN		0x0005		/* the range of a transport latency, ms */
#define LAT_MAX		0x0fa0
#define RESET_TMO_MS	3000		/* CISes not said to be gone by then are taken as gone */
#define PLAY_TRIES	3

#define PHY_1M		0x01
#define PHY_2M		0x02
#define PHY_CODED	0x04

enum {
	G_NONE, G_CODEC, G_CIG, G_QOS, G_REMOVING,
	G_READY, G_ENABLING, G_CIS, G_STREAM, G_STOPPING	/* from here on the group is an output */
};

/* Where a stream's samples come from */
enum { SRC_L, SRC_R, SRC_BOTH, SRC_MIX };

typedef struct {
	LEADEV	*d;		/* NULL once the device is gone */
	INT	ai;
	UW	alloc;
	INT	ch;		/* channels in its SDU */
	UB	src;
	UB	cis_id;
	UH	cis;		/* the CIS's handle, 0xffff none */
	BOOL	up, path;
	UH	seq;
	UH	sdu;		/* bytes of its SDU */
	/* its QoS */
	UB	phy, rtn;
	UW	pd;
} STREAM;

/* What the poll, the encoder and the controller's task share */
typedef struct {
	BTC		*c;
	volatile BOOL	run;
	volatile UW	gen;		/* a new stream: the encoders set up again */
	UW		rate;
	INT		ns;		/* the samples of a channel in a frame */
	INT		oct;
	INT		nstream;
	UB		src[LEA_NSTREAM];
	volatile UW	ph, pt;		/* the PCM: the poll writes at pt, the encoder reads at ph */
	volatile UW	sh, st;		/* the SDUs: the encoder writes at st, the controller's task reads at sh */
	UW		lost, sent;
	short		pcm[NPCM][FRAME_MAX * 2];
	UB		sdu[NSDU][LEA_NSTREAM][LEA_SDU_MAX];
	lc3_encoder_t	enc[2];
	lc3_encoder_mem_48k_t mem[2];
} SHARED;

LOCAL struct {
	BTC		*c;
	UB		st;		/* G_* */
	INT		n;
	STREAM		s[LEA_NSTREAM];
	UB		freq;		/* the configuration's sampling frequency, as Config Codec says it */
	UW		rate;
	UH		oct;
	UB		framing;
	UH		latency;
	BOOL		cig_set;
	BOOL		reset;		/* the group is to be planned again */
	UW		reset_due;
	BOOL		replan;		/* a member of the set came while the group played */
	INT		tries;
	volatile BOOL	want_play;
	volatile BOOL	vol_req;
	volatile UB	vol;
	UW		start_ms;
	UD		made;
} g;

LOCAL SHARED	*sh_room = NULL;
LOCAL ID	enc_sem = 0, enc_tsk = 0;
LOCAL short	pcm_drop[FRAME_MAX * 2];	/* what is mixed when the ring has no room */

/* The settings 48_2, 32_2, 24_2, 16_2 of BAP: frequency, its bit among those supported, bytes a frame */
LOCAL CONST struct {
	UB	code;
	UH	bit;
	UW	rate;
	UH	oct;
} cfgs[] = {
	{ 0x08, 1 << 7, 48000, 100 },
	{ 0x06, 1 << 5, 32000, 80 },
	{ 0x05, 1 << 4, 24000, 60 },
	{ 0x03, 1 << 2, 16000, 40 },
};
#define NCFG	( (INT)( sizeof(cfgs) / sizeof(cfgs[0]) ) )

/* ---------------------------------------------------------------- the encoder */

LOCAL void enc_frame( SHARED *sh, CONST short *pcm, UB (*out)[LEA_SDU_MAX] )
{
	INT	i, e = 0;

	for ( i = 0; i < sh->nstream; i++ ) {
		switch ( sh->src[i] ) {
		case SRC_L:
		case SRC_R:
			(void)lc3_encode(sh->enc[e++], LC3_PCM_FORMAT_S16, pcm + ( sh->src[i] == SRC_R ), 2, sh->oct, out[i]);
			break;
		case SRC_BOTH:
			(void)lc3_encode(sh->enc[e++], LC3_PCM_FORMAT_S16, pcm, 2, sh->oct, out[i]);
			(void)lc3_encode(sh->enc[e++], LC3_PCM_FORMAT_S16, pcm + 1, 2, sh->oct, out[i] + sh->oct);
			break;
		default: {
			short	mono[FRAME_MAX];
			INT	k;

			for ( k = 0; k < sh->ns; k++ ) mono[k] = (short)( ( pcm[2 * k] + pcm[2 * k + 1] ) / 2 );
			(void)lc3_encode(sh->enc[e++], LC3_PCM_FORMAT_S16, mono, 1, sh->oct, out[i]);
			break;
		}
		}
	}
}

/* The one task that computes in floating point: its registers are kept across switches (TA_FPU) */
LOCAL void enc_task( INT stacd, void *exinf )
{
	SHARED	*sh = (SHARED *)exinf;
	UW	gen = 0;

	(void)stacd;
	for ( ;; ) {
		(void)tk_wai_sem(enc_sem, 1, 100);
		if ( !sh->run ) continue;
		if ( gen != sh->gen ) {
			gen = sh->gen;
			sh->enc[0] = lc3_setup_encoder(SDU_US, (int)sh->rate, 0, &sh->mem[0]);
			sh->enc[1] = lc3_setup_encoder(SDU_US, (int)sh->rate, 0, &sh->mem[1]);
		}
		while ( sh->run && sh->ph != sh->pt && ( sh->st + 1 ) % NSDU != sh->sh ) {
			enc_frame(sh, sh->pcm[sh->ph], sh->sdu[sh->st]);
			__sync_synchronize();
			sh->ph = ( sh->ph + 1 ) % NPCM;
			sh->st = ( sh->st + 1 ) % NSDU;
			(void)tk_sig_sem(sh->c->sem, 1);
		}
	}
}

/* The room the three share, and the encoder's task: made once, and kept */
LOCAL SHARED *shared( BTC *c )
{
	T_CSEM	cs;
	T_CTSK	ct;

	if ( sh_room != NULL ) {
		sh_room->c = c;
		return sh_room;
	}
	sh_room = (SHARED *)Kcalloc(1, sizeof(SHARED));
	if ( sh_room == NULL ) return NULL;
	sh_room->c = c;
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = 64;
	enc_sem = tk_cre_sem(&cs);
	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = sh_room;
	ct.tskatr = TA_HLNG | TA_RNG0 | TA_FPU;
	ct.task = (FP)enc_task;
	ct.itskpri = ENC_TSK_PRI;
	ct.stksz = ENC_STK;
	enc_tsk = ( enc_sem > 0 ) ? tk_cre_tsk(&ct) : E_LIMIT;
	if ( enc_tsk > 0 ) (void)tk_sta_tsk(enc_tsk, 0);
	if ( enc_sem <= 0 || enc_tsk <= 0 ) {
		tm_printf((UB *)"bt: no task to encode LC3 (%d, %d)\n", (INT)enc_sem, (INT)enc_tsk);
		Kfree(sh_room);
		sh_room = NULL;
	}
	return sh_room;
}

/* ---------------------------------------------------------------- the group */

LOCAL STREAM *stream_of_cis( UH h )
{
	INT	i;

	for ( i = 0; i < g.n; i++ ) {
		if ( g.s[i].cis == h ) return &g.s[i];
	}
	return NULL;
}

LOCAL CONST UB *name_of( void )
{
	INT	i;

	for ( i = 0; i < g.n; i++ ) {
		if ( g.s[i].d != NULL ) return g.s[i].d->p->name;
	}
	return (CONST UB *)"";
}

/* The location of one of a pair: what it says, or by its place in the set */
LOCAL UW loc_one( LEADEV *d, INT idx )
{
	UW	l = d->sink_loc & ( LOC_FL | LOC_FR );

	if ( l == LOC_FL || l == LOC_FR ) return l;
	return ( idx == 0 ) ? LOC_FL : LOC_FR;
}

LOCAL UB src_of( UW alloc )
{
	return ( alloc == LOC_FL ) ? SRC_L : ( alloc == LOC_FR ) ? SRC_R : SRC_MIX;
}

LOCAL BOOL cfg_fits( LEADEV *d, INT k, INT ch )
{
	return (BOOL)( d->lc3 && ( d->freqs & cfgs[k].bit ) != 0 && ( d->durs & 0x02 ) != 0
		    && cfgs[k].oct >= d->oct_min && cfgs[k].oct <= d->oct_max && cfgs[k].oct * ch <= LEA_SDU_MAX );
}

LOCAL void add_stream( LEADEV *d, INT ai, UW alloc, INT ch, UB src )
{
	STREAM	*s = &g.s[g.n];

	knl_memset(s, 0, sizeof(*s));
	s->d = d;
	s->ai = ai;
	s->alloc = alloc;
	s->ch = ch;
	s->src = src;
	s->cis_id = (UB)g.n;
	s->cis = 0xffff;
	g.n++;
}

/* The streams of the devices ready, and their codec configured */
LOCAL void plan( BTC *c )
{
	LEADEV	*m[2];
	INT	nm = 0, i, k;

	for ( i = 0; i < NPEER && nm == 0; i++ ) {
		LEADEV	*d = knl_btbap_dev(c, i);

		if ( d != NULL && d->ready && !d->failed && d->p->up ) m[nm++] = d;
	}
	if ( nm == 0 ) return;
	if ( m[0]->in_set && m[0]->set_size >= 2 ) {
		for ( i = 0; i < NPEER && nm < 2; i++ ) {
			LEADEV	*e = knl_btbap_dev(c, i);

			if ( e != NULL && e != m[0] && e->ready && !e->failed && e->p->up && e->in_set
			  && same_bt_bytes(e->sirk, m[0]->sirk, 16) ) m[nm++] = e;
		}
		if ( nm == 2 && ( loc_one(m[0], 0) == LOC_FR || ( m[1]->rank != 0 && m[1]->rank < m[0]->rank ) )
		  && loc_one(m[1], 1) != LOC_FR ) {
			LEADEV	*t = m[0];

			m[0] = m[1];
			m[1] = t;
		}
	}
	g.c = c;
	g.n = 0;
	g.reset = g.replan = FALSE;
	g.cig_set = FALSE;
	if ( nm == 2 ) {
		add_stream(m[0], 0, loc_one(m[0], 0), 1, src_of(loc_one(m[0], 0)));
		add_stream(m[1], 0, loc_one(m[1], 1), 1, src_of(loc_one(m[1], 1)));
	} else {
		LEADEV	*d = m[0];
		UW	loc = d->sink_loc & ( LOC_FL | LOC_FR );

		if ( d->nase >= 2 && loc == ( LOC_FL | LOC_FR ) ) {
			add_stream(d, 0, LOC_FL, 1, SRC_L);
			add_stream(d, 1, LOC_FR, 1, SRC_R);
		} else if ( loc == ( LOC_FL | LOC_FR ) && ( d->chans & 0x02 ) != 0 && d->frames_max >= 2 ) {
			add_stream(d, 0, LOC_FL | LOC_FR, 2, SRC_BOTH);
		} else {
			UW	a = ( loc & LOC_FL ) ? LOC_FL : loc;

			add_stream(d, 0, a, 1, ( loc == LOC_FL ) ? SRC_L : ( loc == LOC_FR ) ? SRC_R : SRC_MIX);
		}
	}
	for ( k = 0; k < NCFG; k++ ) {
		BOOL	ok = TRUE;

		for ( i = 0; i < g.n; i++ ) ok = (BOOL)( ok && cfg_fits(g.s[i].d, k, g.s[i].ch) );
		if ( ok ) break;
	}
	if ( k >= NCFG ) {
		tm_printf((UB *)"bt: \"%s\": none of the LC3 settings this end makes fits\n", m[0]->p->name);
		for ( i = 0; i < nm; i++ ) m[i]->failed = TRUE;
		g.n = 0;
		return;
	}
	g.freq = cfgs[k].code;
	g.rate = cfgs[k].rate;
	g.oct = cfgs[k].oct;
	for ( i = 0; i < g.n; i++ ) {
		STREAM	*s = &g.s[i];

		s->sdu = (UH)( g.oct * s->ch );
		knl_btbap_codec(c, s->d, s->ai, g.freq, s->alloc, g.oct);
	}
	g.st = G_CODEC;
	tm_printf((UB *)"bt: \"%s\": LE Audio of %d stream(s) on %d device(s): LC3 %d Hz, 10 ms frames of %d bytes, %d kbit/s a channel\n",
		  m[0]->p->name, g.n, nm, g.rate, g.oct, g.oct * 8 / 10);
}

LOCAL UB phy_of( UB pref )
{
	return ( pref == 0 || ( pref & PHY_2M ) != 0 ) ? PHY_2M : ( pref & PHY_1M ) ? PHY_1M : PHY_CODED;
}

/* All configured: the CIG as their preferences say */
LOCAL void cig_go( BTC *c )
{
	UB	ids[LEA_NSTREAM], phy[LEA_NSTREAM], rtn[LEA_NSTREAM];
	UH	sdu[LEA_NSTREAM];
	INT	i;

	g.framing = 0;
	g.latency = LAT_MAX;
	for ( i = 0; i < g.n; i++ ) {
		STREAM	*s = &g.s[i];
		LEASE	*a = &s->d->ase[s->ai];
		UW	pd = ( a->ppd_min != 0 ) ? a->ppd_min : a->pd_min;

		if ( pd < a->pd_min ) pd = a->pd_min;
		if ( a->pd_max != 0 && pd > a->pd_max ) pd = a->pd_max;
		s->pd = pd;
		s->phy = phy_of(a->phy);
		s->rtn = a->rtn;
		if ( a->framing != 0 ) g.framing = 1;	/* it cannot take unframed PDUs */
		if ( a->latency >= LAT_MIN && a->latency < g.latency ) g.latency = a->latency;
		ids[i] = s->cis_id;
		phy[i] = s->phy;
		rtn[i] = s->rtn;
		sdu[i] = s->sdu;
	}
	g.st = G_CIG;
	knl_btiso_cig(c, SDU_US, g.framing, g.latency, g.n, ids, sdu, phy, rtn);
}

LOCAL BOOL all_ase( UB state )
{
	INT	i;

	for ( i = 0; i < g.n; i++ ) {
		if ( g.s[i].d == NULL || g.s[i].d->ase[g.s[i].ai].state != state ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL any_up( void )
{
	INT	i;

	for ( i = 0; i < g.n; i++ ) {
		if ( g.s[i].up ) return TRUE;
	}
	return FALSE;
}

/* The SDUs start: the rings emptied, the encoders made again */
LOCAL void stream_go( BTC *c )
{
	SHARED	*sh = shared(c);
	INT	i;

	if ( sh == NULL ) return;
	sh->run = FALSE;
	__sync_synchronize();
	sh->rate = g.rate;
	sh->ns = (INT)( g.rate / 100 );
	sh->oct = g.oct;
	sh->nstream = g.n;
	for ( i = 0; i < g.n; i++ ) {
		sh->src[i] = g.s[i].src;
		g.s[i].seq = 0;
	}
	sh->ph = sh->pt = sh->sh = sh->st = 0;
	sh->lost = sh->sent = 0;
	g.made = 0;
	g.start_ms = knl_usb_ms();
	sh->gen++;
	__sync_synchronize();
	sh->run = TRUE;
	g.st = G_STREAM;
	tm_printf((UB *)"bt: \"%s\": LE Audio streaming\n", name_of());
}

/* The streams stopped: the ASEs disabled, the CISes let go */
LOCAL void stop( BTC *c )
{
	INT	i;

	if ( sh_room != NULL ) sh_room->run = FALSE;
	for ( i = 0; i < g.n; i++ ) {
		STREAM	*s = &g.s[i];

		if ( s->d != NULL && s->d->ase[s->ai].state >= ASE_ENABLING && s->d->ase[s->ai].state <= ASE_DISABLING ) {
			knl_btbap_disable(c, s->d, s->ai);
		}
		if ( s->up ) knl_btiso_drop(c, s->cis);
		s->path = FALSE;
	}
	if ( sh_room != NULL && g.st == G_STREAM ) {
		tm_printf((UB *)"bt: \"%s\": LE Audio stopped: %d frames sent, %d lost\n", name_of(), sh_room->sent, sh_room->lost);
	}
	g.st = G_STOPPING;
}

/* The group given up, to be planned again with what is there */
LOCAL void regroup( BTC *c )
{
	if ( g.st >= G_ENABLING ) stop(c);
	g.reset = TRUE;
	g.reset_due = knl_btx_now() + RESET_TMO_MS;
}

/* What has changed: the next step of the group, if it has come */
LOCAL void step( BTC *c )
{
	INT	i;

	if ( g.st >= G_READY ) {
		for ( i = 0; i < g.n; i++ ) {
			LEADEV	*d = g.s[i].d;
			UB	a = ( d != NULL ) ? d->ase[g.s[i].ai].state : ASE_IDLE;

			if ( a == ASE_IDLE || a == ASE_RELEASING || a == ASE_CODEC ) {
				if ( !g.reset ) {
					tm_printf((UB *)"bt: \"%s\": an ASE went back to state %d\n", name_of(), a);
					regroup(c);
				}
				return;
			}
		}
	}
	switch ( g.st ) {
	case G_CODEC:
		if ( all_ase(ASE_CODEC) ) cig_go(c);
		break;
	case G_QOS:
		if ( all_ase(ASE_QOS) ) {
			g.st = G_READY;
			tm_printf((UB *)"bt: \"%s\": LE Audio ready, an output\n", name_of());
		}
		break;
	case G_ENABLING:
		for ( i = 0; i < g.n; i++ ) {
			UB	a = g.s[i].d->ase[g.s[i].ai].state;

			if ( a != ASE_ENABLING && a != ASE_STREAMING ) return;
		}
		{
			UH	cis[LEA_NSTREAM], acl[LEA_NSTREAM];

			for ( i = 0; i < g.n; i++ ) {
				cis[i] = g.s[i].cis;
				acl[i] = g.s[i].d->p->handle;
			}
			g.st = G_CIS;
			knl_btiso_cis_create(c, g.n, cis, acl);
		}
		break;
	case G_CIS:
		for ( i = 0; i < g.n; i++ ) {
			if ( !g.s[i].up || !g.s[i].path || g.s[i].d->ase[g.s[i].ai].state != ASE_STREAMING ) return;
		}
		stream_go(c);
		break;
	case G_STOPPING:
		if ( any_up() ) break;
		for ( i = 0; i < g.n; i++ ) {
			if ( g.s[i].d != NULL && g.s[i].d->ase[g.s[i].ai].state != ASE_QOS ) return;
		}
		if ( !g.reset ) {
			g.st = G_READY;
			if ( g.replan ) regroup(c);
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- from bt_bap.c and bt_iso.c */

EXPORT void knl_btlea_dev_ready( BTC *c, LEADEV *d )
{
	INT	i;

	if ( g.c != c ) {
		knl_memset(&g, 0, sizeof(g));
		g.c = c;
	}
	if ( g.n == 0 && !g.reset ) {
		plan(c);
		return;
	}
	/* the other of a pair: the group planned again with both, once it is not playing */
	for ( i = 0; i < g.n; i++ ) {
		LEADEV	*e = g.s[i].d;

		if ( e != NULL && e != d && e->in_set && d->in_set && same_bt_bytes(e->sirk, d->sirk, 16) ) {
			tm_printf((UB *)"bt: \"%s\": the other of the set of \"%s\"\n", d->p->name, e->p->name);
			if ( g.st >= G_ENABLING ) g.replan = TRUE;
			else regroup(c);
			return;
		}
	}
}

EXPORT void knl_btlea_dev_down( BTC *c, LEADEV *d )
{
	INT	i;
	BOOL	mine = FALSE;

	if ( g.c != c ) return;
	for ( i = 0; i < g.n; i++ ) {
		if ( g.s[i].d == d ) {
			g.s[i].d = NULL;
			mine = TRUE;
		}
	}
	if ( !mine ) return;
	tm_printf((UB *)"bt: \"%s\": gone from the LE Audio group\n", d->p->name);
	regroup(c);
}

EXPORT void knl_btlea_ase( BTC *c, LEADEV *d, INT ai )
{
	INT	i;

	if ( g.c != c ) return;
	for ( i = 0; i < g.n; i++ ) {
		if ( g.s[i].d == d && g.s[i].ai == ai ) {
			UB	a = d->ase[ai].state;

			/* enabled after the stop was asked: disabled now that it can be */
			if ( g.st == G_STOPPING && ( a == ASE_ENABLING || a == ASE_STREAMING ) ) knl_btbap_disable(c, d, ai);
			step(c);
			return;
		}
	}
}

EXPORT void knl_btlea_cp_fail( BTC *c, LEADEV *d, INT ai, UB op, UB code, UB reason )
{
	tm_printf((UB *)"bt: \"%s\": ASE %d refused operation %d: response %02x, reason %02x\n",
		  d->p->name, d->ase[ai].id, op, code, reason);
	d->failed = TRUE;
	if ( g.c == c && g.n > 0 && !g.reset ) regroup(c);
}

EXPORT void knl_btlea_cig_done( BTC *c, INT st, INT n, CONST UH *handles )
{
	INT	i;

	if ( g.c != c || g.st != G_CIG ) return;
	if ( st == 0 ) g.cig_set = TRUE;
	if ( st != 0 || n < g.n ) {
		tm_printf((UB *)"bt: \"%s\": the CIG not set up (%02x)\n", name_of(), st);
		for ( i = 0; i < g.n; i++ ) {
			if ( g.s[i].d != NULL ) g.s[i].d->failed = TRUE;
		}
		g.st = G_NONE;
		regroup(c);
		return;
	}
	g.st = G_QOS;
	if ( g.reset ) return;		/* only for it to be removed */
	for ( i = 0; i < g.n; i++ ) {
		STREAM	*s = &g.s[i];

		s->cis = handles[i];
		knl_btbap_qos(c, s->d, s->ai, CIG_ID, s->cis_id, SDU_US, g.framing, s->phy, s->sdu, s->rtn, g.latency, s->pd);
	}
}

EXPORT void knl_btlea_cis_up( BTC *c, UH h, INT st )
{
	STREAM	*s = stream_of_cis(h);

	if ( g.c != c || s == NULL ) return;
	if ( st != 0 ) {
		if ( g.st == G_CIS ) {
			tm_printf((UB *)"bt: \"%s\": CIS %d not made (%02x)\n", name_of(), h, st);
			stop(c);
			step(c);
		}
		return;
	}
	s->up = TRUE;
	if ( g.st == G_CIS ) knl_btiso_path(c, h);
	else knl_btiso_drop(c, h);		/* it is no longer wanted */
}

EXPORT void knl_btlea_cis_failed( BTC *c, INT st )
{
	if ( g.c != c || g.st != G_CIS ) return;
	tm_printf((UB *)"bt: \"%s\": the CISes not made (%02x)\n", name_of(), st);
	stop(c);
	step(c);
}

EXPORT void knl_btlea_path_done( BTC *c, UH h, INT st )
{
	STREAM	*s = stream_of_cis(h);

	if ( g.c != c || s == NULL || g.st != G_CIS ) return;
	if ( st != 0 ) {
		tm_printf((UB *)"bt: CIS %d: its data path not set up (%02x)\n", h, st);
		stop(c);
		step(c);
		return;
	}
	s->path = TRUE;
	step(c);
}

EXPORT void knl_btlea_cis_down( BTC *c, UH h, INT reason )
{
	STREAM	*s = stream_of_cis(h);

	if ( g.c != c || s == NULL ) return;
	s->up = FALSE;
	s->path = FALSE;
	if ( g.st == G_STREAM || g.st == G_CIS ) {
		tm_printf((UB *)"bt: CIS %d gone, reason %02x\n", h, reason);
		stop(c);
	}
	step(c);
}

EXPORT void knl_btlea_cig_removed( BTC *c, INT st )
{
	if ( g.c != c ) return;
	if ( st == 0 ) g.cig_set = FALSE;
	if ( g.st == G_REMOVING ) {
		g.st = G_NONE;
		g.n = 0;
		g.reset = FALSE;
	}
}

/* ---------------------------------------------------------------- the controller's task */

/* The SDUs made, onto their CISes while the controller has room for all of a frame's */
LOCAL void send_sdus( BTC *c )
{
	SHARED	*sh = sh_room;

	while ( sh != NULL && g.st == G_STREAM && sh->sh != sh->st ) {
		INT	i, need = 0;

		for ( i = 0; i < g.n; i++ ) need += ( g.s[i].sdu + 4 + c->iso_len - 1 ) / c->iso_len;
		if ( c->iso_room < need ) return;
		for ( i = 0; i < g.n; i++ ) {
			STREAM	*s = &g.s[i];

			(void)knl_btiso_send(c, s->cis, s->seq, sh->sdu[sh->sh][i], s->sdu);
			s->seq++;
		}
		__sync_synchronize();
		sh->sh = ( sh->sh + 1 ) % NSDU;
		sh->sent++;
	}
}

/* Each round of the controller's task (from bt_le.c) */
EXPORT void knl_btlea_tick( BTC *c )
{
	INT	i;

	knl_btbap_tick(c);
	if ( g.c != c ) {
		if ( g.c != NULL && g.st != G_NONE ) return;
		knl_memset(&g, 0, sizeof(g));
		g.c = c;
	}
	if ( g.reset && g.st != G_CIG && g.st != G_REMOVING ) {
		if ( any_up() && (W)( knl_btx_now() - g.reset_due ) < 0 ) return;
		for ( i = 0; i < g.n; i++ ) g.s[i].up = FALSE;
		if ( g.cig_set ) {
			g.st = G_REMOVING;
			knl_btiso_cig_remove(c);
		} else {
			g.st = G_NONE;
			g.n = 0;
			g.reset = FALSE;
		}
	}
	if ( g.st == G_NONE && !g.reset ) plan(c);
	if ( g.want_play && g.st == G_READY && g.tries < PLAY_TRIES ) {
		g.tries++;
		for ( i = 0; i < g.n; i++ ) knl_btbap_enable(c, g.s[i].d, g.s[i].ai);
		g.st = G_ENABLING;
	} else if ( !g.want_play && g.st >= G_ENABLING && g.st <= G_STREAM ) {
		stop(c);
		step(c);
	}
	if ( g.vol_req && g.st >= G_READY ) {
		g.vol_req = FALSE;
		for ( i = 0; i < g.n; i++ ) {
			if ( g.s[i].d != NULL && ( i == 0 || g.s[i].d != g.s[0].d ) ) knl_btbap_volume(c, g.s[i].d, g.vol);
		}
	}
	send_sdus(c);
}

/* The controller gone, with the manager's lock held: the output goes */
EXPORT void knl_btlea_gone( BTC *c )
{
	if ( g.c != c ) return;
	if ( sh_room != NULL ) sh_room->run = FALSE;
	g.st = G_NONE;
	g.n = 0;
	g.c = NULL;
}

/* ---------------------------------------------------------------- the sound device's side */

LOCAL BOOL id_of( UB *id )
{
	INT	i;

	for ( i = 0; i < g.n; i++ ) {
		PEER	*p = ( g.s[i].d != NULL ) ? g.s[i].d->p : NULL;

		if ( p != NULL ) {
			(void)tm_sprintf(id, (UB *)"le-%02x%02x%02x%02x%02x%02x",
					 p->addr[5], p->addr[4], p->addr[3], p->addr[2], p->addr[1], p->addr[0]);
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL INT lea_outputs( SDOut *o, INT max )
{
	UB	id[SND_ID_MAX];

	if ( max < 1 || g.c == NULL || g.st < G_READY || !id_of(id) ) return 0;
	knl_snd_out_set(&o[0], id, name_of(), SND_BE_BT, 0);
	return 1;
}

LOCAL BOOL lea_can_record( void )
{
	return FALSE;
}

LOCAL ER lea_play_start( CONST UB *id, UW *rate, INT *channels )
{
	UB	mine[SND_ID_MAX];

	if ( g.c == NULL || g.st < G_READY || id == NULL || !id_of(mine) ) return E_NOEXS;
	if ( knl_strcmp((CONST char *)id, (CONST char *)mine) != 0 ) return E_NOEXS;
	*rate = g.rate;
	*channels = 2;
	g.tries = 0;
	g.want_play = TRUE;
	(void)tk_sig_sem(g.c->sem, 1);
	return E_OK;
}

LOCAL void lea_play_stop( void )
{
	g.want_play = FALSE;
	if ( g.c != NULL ) (void)tk_sig_sem(g.c->sem, 1);
}

LOCAL BOOL lea_play_running( void )
{
	return (BOOL)( g.c != NULL && g.st >= G_READY );
}

/* The frames due by the clock, from the mixer, into the ring of PCM */
LOCAL void lea_poll( void )
{
	SHARED	*sh = sh_room;
	UD	due;
	INT	n = 0;

	if ( sh == NULL || g.st != G_STREAM || !sh->run ) return;
	due = (UD)(UW)( knl_usb_ms() - g.start_ms ) / ( SDU_US / 1000 ) + LEAD_FRAMES;
	while ( g.made < due && n < POLL_FRAMES ) {
		UW	next = ( sh->pt + 1 ) % NPCM;

		g.made++;
		n++;
		if ( next == sh->ph ) {
			(void)knl_snd_mix((UB *)pcm_drop, sh->ns, sh->rate, 2, FALSE);
			sh->lost++;
			continue;
		}
		(void)knl_snd_mix((UB *)sh->pcm[sh->pt], sh->ns, sh->rate, 2, FALSE);
		__sync_synchronize();
		sh->pt = next;
	}
	if ( n > 0 && enc_sem > 0 ) (void)tk_sig_sem(enc_sem, 1);
}

LOCAL ER lea_rec_start( UW rate, UW *dev_rate, INT *channels )
{
	(void)rate;
	(void)dev_rate;
	(void)channels;
	return E_NOSPT;
}

LOCAL void lea_rec_stop( void ) {}
LOCAL BOOL lea_rec_running( void ) { return FALSE; }

LOCAL BOOL out_level( INT dn )
{
	return (BOOL)( dn == DN_SDVOLMAIN || dn == DN_SDVOLPCM || dn == DN_SDGAINOUT );
}

/* The volume of the first device, 0 to 255 there */
LOCAL ER lea_get_level( INT dn, SDVol *v )
{
	if ( !out_level(dn) || g.n == 0 || g.s[0].d == NULL || g.s[0].d->vol_cp == 0 ) return E_NOSPT;
	v->lv = v->rv = (UH)( ( g.s[0].d->vol << 8 ) | g.s[0].d->vol );
	return E_OK;
}

LOCAL ER lea_set_level( INT dn, CONST SDVol *v )
{
	if ( !out_level(dn) || g.c == NULL || g.n == 0 || g.s[0].d == NULL || g.s[0].d->vol_cp == 0 ) return E_NOSPT;
	g.vol = (UB)( v->lv >> 8 );
	g.vol_req = TRUE;
	(void)tk_sig_sem(g.c->sem, 1);
	return E_OK;
}

/* Enabling and the CISes take a moment, and stopping would cut what follows soon after */
EXPORT CONST SNDHW knl_lea_hw = {
	SND_BE_BT,
	lea_outputs, lea_can_record,
	lea_play_start, lea_play_stop, lea_play_running,
	lea_rec_start, lea_rec_stop, lea_rec_running,
	lea_poll, lea_get_level, lea_set_level,
	300, 2000
};
