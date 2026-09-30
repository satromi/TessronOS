/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsmozc.cc
 *	The conversion engine's C interface (include/ts/tsmozc.h)
 *
 *	The engine inside is MozcKserver (mozc/src/btron), which speaks the
 *	BTRON conversion-server protocol: it takes a BTRON key event and
 *	answers with the TIP record,
 *	its text in TRON code with the clauses as offsets in characters.
 *	Here the record is turned into UTF-8 with byte offsets, which is
 *	what the desktop draws and inserts.
 */

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "btron/kserver_protocol.h"
#include "btron/mozc_kserver.h"
#include "btron/tc_codec.h"
#include "data_manager/data_manager.h"
#include "engine/engine.h"
#include "ts/tsmozc.h"

using mozc::btron::EVENT;
using mozc::btron::KTRLIST;
using mozc::btron::KTROUT;
using mozc::btron::MozcKserver;
using mozc::btron::TC;
using mozc::btron::UW;
using mozc::btron::W;

namespace {

std::unique_ptr<MozcKserver> engine;
std::string note;

/* The TIP record, turned into the composition as the desktop takes it */
void to_out(const KTROUT &k, W status, tsmozc_out *out)
{
	const mozc::btron::TIPREC &rec = k.tip;
	std::string text;
	int n_cl = rec.n_cl;

	std::memset(out, 0, sizeof(*out));
	if ( n_cl > TSMOZC_CL_MAX ) {
		n_cl = TSMOZC_CL_MAX;
	}
	out->n_cl = n_cl;
	out->n_out = ( rec.n_out < n_cl ) ? rec.n_out : n_cl;
	out->clause = static_cast<int>(rec.clause & ~mozc::btron::TIP_YOMIMOD);
	out->yomi = ( rec.clause & mozc::btron::TIP_YOMIMOD ) != 0;
	out->caret = 0;

	/* each clause in turn, and where each begins in bytes */
	for ( int c = 0; c < n_cl && rec.cnv != nullptr && rec.cl_cnv != nullptr; c++ ) {
		const W from = rec.cl_cnv[c], to = rec.cl_cnv[c + 1];

		out->cl[c] = static_cast<int>(text.size());
		for ( W i = from; i < to; i++ ) {
			const TC t = rec.cnv[i];

			if ( t == 0xffa2 ) {
				i += 4;		/* a width annotation, not a letter */
				continue;
			}
			if ( static_cast<UW>(i) == rec.caret ) {
				out->caret = static_cast<int>(text.size());
			}
			std::string one = mozc::btron::TcToUtf8(&t, 1);
			if ( text.size() + one.size() + 1 > sizeof(out->text) ) {
				break;
			}
			text += one;
		}
		if ( static_cast<UW>(to) == rec.caret ) {
			out->caret = static_cast<int>(text.size());
		}
	}
	out->cl[n_cl] = static_cast<int>(text.size());
	std::memcpy(out->text, text.c_str(), text.size() + 1);

	if ( status > 0 ) {
		const UW s = static_cast<UW>(status);

		if ( s & mozc::btron::KTR_OUT )	out->flags |= TSMOZC_OUT;
		if ( s & mozc::btron::KTR_CNV )	out->flags |= TSMOZC_CNV;
		if ( s & mozc::btron::KTR_CAR )	out->flags |= TSMOZC_CAR;
		if ( s & mozc::btron::KTR_CL )	out->flags |= TSMOZC_CL;
		if ( ( s & mozc::btron::KTR_LSTCHG ) == mozc::btron::KTR_LSTCHG ) {
			out->flags |= TSMOZC_LIST | TSMOZC_LISTCHG;
		} else if ( s & mozc::btron::KTR_LSTREQ ) {
			out->flags |= TSMOZC_LIST;
		}
		out->sel = static_cast<int>(s & mozc::btron::KTR_SELMSK);
	}
}

/* A key event as the engine takes it */
int event(int kid, const EVENT &ev, tsmozc_out *out)
{
	KTROUT k;
	W r;

	if ( !engine ) {
		return -1;
	}
	std::memset(&k, 0, sizeof(k));
	r = engine->EventTrans(kid, ev, &k);
	if ( r == mozc::btron::kErrCkey ) {
		return TSMOZC_NOTMINE;
	}
	if ( r < 0 ) {
		return -1;
	}
	to_out(k, r, out);

	return static_cast<int>(out->flags);
}

}  // namespace

extern "C" int tsmozc_start(const void *dict, unsigned long size)
{
	if ( engine ) {
		return 0;
	}
	if ( dict == nullptr || size == 0 ) {
		return -1;
	}
	MozcKserver::UseBtronProfile();
	auto data = mozc::DataManager::CreateFromArray(
		absl::string_view(static_cast<const char *>(dict), size),
		MOZC_DATASET_MAGIC_NUMBER_LENGTH);
	if ( !data.ok() ) {
		return -1;
	}
	auto made = mozc::Engine::CreateEngine(*std::move(data));
	if ( !made.ok() ) {
		return -1;
	}
	engine = std::make_unique<MozcKserver>(*std::move(made));
	note = MozcKserver::profile_note;

	return 0;
}

extern "C" const char *tsmozc_note(void)
{
	std::string more = MozcKserver::TakeNote();

	if ( !more.empty() ) {
		note += "\n" + more;
	}
	return note.c_str();
}

extern "C" int tsmozc_open(int mode)
{
	W m = ( mode & TSMOZC_M_ROMAN ) ? mozc::btron::TIP_ROMAN1 : mozc::btron::TIP_KANA;

	if ( !engine ) {
		return -1;
	}
	/* composing straight away; the text is only letters */
	m |= mozc::btron::TIP_CNVMD | mozc::btron::TIP_TCONLY;
	return static_cast<int>(engine->OpenSession(m));
}

extern "C" int tsmozc_input(int kid, int mode)
{
	if ( !engine ) {
		return -1;
	}
	return static_cast<int>(engine->Control(kid, ( mode & TSMOZC_M_ROMAN )
		? mozc::btron::KCM_ROMAMODE : mozc::btron::KCM_KANAMODE));
}

extern "C" void tsmozc_close(int kid)
{
	if ( engine ) {
		(void)engine->CloseSession(kid);
	}
}

extern "C" int tsmozc_key(int kid, unsigned int code, unsigned int stat,
			  tsmozc_out *out)
{
	EVENT ev;

	std::memset(&ev, 0, sizeof(ev));
	ev.type = mozc::btron::EV_KEYDWN;
	ev.data.key.code = static_cast<TC>(code);
	ev.stat = stat;

	return event(kid, ev, out);
}

extern "C" int tsmozc_choose(int kid, int n, tsmozc_out *out)
{
	EVENT ev;

	std::memset(&ev, 0, sizeof(ev));
	ev.type = static_cast<W>(mozc::btron::TIP_EVENT | mozc::btron::EV_KEYDWN);
	ev.data.key.code = mozc::btron::KC_CNV;
	ev.data.key.keytop = static_cast<mozc::btron::UH>(n);

	return event(kid, ev, out);
}

extern "C" int tsmozc_list(int kid, char *buf, int size, int *p_count,
			   int *p_first, int *p_total)
{
	std::vector<uint8_t> room(mozc::btron::kBaseSizeOfKTRLIST + 8192 * sizeof(TC));
	KTRLIST *lst = reinterpret_cast<KTRLIST *>(room.data());
	W chosen;
	int at = 0, count = 0;

	if ( !engine || buf == nullptr || size <= 0 ) {
		return -1;
	}
	chosen = engine->ListTrans(kid, lst, 8192);
	if ( chosen < 0 ) {
		return -1;
	}
	/* each candidate, as UTF-8 ending in 0 */
	for ( W i = 0, n = 0; n < lst->cont && i < lst->len; n++ ) {
		W e = i;

		while ( e < lst->len && lst->sel[e] != 0 ) {
			e++;
		}
		std::string one = mozc::btron::TcToUtf8(&lst->sel[i], static_cast<size_t>(e - i));
		if ( at + static_cast<int>(one.size()) + 1 > size ) {
			break;
		}
		std::memcpy(buf + at, one.c_str(), one.size() + 1);
		at += static_cast<int>(one.size()) + 1;
		count++;
		i = e + 1;
	}
	if ( p_count != nullptr ) *p_count = count;
	if ( p_first != nullptr ) *p_first = lst->first;
	if ( p_total != nullptr ) *p_total = lst->total;

	return static_cast<int>(chosen);
}

extern "C" void tsmozc_flush(void)
{
	if ( engine ) {
		(void)engine->EndTrans(0);
	}
}
