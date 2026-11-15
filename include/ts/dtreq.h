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
#define DT_RQ_TOOL	2		/* the accessory (小物), or the system's tool (屑実身操作 ...), of this name started */

/*
 * DT_RQ_OPEN with DT_F_AUTO in flags (自動起動): the object opened as a
 * link that asks to be opened with the window of the record that holds
 * it, when that window opened. It is opened by no act of the person: it
 * does not take the keys, and the windows opened together stand in the
 * order of their links, grab_y the place of this one (1 the first), the
 * asking window in front with the keys. A request with DT_F_AUTO and no
 * target says that every link of the asking window that is to be opened
 * so has been asked for (grab_y how many): the first window's say when
 * what the system opens as it starts is all asked.
 */
#define DT_F_AUTO	0x0010

/*
 * The first window (最初のウインドウ, the cabinet BTRON the system opens
 * with) is a window of 仮身一覧, a process, which the desktop starts as
 * it starts and starts again when it ended while the session goes on.
 * Its object is SYSDEF_CABINET.
 *
 *   DT_RQ_FIRST_CLOSE	the first window is to close (閉じる, Ctrl+E):
 *			closing it ends the session. While the session is
 *			not ending, the person is asked whether the system
 *			is to end (OSを終了しますか？) in a panel over win:
 *			E_ABORT, and the window stays as it is -- with はい
 *			the system ends, and asks the window to close when
 *			every other program has ended. While the session
 *			ends, E_OK: the window closes, saving what changed
 *
 * A channel named with DT_RQ_TELL for first-window (the first window's)
 * is told DT_HOW_LINK_PUT when something the system made is to be put
 * on the desktop (a file printed, a screen saved, a disk attached):
 * into the object, text its name. The window puts a closed link to it
 * under what is there and saves the record.
 */
#define DT_RQ_FIRST_CLOSE 49
#define DT_HOW_LINK_PUT	15
#define DT_TELL_FIRST	"first-window"

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
 * E_OACV: the asking process may not read target, and no link is made.
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
 *			comes back in look.bgcol. The pop-up has the
 *			switch 仮身属性と同じ, on when the object's metadata
 *			gives no paper of its own; kept on, the paper is
 *			taken out of the metadata and look.bgcol comes back
 *			DT_PAPER_SAME: the object is laid on its link's
 *			表示領域背景 (or the system's look), as one that
 *			never had a paper. look.bgcol TAD_COL_NONE in the
 *			request asks with no colour to start from
 *   DT_RQ_FULL		全画面表示: win made the size of the screen above
 *			the message line, or, when it is that already by
 *			全画面表示 and has not been sized or moved since,
 *			put back where it stood before. The desktop keeps
 *			where it stood, as it does for a double press on
 *			the corner that sizes the window, which does the
 *			same; the answer has DT_F_FULL in flags when the
 *			window is now the screen's size. A program that
 *			puts the window back itself still finds it made
 *			the screen's size the next time it asks. With
 *			DT_F_ASK in flags the window is left as it is, and
 *			the answer only says whether it is so
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
#define DT_PAPER_SAME	0xFFFFFFFFU	/* DT_RQ_PAPER's look.bgcol: 仮身属性と同じ, no paper of its own (TAD_COL_NONE) */
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
 * file is given. The desktop starts 基本印刷 (DT_RQ_PRJOB), whose panels
 * ask how, which shows the pages when asked; the desktop writes the file
 * and puts a link to it on the desktop. The answer comes when the job
 * ends: E_OK when it was written, E_ABORT when the panel was put away.
 * One asked for while another is worked waits its turn. The object is
 * the process's to throw away after the answer.
 *
 * The pages lie on white unless flags has DT_F_PR_PAPER: then on
 * look.bgcol, the colour the window shows the paper in, as a window of
 * the desktop's prints its own. A document's pages are laid as 詳細
 * shows them with DT_F_PR_DETAIL, and with its hidden letters with
 * DT_F_PR_HIDDEN, as the window shows them.
 */
#define DT_RQ_PRINT	21
#define DT_F_PR_PAPER	0x8000
#define DT_F_PR_DETAIL	0x10000
#define DT_F_PR_HIDDEN	0x20000

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
 * 31 to 33 were the requests by which a program had the desktop play
 * 実時間制御TAD in its window. The program plays it itself now, with
 * the player it links (include/ts/rtplay.h); the numbers are not used
 * again.
 */

#define DT_F_HIDDEN	0x0002		/* the link is hidden (隠蔽仮身) */
#define DT_F_KEEP	0x0004		/* the link keeps to its program (起動アプリの固定) */
#define DT_F_FULL	0x1000000	/* DT_RQ_FULL answered: the window is the size of the screen */

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
 * has none), or, when neither names any, those that open an object of
 * its kind. DT_RQ_APPS asks
 * for the entry at recno (0 up) of target's list: the answer's how is
 * how many there are, text the entry's program id, a tab and its name
 * (UTF-8), and flags DT_F_KIND for one offered for the object's kind;
 * E_NOEXS past the last. DT_RQ_EXEC with DT_F_KIND opens the object with
 * such a one and leaves the program it opens with as it is. For an
 * object open already (in any window, or in one being started on it) the
 * list is empty -- how 0, E_NOEXS, flags DT_F_OPEN -- and 実行 is grey:
 * one object is open in one window, and opening it again, whatever
 * program is asked for, brings that window forward.
 */
#define DT_RQ_APPS	34

/*
 * DT_RQ_COLOUR: a colour chosen with the system's panel over the window
 * rq->win, starting from look.bgcol. Answers the colour in look.bgcol, or
 * E_ABORT when it was abandoned. Nothing is written anywhere. With
 * DT_F_AT in flags the panel comes up with its corner at (grab_x,
 * grab_y) of the screen -- under the part that showed the colour, as
 * the system's own parts bring it up -- and otherwise at the pointer.
 */
#define DT_RQ_COLOUR	36
#define DT_F_AT		0x4000
#define DT_F_KIND	0x0400

/*
 * DT_RQ_CARRY with DT_F_TEXT in flags: what is carried is letters, the
 * line of UTF-8 in text, not a link. Let go over a text of the desktop's
 * they go in where the outline's corner stood (E_OK); over the window of
 * another program that takes letters (OB_E_TEXT) they are dropped on it,
 * with DT_F_TO_PROG in the answer's flags. Unless the second button went
 * down on the way, that program is first asked whether it takes letters
 * there (wm_obj_ask_text): E_OK with how 0 when it said yes (a move),
 * E_NOEXS when it said no or did not answer; one that does not take the
 * question is offered them, E_OK with how DT_HOW_COPY, since it may yet
 * refuse them and the asking one keeps its letters; anywhere else
 * E_NOEXS, and over the asking program's
 * own window E_NOEXS and nothing said, as for a link. how says
 * DT_HOW_COPY when the second button went down on the way: the program
 * keeps its letters; else it takes them away (a move).
 * Links let go over another program's window (DT_F_TO_PROG) are dropped
 * with the outline's corner as the drop's x, y, each link at its place in
 * the outline, as they are put in a window of the desktop's.
 */
#define DT_F_TEXT	0x0008
#define DT_CARRY_TEXT	512		/* bytes of the letters carried, with their terminator */

/*
 * DT_RQ_CARRY with DT_F_TO_PROG in flags: the links carried may also be
 * let go over the window of another program that takes links (OB_E_DROP).
 * The answer's flags say DT_F_TO_PROG when it went there: the other
 * program speaks on the message line, and the asking one says nothing of
 * its own about the link having gone.
 * A program sets it when the objects stand by themselves whatever the
 * other program answers: links it already shows, not objects it would
 * make for the carry, which nothing would link to when the drop is
 * refused. target names the object when there is one link, 0 when there
 * are several. Unless the second button went down on the way, the other
 * program is first asked whether it takes them there (wm_obj_ask): the
 * answer is E_OK with how 0 when it said yes (a move: the program takes
 * its links away once they are placed), E_NOEXS when it said no or did
 * not answer. One that does not take the question is not asked, and the
 * answer is E_OK with how DT_HOW_COPY, since it may yet refuse (the
 * program keeps its link); into is 0. The
 * program then asks DT_RQ_PLACE as usual, and the last DT_RQ_PLACE drops
 * all the links on that window (wm_obj_drop_for). E_OACV, with the reason
 * on the message line: the asking process or the taker may not read the
 * object, and nothing was dropped.
 */
#define DT_F_TO_PROG	0x0800

/*
 * DT_RQ_CARRY with DT_F_ASK beside DT_F_TO_PROG: the program makes the
 * objects only once they would be taken (書庫管理's 取り出し). Let go over
 * another program's window, the desktop first asks that program whether
 * it would take links there (wm_obj_ask, OB_E_ASK) and answers only when
 * it has said: E_OK with DT_F_TO_PROG and into the object the window
 * shows when it would, E_NOEXS when it would not or does not answer in a
 * while, or does not take the question. Over a window of the desktop's
 * the desktop answers for it as without DT_F_ASK. Once answered E_OK the
 * program makes the objects and asks DT_RQ_PLACE; when that fails, it
 * deletes what it made, so that nothing is left that no link reaches.
 */
#define DT_F_ASK	0x40000

/*
 * DT_RQ_CARRY with DT_F_FRAG in flags: what is carried out of a text is
 * also in the tray's place of its own (TR_PUT, one record of TR_TAD) as
 * a <document> fragment, with its look: letters (with DT_F_TEXT, their
 * line in text as well), or a picture or a figure standing in the text
 * alone (no DT_F_TEXT; look.w, look.h its box and grab_x, grab_y where
 * the hand took it in that box). Let go over a text of the desktop's,
 * the fragment goes in as it looked: letters where the outline's corner
 * is, a picture or a figure where the hand is. A picture or a figure let
 * go over a figure of the desktop's is put there where its box is, as
 * from a text of the desktop's; anywhere else it is not taken (E_NOEXS).
 * Letters elsewhere go as without DT_F_FRAG. The asking program empties
 * TR_PUT once answered.
 *
 * Let go back over the asking program's own window, a carry is answered
 * E_NOEXS with DT_F_BACK in the answer's flags and look.w, look.h the
 * place on the screen where the hand let go: the program moves (or, with
 * how DT_HOW_COPY, copies) what it carried within its window there, as
 * when it had not gone out.
 */
#define DT_F_FRAG	0x80000
#define DT_F_BACK	0x100000

/*
 * DT_RQ_CARRY with DT_F_BASE in flags: target is a template, carried
 * out of a list of templates (原紙集め). Let go where a link can go --
 * a window of the desktop's that takes links, or with DT_F_TO_PROG the
 * window of another program that takes them -- the desktop makes the
 * new object from the template there, as for one carried out of a
 * 原紙箱 (the name asked for, starting with the template's), and puts
 * the link to it where the hand let go, looking as look. The answer is
 * E_OK with the new object in into, E_ABORT when the name was not given;
 * the program asks no DT_RQ_PLACE.
 */
#define DT_F_BASE	0x200000

/*
 * A program window opened from a link of a window of the desktop's acts
 * on that link as the desktop's own windows do. The program learns the
 * link from its start-up argument (T_PRCOPEN); what it asks names it in
 * text, the UUID of the object whose record holds the link, a tab, and
 * the link's vobjid (both as ts_uuid_text writes them). target is the
 * object the program opened, and the link must point at it.
 *
 *   DT_RQ_SAVE_NEW	新たな実身に保存: the object (saved by the program
 *			first) copied under a name asked for in a panel over
 *			win, and a link to the copy put with the link, in a
 *			text just after it. into names the copy; E_ABORT when
 *			the panel was put away
 *   DT_RQ_LINK_VIEW	the window closing: where it was scrolled to
 *			(grab_x, grab_y), how large (look.w, per cent; 0
 *			leaves it), whether its lines wrapped (look.h, the
 *			wordWrap: TAD_WRAP_ON or TAD_WRAP_OFF; TAD_WRAP_NONE
 *			leaves it) and how it was shown (name, the viewMode;
 *			empty leaves it) written into the link, as a window
 *			of the desktop's does when it closes
 */
#define DT_RQ_SAVE_NEW	37
#define DT_RQ_LINK_VIEW	38

/*
 * DT_RQ_OPEN with DT_F_LINK in flags: the object opened from a link of
 * the asking program's own window. text begins with the UUID of the
 * object whose record holds the link (the one the program has open), a
 * tab, the link's vobjid, a tab, and a channel the program reads, then
 * a line feed; with DT_F_APPLIST the applist comes after it. look says
 * the link's colours, name its viewMode and grab_x its wordWrap
 * (TAD_WRAP_*): the window opened is shown as from a link of a window of
 * the desktop's (its paper the link's bgcol when it has none of its own,
 * a text as the link says it is to be shown). The window
 * opened acts on that link as on one of a window of the desktop's
 * (DT_RQ_SAVE_NEW, DT_RQ_LINK_VIEW, a window of the desktop's closing),
 * but what would be written into the record, which the program holds
 * while it runs, is written to that channel as a T_DTANS with the
 * request's seq, for the program to put into its record itself; then
 * the channel is told OB_E_CHANGE on record 0 (to whoever asked
 * notices of it, ob_ntf_evt). text begins with the UUID of the object
 * whose record holds the link and a tab, as it was asked; how says what
 * it is:
 *   DT_HOW_LINK_VIEW	into is the object the link points at; text goes
 *			on with the link's vobjid, then, each after a tab, scrollx,
 *			scrolly, the zoom (per cent, 0 leaves it), the
 *			wordWrap (TAD_WRAP_*, TAD_WRAP_NONE leaves it) and
 *			the viewMode (empty leaves it)
 *   DT_HOW_LINK_ADD	新たな実身に保存: into is the copy; text goes on
 *			with the vobjid of the link a link to the copy goes with, made like
 *			it, just after it in a text. Nothing links to the
 *			copy yet: the link counts when the record is saved,
 *			as every link does
 * Once the program has ended, the desktop writes into the record as for
 * a window of its own. A program that does not ask so is not told, and
 * a window opened from its links does not act on them.
 */
#define DT_F_LINK	0x2000
#define DT_HOW_LINK_VIEW 10
#define DT_HOW_LINK_ADD	11

/*
 * How the link looks, told to the windows opened from it before the
 * record holding it is saved:
 *   DT_RQ_ATTR with DT_F_LINK	仮身属性変更 of a link of the asking
 *			program's own window, text beginning as DT_RQ_OPEN's
 *			with DT_F_LINK does (the object whose record holds the
 *			link, its vobjid, the program's channel, a line feed;
 *			the applist after it with DT_F_APPLIST). Answered as
 *			DT_RQ_ATTR is; when it was agreed to, every window
 *			watching that link is told the answer's look
 *   DT_RQ_LINK_WATCH	the asking program's window, opened from a link
 *			(T_PRCOPEN's parent and vobjid), told how it looks: text
 *			is the object whose record holds the link, a tab, its
 *			vobjid, a tab, a channel the program reads, a line
 *			feed; target the object the window shows. Told until
 *			the process ends or the channel is gone. Asked again
 *			by the same window (〈再表示〉), the answer's how is
 *			DT_HOW_LINK_LOOK and its look the link's last told
 *			since the window opened, when it was told one
 *   DT_HOW_LINK_LOOK	what such a window is told at that channel, as a
 *			T_DTANS: into the object it shows, look the link's
 *			look as 仮身属性変更 left it (its bgcol the
 *			表示領域背景, the window's paper when it has none of its
 *			own), text the object holding the link, a tab, its vobjid;
 *			then the channel is told OB_E_CHANGE on record 0
 */
#define DT_RQ_LINK_WATCH 50
#define DT_HOW_LINK_LOOK 16

/*
 * DT_RQ_LET_GO: what was carried within the asking program's window was
 * let go outside it, at (grab_x, grab_y) on the screen, and only links
 * may leave a window. Over another program's window that shows an
 * object (the first window, a cabinet, a text), that window takes the
 * keys and the answer is E_OK with into its
 * object: the program leaves what it carried where it was and lets it
 * go, as when nothing is carried there. Anywhere else E_NOEXS, and
 * nothing is done.
 */
#define DT_RQ_LET_GO	39

/*
 * DT_RQ_LINK_STATE: how a program's link stands, for its 仮身操作 and
 * 実身操作 to be greyed as the desktop greys them for a link of its own.
 * Asked as DT_RQ_ATTR is (DT_F_APPLIST with the link's applist). The
 * answer's flags: DT_F_OPEN when the object is open, in a window of the
 * desktop's or of a program, or in one being started on it (〈開く〉
 * grey, 〈閉じる〉 not), DT_F_LOCKABLE when there is
 * a program the link opens with, which 起動アプリの固定 fixes.
 */
#define DT_RQ_LINK_STATE 40
#define DT_F_OPEN	0x400000
#define DT_F_LOCKABLE	0x800000
/*
 * and, in the same answer, for ディスク操作's items: DT_F_DETACHABLE when
 * the link is a disk's that 切り離し may take away, DT_F_GROWABLE when
 * 拡張 may make the volume of its object larger
 */
#define DT_F_DETACHABLE	0x2000000
#define DT_F_GROWABLE	0x4000000

/*
 * The 小物箱 (application/accbox, a process) shows the accessories
 * of the program box in the order of 小物の設定, as the desktop lays out
 * its lists, and asks the desktop for them and for what the person
 * changes of them.
 *
 *   DT_RQ_ACC_LIST	the accessory at place recno (0 the first) of those
 *			in the store: how is the number of them, into its
 *			program object, text its name, DT_F_HIDDEN in flags
 *			for one 小物 does not list (the program lays the list
 *			out itself, as the system's lists are, tvlist.h).
 *			E_NOEXS past the last; recno -1 answers the number
 *			alone. Whenever the list changes -- the order, what
 *			小物 lists, an accessory installed or deleted -- the
 *			channels named with DT_RQ_TELL for accessory-box are
 *			told DT_HOW_ACC
 *   DT_RQ_ACC_SET	a change of 小物の設定 for the accessory target:
 *			name is a code of 小物管理 (acc.top, acc.up,
 *			acc.down, acc.bottom, acc.show, acc.hide), or
 *			acc.place with text the accessory it goes before
 *			(empty: after them all), as when it is carried within
 *			the 小物箱. E_PAR for another code
 */
#define DT_RQ_ACC_LIST	41
#define DT_RQ_ACC_SET	42
#define DT_HOW_ACC	12

/*
 * 管理情報 (application/realinfo) is a process of its own for each
 * object, started by the desktop on the link it was chosen on (openArg
 * "link"); asked again for the same object, the desktop brings its
 * window to the front. What it shows it reads itself, and it sets the
 * protection itself (ob_set_prt, with the rights of the person it runs
 * for). What only the desktop keeps it asks for that link, target the
 * object, and only the process started on it is answered (E_PAR for
 * any other):
 *
 *   DT_RQ_APPS with DT_F_INFO	as DT_RQ_APPS, the list given whether the
 *			object is open or not, text the program's id (not
 *			the applist's key) and its name, DT_F_DEFAULT in the
 *			answer's flags for the one it opens with
 *   DT_RQ_INFO_APPS	the object's applist written (付箋指定's 適用): text is
 *			the place of the one it opens with (-1 none), then
 *			each program after a line feed, its id, a tab and its
 *			name
 *   DT_RQ_INFO_KEEP	起動アプリの固定 of the link, fixed with DT_F_KEEP in
 *			flags or let go without, in the window it was chosen
 *			in. The answer's flags say DT_F_LOCKABLE when it can be
 *			(that window still open and not sealed, a program the
 *			link opens with) and DT_F_KEEP when it is fixed; with
 *			DT_F_ASK nothing is changed, and look.w, look.h are
 *			where that window's outer corner stands on the screen
 *			(-1 when it is gone), for 管理情報's window to stand
 *			down and right of it. E_NOEXS when it cannot be
 */
#define DT_RQ_INFO_APPS	70
#define DT_RQ_INFO_KEEP	71
#define DT_F_INFO	0x40000000
#define DT_F_DEFAULT	0x20000000

/*
 * What any program running as a process is told, and what the system's
 * tools (application/systool: 屑実身操作, 実身/仮身検索, 仮身ネットワーク,
 * ウインドウ一覧) ask of the desktop for what only it can do; they make
 * their lists themselves, from what they may read.
 *
 *   DT_RQ_TELL		name is the program's id, text a channel of the
 *			program's (as ts_uuid_text writes it): until the
 *			channel goes, the desktop writes a T_DTANS there and
 *			tells the channel OB_E_CHANGE -- how DT_HOW_AGAIN
 *			when the program is run again while a window of it
 *			stands for its program object (the window is brought
 *			to the front with the keys first; 仮身ネットワーク:
 *			into the object to start from), DT_HOW_TOOL_REMADE
 *			when what a tool's list shows may have changed
 *			(屑実身操作: a record saved, a package installed or
 *			deleted; ウインドウ一覧: the windows on the screen or
 *			their order), DT_HOW_ACC when the accessories the
 *			小物箱 (accessory-box) shows changed. E_PAR for an id
 *			that is no program's
 *   DT_RQ_PURGE	屑実身操作's 削除: the object target thrown away for
 *			good, the windows showing it closed first; what its
 *			records linked to is linked to that much less. Only
 *			a 屑実身 (no link reaches it, and the system does not
 *			hold it): E_OBJ for another; E_OACV when the asking
 *			process may not delete it
 *   DT_RQ_RECOUNT	屑実身操作's 再探索: every count of links made again
 *			from the records, the objects the system holds counted
 *			once each. text answers "TOTAL<tab>CHANGED": how many
 *			objects there are and how many counts were put right
 *   DT_RQ_ROOTS	what the system itself holds that no record links to
 *			(the first window's cabinet, the システム箱, the
 *			templates of the programs): the one at place recno in
 *			into, how the number of them. E_NOEXS past the last;
 *			recno -1 answers the number alone
 *   DT_RQ_WIN_LIST	ウインドウ一覧: the window at place recno (0 the
 *			nearest) of those that show an object -- a program's
 *			that says what object it shows, not a panel, not the
 *			asking process's own, not a tool's -- with how the
 *			number of them, into the object and text the window's
 *			title. E_NOEXS past the last; recno -1 answers the
 *			number alone. With DT_F_OPEN in flags, the window that
 *			shows target is brought to the front with the keys,
 *			or the object opened as a double click opens it when
 *			no window shows it
 */
#define DT_RQ_TELL	43
#define DT_RQ_TOOL_TELL	DT_RQ_TELL
#define DT_RQ_PURGE	44
#define DT_RQ_RECOUNT	45
#define DT_RQ_WIN_LIST	46
#define DT_RQ_ROOTS	48
#define DT_HOW_TOOL_REMADE 13
#define DT_HOW_AGAIN	14
#define DT_HOW_TOOL_AGAIN DT_HOW_AGAIN

/*
 * ディスク操作 (application/diskop, design 11.26): what the program asks
 * of the desktop, which alone knows the disks, the volumes and the
 * windows open on them. recno says what (DT_DISK_*); target is the
 * object of the link the item was chosen on, 0 for none.
 *   DT_DISK_LIST	the partitions 接続 offers, the one at grab_x (0 up):
 *			how is how many, text its device name, a tab and its
 *			row; DT_F_DISK_PICK in flags on the one chosen first
 *			(the disk of target, a 虚身's object, when it is in).
 *			E_NOEXS past the last. The list is made when grab_x
 *			is 0, and kept for the rest
 *   DT_DISK_ATTACH	the partition name names attached, the link to its
 *			top put in the first window, and remembered as
 *			attached: text the disk's name. E_OBJ when the same
 *			volume is attached already, E_NOSPT for files (what
 *			ファイル変換 reads), E_PAR for a kind not handled
 *   DT_DISK_DETACH	the volume of target taken away: text the disk's
 *			name. E_NOEXS when it is not attached, E_RONLY for the
 *			system's, E_BUSY with how DT_HOW_DISK_WIN (an object
 *			of it in a window), DT_HOW_DISK_PROG (used by a
 *			program) or DT_HOW_DISK_CHECK (ディスク修復 checks it)
 *   DT_DISK_STATUS	how full the disk of target is (the system's for 0):
 *			text "NAME<tab>BYTES<tab>USED<tab>OBJECTS<tab>GARBAGE"
 *   DT_DISK_FREE	the free stretches 区画作成 offers, the one at grab_x:
 *			how is how many, text "DISK<tab>FIRST<tab>LAST<tab>ROW"
 *			(FIRST and LAST sectors), DT_F_DISK_SYS on a stretch
 *			of the system's disk. Made at grab_x 0 as for LIST
 *   DT_DISK_MAKE	a partition made on the disk name names, in the free
 *			stretch that begins at the sector text says, grab_x
 *			megabytes (0 the whole stretch), its volume labelled
 *			with what follows a tab in text; attached and
 *			remembered as ATTACH does: text the disk's name.
 *			E_NOEXS when there is no such stretch now, E_PAR for a
 *			size it cannot be; how DT_HOW_DISK_MADE when it was
 *			made but not attached
 *   DT_DISK_GROWS	what 拡張 can do to the volume of target (the
 *			system's for 0): text "NAME<tab>PATH<tab>NOW<tab>MOST"
 *			(MB), DT_F_DISK_SYS for the system's. E_NOSPT when it
 *			is not a native volume, E_OBJ when it is not a
 *			partition of a GPT, E_LIMIT when nothing is free after
 *			it (text said all the same)
 *   DT_DISK_GROW	that volume made grab_x megabytes: text "FROM<tab>TO"
 *			(MB). E_PAR for a size it cannot be made, E_BUSY with
 *			DT_HOW_DISK_CHECK while it is being checked
 *   DT_DISK_LOG	ディスク修復's 障害情報 saved: target's record 0, a
 *			document the program made (ui_prn_open), written as
 *			the text object ディスク修復記録 linked from the system
 *			box (/SYS). Only for the program ディスク修復 (E_OACV)
 *   DT_DISK_MENU	an item of ディスク操作 chosen in a window of a program
 *			(name its code: disk.connect, disk.status,
 *			disk.create, disk.grow, disk.detach), on the link to
 *			target (0 none): the program ディスク操作 started
 *			with it, as for the first window's menu
 *   DT_DISK_REMOVE	the link to target deleted from the first window
 *			(取り外し), text the volume it was saved on (tvol, as
 *			ts_uuid_text writes it): a disk's link takes the
 *			volume away and forgets it, so that it is not attached
 *			again at the next start; a link to a disk not attached
 *			forgets it. E_OK when the link may go (one that is no
 *			disk's too), else why not, the person told in a panel
 *			over win, and the link stays. Only the first window's
 *   DT_DISK_RESCUE	a program whose window was asked to close because the
 *			disk of its object went, holding changes not saved:
 *			grab_x 0 asks for an object to hand them over in
 *			(into; grab_y 1 for a figure, 0 a text), which the
 *			program may write record 0 of; grab_x 1, text that
 *			object (as ts_uuid_text writes it), hands them over:
 *			the person is asked over win where they go, as for
 *			any window whose disk went, the object is thrown
 *			away, and the program closes its window without
 *			saving. E_PAR from a program not so asked
 * ATTACH, DETACH, MAKE and GROW are an administrator's: a process that is
 * not one is asked the password for (as DT_RQ_ADMIN asks, and is given
 * the rights as it gives them), E_ABORT or E_OACV when it is not given.
 */
#define DT_RQ_DISK	47

/*
 * システム登録 (application/sysinst, a process, x64 only; design 3.10.10,
 * 11.32): the disks the system may be put on, the plan and the copy are
 * the kernel's (application/sysinst/sysinst.c, si_core.c). recno says
 * what (DT_SI_*):
 *   DT_SI_DISK		the disk at grab_x (0 up): how is how many, text its
 *			device name, a tab and its row. The list is made
 *			again when grab_x is 0. E_NOEXS past the last;
 *			E_NOSPT on a machine that is not a PC
 *   DT_SI_PART		the partition at grab_y of the disk at grab_x that
 *			区画を1つ消して使う may take: how is how many, text its
 *			subunit, a tab and its row; DT_F_SI_BEST on the one
 *			offered first (the largest Windows volume)
 *   DT_SI_PLAN		the way grab_x (DT_SI_COEXIST, DT_SI_ERASE,
 *			DT_SI_REPLACE) on the disk name names, with the
 *			partition of subunit grab_y for DT_SI_REPLACE, looked
 *			at: how is 0 when it can be taken, and the plan is
 *			kept, look.w the lines of what is asked before it is
 *			done; else why not, said in text
 *   DT_SI_LINE		line grab_y of what grab_x says (DT_SI_L_*): how is
 *			how many lines there are
 *   DT_SI_RUN		the plan kept done, in a task of the desktop's, which
 *			runs to its end whatever the program does. An
 *			administrator's: a process that is not one is asked
 *			the password for, as DT_RQ_ADMIN asks. E_BUSY while
 *			one runs, E_OBJ when no plan is kept that can be taken
 *   DT_SI_STATE	how it goes: how the step, DT_F_SI_RUN while it runs,
 *			DT_F_SI_OVER once it is over, with look.w what it ended
 *			with (below 0 an error) and look.h the step it failed
 *			at (-1 none)
 *   DT_SI_RESTART	the machine started again: only once it went well
 *   DT_SI_KEEPQ	whether the way grab_x on the disk name names (the
 *			partition of subunit grab_y for DT_SI_REPLACE) can put
 *			the system over the TessronOS there, keeping its
 *			objects (上書き): how is 0 when it can, DT_SI_K_NONE
 *			when there is no TessronOS there to keep, else why
 *			not, said in text
 * DT_SI_KEEP or'd in grab_x of DT_SI_PLAN asks for that way; its answer
 * has look.h 1 when what the disk holds goes (the warning panel).
 */
#define DT_RQ_SYSINST	80
#define DT_SI_DISK	0
#define DT_SI_PART	1
#define DT_SI_PLAN	2
#define DT_SI_LINE	3
#define DT_SI_RUN	4
#define DT_SI_STATE	5
#define DT_SI_RESTART	6
#define DT_SI_KEEPQ	7
#define DT_SI_KEEP	0x100		/* or'd in grab_x: the objects there kept (上書き) */
#define DT_SI_K_NONE	(-1)		/* DT_SI_KEEPQ: no TessronOS there */
#define DT_SI_COEXIST	0		/* beside Windows, in the free stretch */
#define DT_SI_ERASE	1		/* the whole disk */
#define DT_SI_REPLACE	2		/* one partition made the TessronOS one */
#define DT_SI_L_ASK	0		/* what is asked before the plan is done */
#define DT_SI_L_RUN	1		/* what is under way */
#define DT_SI_L_RESULT	2		/* what came of it */
#define DT_SI_ROWS	16		/* lines at most of any of them */
#define DT_F_SI_BEST	0x8000000
#define DT_F_SI_RUN	0x10000000
#define DT_F_SI_OVER	0x80000000
#define DT_DISK_LIST	0
#define DT_DISK_ATTACH	1
#define DT_DISK_DETACH	2
#define DT_DISK_STATUS	3
#define DT_DISK_FREE	4
#define DT_DISK_MAKE	5
#define DT_DISK_GROWS	6
#define DT_DISK_GROW	7
#define DT_DISK_LOG	8
#define DT_DISK_MENU	9
#define DT_DISK_REMOVE	10
#define DT_DISK_RESCUE	11
#define DT_F_DISK_PICK	0x2000000
#define DT_F_DISK_SYS	0x4000000
#define DT_HOW_DISK_WIN		20
#define DT_HOW_DISK_PROG	21
#define DT_HOW_DISK_CHECK	22
#define DT_HOW_DISK_MADE	23

/*
 * The start-up argument of ディスク操作 (dt_disk_menu): the code of the
 * item of the menu ディスク操作 chosen (disk.connect, disk.detach,
 * disk.status, disk.create, disk.grow), and the object of the link it was
 * chosen on, 0 away from a link.
 */
typedef struct {
	UB	code[16];
	TS_UUID	target;
} T_DTDISKARG;

/*
 * 基本印刷 (application/bprint, design 11.22) works the panels of a
 * print job the desktop holds -- the pages of DT_RQ_PRINT, an object
 * printed from the 実行 menu, pages of the desktop's own -- and asks the
 * desktop for what needs the pages. The desktop starts it for the job;
 * only a process of that program, and then only the one that took the
 * job, is answered. recno says what (DT_PJ_*). Every step but the first
 * carries the setting in text: the thirteen numbers of T_PRNSET (ts/
 * print.h) apart by spaces, in its order (all from to copies quality
 * colour paper_w paper_h rotate scale links expand depth).
 *   DT_PJ_JOB		the job taken: text the setting to start from, a
 *			tab, and what is printed (the file's name); how the
 *			pages of layout paper; flags DT_F_PJ_SHOW when 画面表示
 *			is offered, DT_F_PJ_SET when only the setting is
 *			asked for (〈印刷〉 writes nothing yet). E_NOEXS when
 *			no job waits; E_BUSY when another 基本印刷 works it,
 *			which is brought to the front
 *   DT_PJ_PAPER	how the setting lays the pages: text "W H PCT SHEETS
 *			LW LH" -- the printing paper as it lies and the
 *			layout paper in thousandths of a point, the
 *			magnification 自動 makes in hundredths of a percent,
 *			the sheets that will be printed; how the pages
 *			(which 仮身: 印刷 changes for pages with links)
 *   DT_PJ_SHOW		sheet grab_x (0 up) drawn in the work area of win, a
 *			window of the asking process's, below grab_y: the
 *			whole sheet in the middle on grey, or with
 *			DT_F_PJ_ZOOM twice as large about (look.w, look.h)
 *			of it. The answer's look.w and look.h are the sheet
 *			as shown, how the sheets
 *   DT_PJ_OUT		the file written and its link put in the desktop's
 *			first window (with DT_F_PJ_SET: the setting taken);
 *			the job ends, and the one who asked is answered E_OK.
 *			E_NOEXS when the setting leaves no sheet
 *   DT_PJ_END		the job put away (〈取消〉): its asker answered
 *			E_ABORT; the setting is kept for the next time
 * A job whose 基本印刷 ends before OUT or END is put away as by END.
 */
#define DT_RQ_PRJOB	60
#define DT_PJ_JOB	0
#define DT_PJ_PAPER	1
#define DT_PJ_SHOW	2
#define DT_PJ_OUT	3
#define DT_PJ_END	4
#define DT_F_PJ_SHOW	0x10000000
#define DT_F_PJ_SET	0x20000000
#define DT_F_PJ_ZOOM	0x40000000

/*
 * 画面印刷 (application/scrprint, design 11.24): the panel is the
 * program's, the screen and the input the desktop's. recno says what
 * (DT_SP_*); what is set travels as text, "TO POINTER AREA BY SECS",
 * a tab, the volume a figure object goes on ("" the first), a tab, and
 * its name -- TO 0 図形実身へ保存, 1 トレーへ複写, 2 プリンタへ印刷; AREA
 * 0 全画面, 1 最前面ウィンドウのみ, 2 範囲指定; BY 0 指定時間後に実行,
 * 1 PrntScrnキーで実行, 2 PrntScrnキーで連続実行.
 *   DT_SP_GET		the panel coming up: what was under way stops, and
 *			text answers what was set last (the machine's
 *			setting SCRPRINT); how counts the panels shown.
 *			E_BUSY when another 画面印刷's panel is up, which is
 *			brought to the front
 *   DT_SP_VOL		the volume at grab_x (0 the system's) a figure
 *			object can go on: text its path, a tab and its name
 *			(empty when it has none); how how many. E_NOEXS past
 *			the last
 *   DT_SP_ARM		実行 with what text sets, kept for the next time:
 *			printing asks its 印刷設定 of 基本印刷, a rectangle is
 *			dragged out, then the count or the key, and the
 *			screen taken and sent where it is set to go. Only
 *			the process whose panel is up
 *   DT_SP_STOP		取り消し: what was under way stopped, and what text
 *			sets kept when it is not empty
 */
#define DT_RQ_SCRPRINT	61
#define DT_SP_GET	0
#define DT_SP_VOL	1
#define DT_SP_ARM	2
#define DT_SP_STOP	3

/*
 * The start-up argument of a program whose program object says
 * "openArg": "link" in tessronos.program (ts_get_arg): the object to
 * open first, as every program is given it, and after it the link it
 * was opened from. A program without "openArg" is given the object's
 * UUID alone, sizeof(TS_UUID) bytes.
 * Opened from a link, fromlink is set and bgcol, wordwrap and viewmode
 * are the link's as it is when the object is opened -- with what a
 * window that closed wrote back into it, before its record is saved --
 * for the window to be shown as the link asks, as a window of the
 * desktop's is.
 */
#define DT_VIEW_MAX	16		/* bytes of a viewMode, with its terminator (TAD_VIEW_MAX) */

typedef struct {
	TS_UUID	target;			/* the object to open */
	TS_UUID	parent;			/* the object whose record holds the link; 0 none */
	TS_UUID	vobjid;			/* the link's vobjid there */
	INT	recno;			/* the record to open, -1 the first */
	BOOL	fromlink;		/* opened from a link: the three below are its */
	UINT	bgcol;			/* 0x00rrggbb, 0xFFFFFFFF none (TAD_COL_NONE) */
	INT	wordwrap;		/* TAD_WRAP_* */
	UB	viewmode[DT_VIEW_MAX];	/* empty when it says none */
} T_PRCOPEN;

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
	INT	grab_x, grab_y;		/* DT_RQ_CARRY: where the press held the outline, from its corner */
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
