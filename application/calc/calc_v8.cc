/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_v8.cc
 *	基本表計算: the formulas worked out in V8 (design 11.13)
 *
 *	One isolate and one context for the program's life. jstat.min.js,
 *	formula.min.js and calc_engine.js, which the program carries in
 *	itself (calc_js.S), are run in it when it starts; after that each
 *	call of calc_eng.h is a call of a method of the object "calc" the
 *	last of them made. The engine runs in the program's own thread, with
 *	its compilers to machine code; nothing it does reaches outside the
 *	context.
 *
 *	Built by tools/browser/build.sh v8app of the engine's tree, which
 *	gives this file the compiler's flags of V8's own and links V8 in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <memory>

#include "include/libplatform/libplatform.h"
#include "include/v8-context.h"
#include "include/v8-function.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-object.h"
#include "include/v8-primitive.h"
#include "include/v8-exception.h"
#include "include/v8-message.h"
#include "include/v8-persistent-handle.h"
#include "include/v8-script.h"

#include "calc_eng.h"

extern "C" {
extern const char calc_js_jstat[], calc_js_jstat_end[];
extern const char calc_js_formula[], calc_js_formula_end[];
extern const char calc_js_engine[], calc_js_engine_end[];
}

static std::unique_ptr<v8::Platform>	platform;
static v8::Isolate			*iso;
static v8::ArrayBuffer::Allocator	*allocator;
static v8::Global<v8::Context>		g_ctx;
static v8::Global<v8::Object>		g_calc;
static std::string			g_name = "V8";

/* What went wrong in the script, on the console */
static void said( v8::Local<v8::Context> ctx, v8::TryCatch &tc, const char *what )
{
	v8::String::Utf8Value	why(iso, tc.Exception());

	printf("calc: %s: %s (line %d)\n", what, *why ? *why : "?",
	       tc.Message().IsEmpty() ? 0 : tc.Message()->GetLineNumber(ctx).FromMaybe(0));
}

static bool run( v8::Local<v8::Context> ctx, const char *src, const char *end, const char *name )
{
	v8::TryCatch		tc(iso);
	v8::Local<v8::String>	s;
	v8::Local<v8::Script>	script;
	v8::Local<v8::Value>	r;

	if ( !v8::String::NewFromUtf8(iso, src, v8::NewStringType::kNormal, (int)( end - src )).ToLocal(&s)
	  || !v8::Script::Compile(ctx, s).ToLocal(&script) || !script->Run(ctx).ToLocal(&r) ) {
		said(ctx, tc, name);
		return false;
	}
	return true;
}

int ce_start( void )
{
	static const char flags[] = "--single-threaded --stack-size=900";

	if ( iso != nullptr ) return 0;
	v8::V8::SetFlagsFromString(flags, sizeof(flags) - 1);
	v8::V8::InitializeICU();
	platform = v8::platform::NewSingleThreadedDefaultPlatform();
	v8::V8::InitializePlatform(platform.get());
	v8::V8::Initialize();
	g_name = std::string("V8 ") + v8::V8::GetVersion();

	v8::Isolate::CreateParams	params;

	allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
	params.array_buffer_allocator = allocator;
	iso = v8::Isolate::New(params);
	v8::Isolate::Scope	is(iso);
	v8::HandleScope		hs(iso);
	v8::Local<v8::Context>	ctx = v8::Context::New(iso);
	v8::Context::Scope	cs(ctx);

	if ( !run(ctx, calc_js_jstat, calc_js_jstat_end, "jstat.min.js")
	  || !run(ctx, calc_js_formula, calc_js_formula_end, "formula.min.js")
	  || !run(ctx, calc_js_engine, calc_js_engine_end, "calc_engine.js") ) {
		return -1;
	}
	v8::Local<v8::Value>	v;
	v8::Local<v8::String>	name = v8::String::NewFromUtf8Literal(iso, "calc");

	if ( !ctx->Global()->Get(ctx, name).ToLocal(&v) || !v->IsObject() ) {
		printf("calc: the engine's object is not there\n");
		return -1;
	}
	g_ctx.Reset(iso, ctx);
	g_calc.Reset(iso, v.As<v8::Object>());
	return 0;
}

void ce_end( void )
{
	if ( iso == nullptr ) return;
	g_calc.Reset();
	g_ctx.Reset();
	iso->Dispose();
	iso = nullptr;
	delete allocator;
	v8::V8::Dispose();
	v8::V8::DisposePlatform();
}

const char *ce_name( void )
{
	return g_name.c_str();
}

static v8::Local<v8::Value> str( const unsigned char *s )
{
	return v8::String::NewFromUtf8(iso, (const char *)s).ToLocalChecked();
}

/* calc.<method>(args...): its answer, or an empty handle when it threw */
static v8::MaybeLocal<v8::Value> call( v8::Local<v8::Context> ctx, const char *method, int argc,
				       v8::Local<v8::Value> *argv )
{
	v8::TryCatch		tc(iso);
	v8::Local<v8::Object>	calc = g_calc.Get(iso);
	v8::Local<v8::Value>	f, r;

	if ( !calc->Get(ctx, v8::String::NewFromUtf8(iso, method).ToLocalChecked()).ToLocal(&f) || !f->IsFunction()
	  || !f.As<v8::Function>()->Call(ctx, calc, argc, argv).ToLocal(&r) ) {
		if ( tc.HasCaught() ) said(ctx, tc, method);
		return v8::MaybeLocal<v8::Value>();
	}
	return r;
}

#define ENTER()							\
	if ( iso == nullptr ) return -1;			\
	v8::Isolate::Scope	is(iso);			\
	v8::HandleScope		hs(iso);			\
	v8::Local<v8::Context>	ctx = g_ctx.Get(iso);		\
	v8::Context::Scope	cs(ctx)

static int cell_call( const char *method, int col, int row, const unsigned char *text )
{
	ENTER();
	v8::Local<v8::Value>	argv[3] = { v8::Integer::New(iso, col), v8::Integer::New(iso, row),
					    text != nullptr ? str(text) : v8::Undefined(iso).As<v8::Value>() };

	return call(ctx, method, text != nullptr ? 3 : 2, argv).IsEmpty() ? -1 : 0;
}

int ce_set( int col, int row, const unsigned char *text )
{
	return cell_call("set", col, row, text);
}

int ce_clear( int col, int row )
{
	return cell_call("clear", col, row, nullptr);
}

int ce_blank( int col, int row )
{
	return cell_call("blank", col, row, nullptr);
}

int ce_move( int cols, int at, int d )
{
	ENTER();
	v8::Local<v8::Value>	argv[3] = { v8::Boolean::New(iso, cols != 0), v8::Integer::New(iso, at),
					    v8::Integer::New(iso, d) };

	return call(ctx, "move", 3, argv).IsEmpty() ? -1 : 0;
}

int ce_reset( void )
{
	ENTER();
	return call(ctx, "reset", 0, nullptr).IsEmpty() ? -1 : 0;
}

int ce_bulk( int on )
{
	ENTER();
	v8::Local<v8::Value>	argv[1] = { v8::Boolean::New(iso, on != 0) };

	return call(ctx, "bulk", 1, argv).IsEmpty() ? -1 : 0;
}

/* Records "col\x1Frow\x1Fwords" joined by \x1E */
int ce_taken( CE_FN fn, void *arg )
{
	ENTER();
	v8::Local<v8::Value>	r;
	int			n = 0;

	if ( !call(ctx, "taken", 0, nullptr).ToLocal(&r) ) return -1;
	v8::String::Utf8Value	all(iso, r);
	const char		*p = *all, *end = p + all.length();

	while ( p != nullptr && p < end ) {
		const char	*rec = (const char *)memchr(p, 0x1E, (size_t)( end - p ));
		const char	*re = ( rec != nullptr ) ? rec : end;
		const char	*f1 = (const char *)memchr(p, 0x1F, (size_t)( re - p ));
		const char	*f2 = ( f1 != nullptr ) ? (const char *)memchr(f1 + 1, 0x1F, (size_t)( re - f1 - 1 )) : nullptr;

		if ( f1 != nullptr && f2 != nullptr ) {
			std::string	w(f2 + 1, (size_t)( re - f2 - 1 ));

			fn(atoi(p), atoi(f1 + 1), (const unsigned char *)w.c_str(), (int)w.size(), arg);
			n++;
		}
		p = ( rec != nullptr ) ? rec + 1 : end;
	}
	return n;
}

int ce_test( const unsigned char *text, const unsigned char *pat, int regex )
{
	ENTER();
	v8::Local<v8::Value>	argv[3] = { str(text), str(pat), v8::Boolean::New(iso, regex != 0) }, r;

	if ( !call(ctx, "test", 3, argv).ToLocal(&r) ) return -1;
	return r->Int32Value(ctx).FromMaybe(-1);
}

int ce_replace( const unsigned char *text, const unsigned char *pat, const unsigned char *rep, int regex,
		unsigned char *out, int max )
{
	ENTER();
	v8::Local<v8::Value>	argv[4] = { str(text), str(pat), str(rep), v8::Boolean::New(iso, regex != 0) }, r;

	if ( max <= 0 || !call(ctx, "replace", 4, argv).ToLocal(&r) ) return -1;
	v8::String::Utf8Value	s(iso, r);
	int			n = s.length() < max - 1 ? s.length() : max - 1;

	memcpy(out, *s, (size_t)n);
	out[n] = 0;
	return s.length();		/* the whole, which may be more than was written */
}
