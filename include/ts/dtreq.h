/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtreq.h
 *
 *	What a process asks of the desktop. The desktop keeps a channel
 *	named DT_REQ_NAME; a process opens it for writing and writes one
 *	T_DTREQ. The desktop does what a double click does and, when the
 *	request names a channel to answer at, writes one T_DTANS there.
 */

#ifndef __TS_DTREQ_H__
#define __TS_DTREQ_H__

#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_REQ_NAME	"tessronos.desktop"

#define DT_RQ_OPEN	1		/* the object opened as a double click opens it */
#define DT_RQ_TOOL	2		/* the accessory of this name (小物) started */

/*
 * A link carried out of the asking program's window: asked while the
 * hand is down on what the program shows as a link, the desktop carries
 * its outline until the hand lets go, and answers where. Let go over a
 * window of the desktop's that takes links (a figure, a cabinet, a
 * text), after the question in ask (when there is one) is answered
 * with yes, the answer is E_OK and names that window's object in into;
 * anywhere else, or answered no, it is E_NOEXS or E_ABORT. The program
 * then makes what the link is to point at and asks DT_RQ_PLACE with
 * the carry's seq: the link is put where the hand let go.
 */
#define DT_RQ_CARRY	3
#define DT_RQ_PLACE	4		/* a link to target put where a carry (carry) was let go */

/*
 * Several links carried as one outline: each but the last put with
 * DT_RQ_PLACE_MORE, which leaves the carry open for the next, and the
 * last with DT_RQ_PLACE. With either, grab_x and grab_y say where the
 * link's corner is from the outline's corner where it was let go (0, 0
 * the corner itself).
 */
#define DT_RQ_PLACE_MORE 7

/*
 * An application installed from a package (.tpk) and deleted
 * (システム環境設定's バージョン sheet). DT_RQ_INSTALL: target is the
 * package, an object whose record 1 is the file; recno 1 installs it
 * even when the system has the same version or a newer one. The answer
 * is E_OK with the program in into and how (DT_HOW_*), E_LIMIT when the
 * package is not newer than what the system has, E_OBJ when it is not
 * a package, E_PAR for a program of the desktop's own. DT_RQ_REMOVE:
 * target is the program object.
 */
#define DT_RQ_INSTALL	5
#define DT_RQ_REMOVE	6
#define DT_HOW_NEW	1		/* a program the system did not have */
#define DT_HOW_UPDATED	2		/* one it had, now the package's */

/*
 * The system updated from its package (.tpk with "kind": "system",
 * design 11.8), dropped on the バージョン sheet; target is the package
 * object, recno the step:
 *   DT_SYSUP_CHECK  what the package is: E_OK when it is a newer
 *                   version for this machine, E_LIMIT when it is the
 *                   same version or an older one, E_NOSPT when it is
 *                   for another machine or processor, E_OBJ when it is
 *                   not a system package. text says the package's
 *                   version, a tab, and the system's version now; with
 *                   E_NOSPT, the machine the package is for.
 *   DT_SYSUP_BEGIN  the update begun (flags 1: even when it is not
 *                   newer); only an administrator, as DT_RQ_INSTALL.
 *                   how is the steps there are, nothing done yet.
 *   DT_SYSUP_STEP   the next steps done, as many as take a moment: how
 *                   is the steps done, flags the steps there are. When
 *                   they are all done the update is over and the system
 *                   asks for a restart. Only the process that began it.
 */
#define DT_RQ_SYSUP	35
#define DT_SYSUP_CHECK	0
#define DT_SYSUP_BEGIN	1
#define DT_SYSUP_STEP	2

/*
 * The answer to DT_RQ_CARRY says in how whether the second button went
 * down while the outline was carried (複写ドラッグ): the program leaves
 * its own link where it is, as for a copy, rather than taking it away
 * as for a move. Let go over win, the asking program's own window, the
 * answer is E_NOEXS and nothing is said: what is inside its window the
 * program does itself.
 */
#define DT_HOW_COPY	3

/*
 * DT_RQ_PLACE with DT_F_DUPLICATE in flags (実身複製ドラッグ): the object
 * target is copied first, with all it holds, under a name asked for in
 * the window the outline was let go over, and the link put is to the
 * copy, which the answer names in into.
 */
#define DT_F_DUPLICATE	0x0001

/*
 * The 仮身操作 and 実身操作 of a link a program shows itself, done by the
 * desktop as for a link in a window of its own; what changes the link
 * comes back in the answer, for the program to write into its record.
 * A panel the desktop asks with is put in the middle of win, the asking
 * program's window (0: the desktop's first window). target is the
 * object the link points at.
 *
 *   DT_RQ_CLOSE	the window the object is open in, closed
 *   DT_RQ_ATTR		属性変更 of a link that looks as look (w and h its
 *			box), hidden when flags has DT_F_HIDDEN: E_OK with
 *			the look and flags chosen, E_ABORT when the panel
 *			was put away
 *   DT_RQ_RELATION	続柄設定: name is the link's own relationship. The
 *			object's tags (in brackets) are kept by the desktop;
 *			the link's come back in text
 *   DT_RQ_RENAME	実身名変更: the new name in text
 *   DT_RQ_DUPLICATE	実身複製: the name asked for, the object copied with
 *			all it holds; the copy in into
 *   DT_RQ_NEWVER	新版作成: the copy, under the same name, in into
 *   DT_RQ_INFO		管理情報 of the object, in a window of its own
 *   DT_RQ_NETWORK	仮身ネットワーク, from the object
 *   DT_RQ_SEARCH	実身/仮身検索
 *   DT_RQ_EXEC		実行: the object opened with the program whose id
 *			(its applist's key) is in name. Unless flags has
 *			DT_F_KEEP (the link keeps to the program it opens
 *			with), that one is what the object opens with from
 *			then on
 *   DT_RQ_PAPER	背景色変更: the colour asked for, starting from
 *			look.bgcol, kept as the paper of the object; it
 *			comes back in look.bgcol
 *   DT_RQ_FULL		win made the size of the screen above the message
 *			line (全画面表示); the program puts it back itself
 */
#define DT_RQ_CLOSE	8
#define DT_RQ_ATTR	9
#define DT_RQ_RELATION	10
#define DT_RQ_RENAME	11
#define DT_RQ_DUPLICATE	12
#define DT_RQ_NEWVER	13
#define DT_RQ_INFO	14
#define DT_RQ_NETWORK	15
#define DT_RQ_SEARCH	16
#define DT_RQ_EXEC	17
#define DT_RQ_PAPER	18
#define DT_RQ_FULL	19

/*
 * Letters typed for the asking program (文字検索's 入力): name holds them
 * (UTF-8, ending in 0), win is the program's window. They go into the
 * window the keys went to last before win -- a text of the desktop's at
 * its caret, or the box of a panel that has the keys -- and the answer
 * says which in how (DT_HOW_TEXT, DT_HOW_BOX), or is E_NOEXS when there
 * is no such window or it takes no letters.
 */
#define DT_RQ_TYPE	20
#define DT_HOW_TEXT	4
#define DT_HOW_BOX	5

/*
 * 印刷 of pages a process made (design 11.22): target is an object the
 * process made for them, a page of xmlTAD in each record -- a document,
 * which may run to several pages of its paper, or a figure, one sheet
 * of its paper -- and name is what is printed (UTF-8), the name the
 * file is given. The desktop asks with its panels in the middle of win,
 * shows the pages when asked, writes the file and puts a link to it on
 * the desktop; the answer is E_OK when it was written, E_ABORT when the
 * panel was put away. The object is the process's to throw away after
 * the answer.
 */
#define DT_RQ_PRINT	21

/*
 * The administrator's rights for one operation (design 8.7): name says
 * what the asking process is about to do (UTF-8). The desktop asks the
 * person, in a panel over a window of its own (never over the asking
 * program's, whose parts would take the keys too), for the
 * administrator's password -- the process never sees it -- and when it
 * is typed right the process acts as an administrator until it calls
 * ob_end_adm, for OB_ADM_MS at most. On a machine whose administrator
 * has no password yet, the panel asks for one to be chosen first. The
 * answer is E_OK (also when the process is an administrator already),
 * E_ABORT when the panel was put away, E_OACV when the password was
 * typed wrong three times. DT_RQ_INSTALL and DT_RQ_REMOVE from a
 * process that is not an administrator ask in the same way.
 */
#define DT_RQ_ADMIN	22

/*
 * 通知 (design 11.28): a notice a process shows the person, in a small
 * window of the desktop's at the screen's top right that does not take
 * the keys. text is the title, a line feed, then what it says (UTF-8);
 * name is where it comes from (a page's origin, a program's name); ask
 * holds the words of up to two buttons of the process's own, empty for
 * none; recno is the process's number for it. One with the number of a
 * notice of the same reply channel still shown takes its place. The
 * answer is at once, E_OK and DT_HOW_NOTE_SHOWN in how; then, at the
 * same reply channel with the same seq, one more answer says what came
 * of it: DT_HOW_NOTE_CLICK with flags 0 for 〈開く〉 and 1 or 2 for the
 * process's buttons, DT_HOW_NOTE_CLOSED when the person closed it
 * (〈閉じる〉, its frame, its menu), DT_HOW_NOTE_GONE when it went by
 * itself after DT_NOTE_MS. With DT_F_NOTE_CLOSE in flags the notice of
 * that number goes, and nothing more is answered for it. DT_F_NOTE_STAY:
 * it stays until it is answered.
 */
#define DT_RQ_NOTIFY	23
#define DT_F_NOTE_CLOSE	0x0010
#define DT_F_NOTE_STAY	0x0020
#define DT_HOW_NOTE_SHOWN	6
#define DT_HOW_NOTE_CLICK	7
#define DT_HOW_NOTE_CLOSED	8
#define DT_HOW_NOTE_GONE	9
#define DT_NOTE_MS	10000

/*
 * The faces of the 書体箱 (システム環境設定's 書体 sheet, design 10.3).
 * A face object is one whose data record is a font file (OpenType,
 * TrueType, or a collection of them); what each of its faces is
 * registered as is the JSON of its resource DT_FONT_RES. recno is the
 * face of the file (0 the first), text one face's settings as that
 * JSON's members are written:
 *   {"name":"詳細名","use":true,"family":"…","class":"6050","attr":"0010","base":"…"}
 *
 *   DT_RQ_FONT_REG	target copied into the system's volume and linked
 *			from the 書体箱, its face recno set as text says
 *			(nothing said: as the object's own resource says);
 *			into names the copy. With DT_F_FONT_UPDATE a face
 *			registered before of the same family and look
 *			(FN_FA_LOOK) is taken out of the box and deleted.
 *   DT_RQ_FONT_SET	face recno of target, a face of the box, set as
 *			text says
 *   DT_RQ_FONT_USE	face recno of target used, or not with DT_F_FONT_OFF;
 *			E_RONLY for the system's own face
 *   DT_RQ_FONT_DEL	target taken out of the box and deleted; E_RONLY for
 *			the object of the system's own face
 *   DT_RQ_FONT_SAVE	target copied, under a name and onto a disk asked
 *			for in a panel over win, a link to it put in that
 *			disk's top (the first window for the system's disk);
 *			into names the copy, E_ABORT when the panel was put
 *			away
 *   DT_RQ_FONT_TRY	face recno of target opened to be looked at, when it
 *			is not in use: text answers the family a document
 *			names it by. With DT_F_FONT_DONE the face opened so
 *			is closed again
 * REG, SET and DEL are an administrator's: the password is asked for as
 * for DT_RQ_INSTALL. DT_RQ_FONT_USE is the person's at the machine.
 */
#define DT_RQ_FONT_REG	24
#define DT_RQ_FONT_SET	25
#define DT_RQ_FONT_USE	26
#define DT_RQ_FONT_DEL	27
#define DT_RQ_FONT_SAVE	28
#define DT_RQ_FONT_TRY	29
#define DT_F_FONT_UPDATE 0x0040
#define DT_F_FONT_OFF	0x0080
#define DT_F_FONT_DONE	0x0100
#define DT_FONT_RES	"_0_font.json"

/*
 * 画面サイズ (システム環境設定): the screen set now, while the desktop
 * runs, to the size text says, "WIDTHxHEIGHT", and after it "aspect" or
 * "stretch" for how a size of another shape than the panel is laid on
 * it, where the screen chooses that (a PC's panel). The desktop draws
 * everything again at the new size and asks, in a panel over its own
 * window, whether it shows right: Enter or 確認 within thirty seconds
 * keeps it, and the machine's settings take it for the next start
 * (VIDEOMODE, VIDEOFIT); anything else, or nothing, puts back what
 * there was. The answer is E_OK when it was kept, E_ABORT when it was
 * put back, E_NOSPT when the screen cannot be set so, E_PAR when text
 * is not a size.
 */
#define DT_RQ_SCREEN	30

/*
 * 実時間制御TAD played in the asking program's window (design 15.5.1):
 * the scenes of such a record are documents and figures, which only the
 * desktop draws, so a program that plays media hands the record to it.
 * The desktop lays the scene out in the top left of win's work area,
 * look.w by look.h, and plays it there as it plays a slide, starting the
 * processes that sound and decode for it. win must be a window the asking
 * process made, and only that process asks about the player afterwards.
 *   DT_RQ_RT_OPEN	target's record recno (-1 the first) played, the
 *			ground round it look.bgcol (0xFFFFFFFF black). E_OBJ
 *			when the record has no <realtime>; the answer's how
 *			is the number the others name the player by
 *   DT_RQ_RT_DO	what recno says (DT_RT_*) done to player carry
 *   DT_RQ_RT_CLOSE	player carry stopped, its processes ended; it also
 *			goes by itself when win goes
 * The answers to DT_RQ_RT_OPEN and DT_RQ_RT_DO say how the program goes:
 * flags the DT_RTS_* bits, look.w its time in ms, look.h the media of its
 * playlist, text the title and the artist of the one playing. E_NOEXS:
 * there is no such player (any more).
 */
#define DT_RQ_RT_OPEN	31
#define DT_RQ_RT_DO	32
#define DT_RQ_RT_CLOSE	33

#define DT_RT_STATE	0		/* nothing done: only how it goes */
#define DT_RT_TOGGLE	1		/* 再生 / 一時停止; one waiting to be begun begins */
#define DT_RT_STOP	2		/* back to the start, and held there */
#define DT_RT_RESTART	3		/* 先頭から */
#define DT_RT_SEEK	4		/* grab_x ms forward, back when less than 0 */
#define DT_RT_TRACK	5		/* the medium of the playlist grab_x after (before) the one playing */
#define DT_RT_AREA	6		/* the scene laid out again in look.w by look.h */
#define DT_RT_REDRAW	7		/* the scene drawn again */
#define DT_RT_GROUND	8		/* 背景色変更: chosen in a panel over win */
#define DT_RT_LIST	9		/* プレイリスト表示: a panel over win, the medium chosen played */
#define DT_RT_PRESS	10		/* the first button down at grab_x, grab_y of the work area */
#define DT_RT_RELEASE	11		/* and up there */
#define DT_RT_KEY	12		/* key grab_x (a HID usage) down, grab_y DT_RT_KEY_* */
#define DT_RT_MOVE	13		/* the pointer at grab_x, grab_y of the work area (outside it: left the scene) */
#define DT_RT_KEY_UP	0x0001		/* up, not down */
#define DT_RT_KEY_MINE	0x0002		/* the program's own key: given only when the program takes it */

#define DT_RTS_PLAYING	0x0001
#define DT_RTS_PAUSED	0x0002
#define DT_RTS_WAITING	0x0004		/* it waits to be begun (autoplay="false") */
#define DT_RTS_DONE	0x0008		/* it has run to its end */
#define DT_RTS_TAKEN	0x0010		/* DT_RT_PRESS, DT_RT_RELEASE, DT_RT_KEY: a trigger or a wait of it took it */

#define DT_F_HIDDEN	0x0002		/* the link is hidden (隠蔽仮身) */
#define DT_F_KEEP	0x0004		/* the link keeps to its program (起動アプリの固定) */

/*
 * DT_RQ_OPEN and the requests about a link with DT_F_APPLIST in flags:
 * text holds the applist the link carries (the JSON of its applist
 * attribute, UTF-8, ending in 0). The desktop decides the program from
 * it as for a link of its own windows: the one the link keeps to when
 * its default entry says "defaultOpenLock": true, else the one the
 * object's applist marks. DT_RQ_EXEC with it records the program run
 * unless the link keeps to its own, and DT_F_KEEP is not looked at.
 */
#define DT_F_APPLIST	0x0200

/*
 * 実行's list for a link in a program's window, the one the desktop's
 * own links show (dt_app_list): the programs of the object's applist (of
 * the applist the link carries, given with DT_F_APPLIST, when the object
 * has none), then those that open an object of its kind. DT_RQ_APPS asks
 * for the entry at recno (0 up) of target's list: the answer's how is
 * how many there are, text the entry's program id, a tab and its name
 * (UTF-8), and flags DT_F_KIND for one offered for the object's kind;
 * E_NOEXS past the last. DT_RQ_EXEC with DT_F_KIND opens the object with
 * such a one and leaves the program it opens with as it is.
 */
#define DT_RQ_APPS	34
#define DT_F_KIND	0x0400

/*
 * DT_RQ_CARRY with DT_F_TEXT in flags: what is carried is letters, the
 * line of UTF-8 in text, not a link. Let go over a text of the desktop's
 * they go in where the outline's corner stood (E_OK); over the window of
 * another program that takes letters (OB_E_TEXT) they are dropped on it
 * (E_OK); anywhere else E_NOEXS, and over the asking program's own window
 * E_NOEXS and nothing said, as for a link. how says DT_HOW_COPY when the
 * second button went down on the way: the program keeps its letters;
 * else it takes them away (a move).
 */
#define DT_F_TEXT	0x0008
#define DT_CARRY_TEXT	512		/* bytes of the letters carried, with their terminator */

#define DT_TEXT_MAX	192		/* bytes of an answer's text, with its terminator */

/* How a link is to look: its outline while carried, and the link put */
typedef struct {
	INT	w, h;			/* the frame, in screen pixels */
	INT	chsz;			/* the letters, in points; 0 the usual */
	UINT	frcol, chcol, tbcol, bgcol;	/* 0x00rrggbb; 0xFFFFFFFF not said */
	UINT	disp;			/* what the frame shows (TAD_D_* of ts/tad.h); 0 the usual */
	UINT	autoopen;		/* it opens by itself when shown */
} T_DTLOOK;

typedef struct {
	UINT	req;			/* DT_RQ_* */
	UINT	seq;			/* given back in the answer */
	TS_UUID	target;			/* DT_RQ_OPEN: the object; DT_RQ_PLACE: what the link points at */
	INT	recno;			/* and its record, -1 the first */
	TS_UUID	reply;			/* a channel to answer at; 0 none */
	UB	name[64];		/* DT_RQ_TOOL: the accessory's name, UTF-8 */
	T_DTLOOK look;			/* DT_RQ_CARRY, DT_RQ_PLACE */
	INT	grab_x, grab_y;		/* DT_RQ_CARRY: where the hand holds the outline, from its corner */
	UB	ask[2][96];		/* DT_RQ_CARRY: the question where it is let go, UTF-8; empty: none */
	UB	yes[16];		/* and the word on the button that goes ahead; empty: 「実行」 */
	UINT	carry;			/* DT_RQ_PLACE: the seq of the DT_RQ_CARRY it ends */
	TS_UUID	win;			/* the asking program's window; 0 none said */
	UINT	flags;			/* DT_F_* */
	UB	text[DT_CARRY_TEXT];	/* DT_RQ_CARRY with DT_F_TEXT: the letters */
} T_DTREQ;

/*
 * A request written in the size it had before win and flags (a program
 * built before them) is still taken, and answered in the size the
 * answer had then, without look, flags and text. One written in the size
 * it had before text is taken as it is, with no letters.
 */
typedef struct {
	UINT	seq;
	ER	er;			/* E_OK, or why not */
	TS_UUID	proc;			/* the process it started; 0 when a window of the desktop's own opened */
	TS_UUID	into;			/* DT_RQ_CARRY: the object of the window it was let go over;
					   DT_RQ_INSTALL: the program; DT_RQ_DUPLICATE, DT_RQ_NEWVER,
					   DT_RQ_PLACE with DT_F_DUPLICATE: the copy */
	UINT	how;			/* DT_RQ_INSTALL, DT_RQ_CARRY: DT_HOW_* */
	T_DTLOOK look;			/* DT_RQ_ATTR, DT_RQ_PAPER */
	UINT	flags;			/* DT_RQ_ATTR: DT_F_HIDDEN */
	UB	text[DT_TEXT_MAX];	/* DT_RQ_RELATION, DT_RQ_RENAME: UTF-8 */
} T_DTANS;

#ifdef __cplusplus
}
#endif
#endif /* __TS_DTREQ_H__ */
