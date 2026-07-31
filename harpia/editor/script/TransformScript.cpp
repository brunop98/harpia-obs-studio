#include "TransformScript.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>

// HARPIA_HAVE_QJS is set by CMake when the vendored QuickJS is part of the
// build (harpia/third_party/quickjs). Without it the whole engine compiles out
// and scripting degrades to a no-op with a clear message rather than breaking
// the build.
#if defined(HARPIA_HAVE_QJS) && HARPIA_HAVE_QJS
#include "quickjs.h"
#endif

namespace harpia {

#if defined(HARPIA_HAVE_QJS) && HARPIA_HAVE_QJS

namespace {

// A well-behaved channel function is pure arithmetic and returns in
// microseconds. These budgets exist only so a runaway script (`while (true) {}`)
// can't wedge the UI thread or an export — they are thousands of times more than
// real work needs, so no legitimate script will ever reach them.
constexpr qint64 kCompileBudgetMs = 1000;
constexpr qint64 kCallBudgetMs = 50;

// Far beyond what a transform script can justify, but bounded, so a runaway
// allocation fails the script instead of the application.
constexpr size_t kMemoryLimit = 64u * 1024u * 1024u;

// QuickJS measures the JS stack against the native one. 256 KB leaves room even
// on the 1 MB stacks MSVC gives secondary threads.
constexpr size_t kStackLimit = 256u * 1024u;

QString toQString(JSContext *ctx, JSValueConst v)
{
	const char *s = JS_ToCString(ctx, v);
	if (!s)
		return QString();
	const QString out = QString::fromUtf8(s);
	JS_FreeCString(ctx, s);
	return out;
}

// Consumes the pending exception and renders it as one readable line.
QString takeError(JSContext *ctx)
{
	const JSValue e = JS_GetException(ctx);
	QString msg = toQString(ctx, e);
	if (msg.isEmpty())
		msg = QStringLiteral("The script failed.");

	const JSValue line = JS_GetPropertyStr(ctx, e, "lineNumber");
	double n = 0;
	if (JS_IsNumber(line) && JS_ToFloat64(ctx, &n, line) == 0 && n > 0)
		msg += QStringLiteral(" (line %1)").arg(int(n));
	JS_FreeValue(ctx, line);
	JS_FreeValue(ctx, e);
	return msg;
}

double numberOr(JSContext *ctx, JSValueConst v, double fallback)
{
	double n = 0;
	if (JS_IsNumber(v) && JS_ToFloat64(ctx, &n, v) == 0)
		return n;
	return fallback;
}

} // namespace

struct TransformEvaluator::Impl {
	// A channel the script doesn't define stays JS_UNDEFINED and is skipped.
	struct Compiled {
		JSValue position = JS_UNDEFINED;
		JSValue scale = JS_UNDEFINED;
		JSValue rotation = JS_UNDEFINED;
		JSValue opacity = JS_UNDEFINED;
	};

	JSRuntime *rt = nullptr;
	JSContext *ctx = nullptr;
	JSValue global = JS_UNDEFINED;
	QMap<QString, Compiled> scripts;
	QString lastError;

	// Armed around every JS_Eval/JS_Call; the interrupt handler reads it.
	QElapsedTimer watchdog;
	qint64 budgetMs = kCallBudgetMs;

	void release(Compiled &c)
	{
		JS_FreeValue(ctx, c.position);
		JS_FreeValue(ctx, c.scale);
		JS_FreeValue(ctx, c.rotation);
		JS_FreeValue(ctx, c.opacity);
		c.position = c.scale = c.rotation = c.opacity = JS_UNDEFINED;
	}

	// QuickJS anchors its stack check to wherever the runtime was last told the
	// stack top is. Re-anchoring on entry keeps an evaluator correct even if it
	// is used from a thread other than the one that constructed it.
	void enter(qint64 budget)
	{
		JS_UpdateStackTop(rt);
		budgetMs = budget;
		watchdog.restart();
	}
	void leave() { watchdog.invalidate(); }

	static int interrupted(JSRuntime *, void *opaque)
	{
		auto *self = static_cast<Impl *>(opaque);
		if (!self->watchdog.isValid())
			return 0;
		return self->watchdog.elapsed() > self->budgetMs ? 1 : 0;
	}

	JSValue callChannel(JSValueConst fn, double t, double u, double dur, const ScriptContext &sc,
			    const TlTransform &base)
	{
		JSValue c = JS_NewObject(ctx);
		// What the clip would be framed at WITHOUT this script: its Inspector
		// pose, or its keyframes at this instant. A script that reads it composes
		// with hand work instead of overriding it; one that ignores it keeps the
		// old absolute behaviour, so nothing already written changes.
		JSValue b = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, b, "x", JS_NewFloat64(ctx, base.posX));
		JS_SetPropertyStr(ctx, b, "y", JS_NewFloat64(ctx, base.posY));
		JS_SetPropertyStr(ctx, b, "scale", JS_NewFloat64(ctx, base.scale));
		JS_SetPropertyStr(ctx, b, "rotation", JS_NewFloat64(ctx, base.rotation));
		JS_SetPropertyStr(ctx, b, "opacity", JS_NewFloat64(ctx, base.opacity));
		JS_SetPropertyStr(ctx, c, "base", b);
		JS_SetPropertyStr(ctx, c, "clipW", JS_NewInt32(ctx, sc.clipW));
		JS_SetPropertyStr(ctx, c, "clipH", JS_NewInt32(ctx, sc.clipH));
		JS_SetPropertyStr(ctx, c, "canvasW", JS_NewInt32(ctx, sc.canvasW));
		JS_SetPropertyStr(ctx, c, "canvasH", JS_NewInt32(ctx, sc.canvasH));
		JS_SetPropertyStr(ctx, c, "fps", JS_NewFloat64(ctx, sc.fps));
		JS_SetPropertyStr(ctx, c, "index", JS_NewInt32(ctx, sc.index));
		JS_SetPropertyStr(ctx, c, "globalTime", JS_NewFloat64(ctx, sc.globalTime));

		JSValue argv[4] = {JS_NewFloat64(ctx, t), JS_NewFloat64(ctx, u), JS_NewFloat64(ctx, dur),
				   c};
		enter(kCallBudgetMs);
		JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 4, argv);
		leave();
		for (JSValue &a : argv)
			JS_FreeValue(ctx, a);
		return r;
	}
};

bool TransformEvaluator::available()
{
	return true;
}

TransformEvaluator::TransformEvaluator() : d_(std::make_unique<Impl>())
{
	d_->rt = JS_NewRuntime();
	JS_SetMemoryLimit(d_->rt, kMemoryLimit);
	JS_SetMaxStackSize(d_->rt, kStackLimit);
	JS_SetInterruptHandler(d_->rt, &Impl::interrupted, d_.get());

	// A bare context: the language and its Math/JSON built-ins, and nothing
	// else. quickjs-libc — which is what would provide file, process and
	// network access — is deliberately not part of the build at all, so a
	// shared script cannot reach outside the animation it describes.
	d_->ctx = JS_NewContext(d_->rt);
	d_->global = JS_GetGlobalObject(d_->ctx);
}

TransformEvaluator::~TransformEvaluator()
{
	for (auto it = d_->scripts.begin(); it != d_->scripts.end(); ++it)
		d_->release(it.value());
	d_->scripts.clear();
	JS_FreeValue(d_->ctx, d_->global);
	JS_FreeContext(d_->ctx);
	JS_FreeRuntime(d_->rt);
}

bool TransformEvaluator::compile(const QString &name, const QString &source, QString *err)
{
	// Each script gets its own scope so two scripts can declare the same
	// function names without colliding. The wrapper opens on the same line the
	// user's source starts on, so reported line numbers match their editor.
	const QString wrapped = QStringLiteral("(function(){%1\n;return {"
					       "position: typeof position === 'function' ? position : null,"
					       "scale:    typeof scale    === 'function' ? scale    : null,"
					       "rotation: typeof rotation === 'function' ? rotation : null,"
					       "opacity:  typeof opacity  === 'function' ? opacity  : null"
					       "};})()")
				       .arg(source);
	const QByteArray src = wrapped.toUtf8();
	const QByteArray file = name.toUtf8();

	auto bail = [&](const QString &msg) {
		d_->lastError = msg;
		if (err)
			*err = msg;
		return false;
	};

	d_->enter(kCompileBudgetMs);
	JSValue result = JS_Eval(d_->ctx, src.constData(), size_t(src.size()),
				 file.isEmpty() ? "<script>" : file.constData(), JS_EVAL_TYPE_GLOBAL);
	d_->leave();

	if (JS_IsException(result)) {
		JS_FreeValue(d_->ctx, result);
		return bail(takeError(d_->ctx));
	}
	if (!JS_IsObject(result)) {
		JS_FreeValue(d_->ctx, result);
		return bail(QStringLiteral("The script did not evaluate to anything usable."));
	}

	Impl::Compiled c;
	auto grab = [&](const char *key) -> JSValue {
		JSValue fn = JS_GetPropertyStr(d_->ctx, result, key);
		if (JS_IsFunction(d_->ctx, fn))
			return fn;
		JS_FreeValue(d_->ctx, fn);
		return JS_UNDEFINED;
	};
	c.position = grab("position");
	c.scale = grab("scale");
	c.rotation = grab("rotation");
	c.opacity = grab("opacity");
	JS_FreeValue(d_->ctx, result);

	if (JS_IsUndefined(c.position) && JS_IsUndefined(c.scale) && JS_IsUndefined(c.rotation) &&
	    JS_IsUndefined(c.opacity)) {
		d_->release(c);
		return bail(QStringLiteral(
			"No channel functions found — define at least one of position(), scale(), "
			"rotation() or opacity()."));
	}

	// Recompiling under an existing name (live reload) replaces the old one.
	const auto old = d_->scripts.find(name);
	if (old != d_->scripts.end())
		d_->release(old.value());
	d_->scripts.insert(name, c);

	d_->lastError.clear();
	if (err)
		err->clear();
	return true;
}

bool TransformEvaluator::has(const QString &name) const
{
	return d_->scripts.contains(name);
}

int TransformEvaluator::channelsOf(const QString &name) const
{
	const auto it = d_->scripts.constFind(name);
	if (it == d_->scripts.constEnd())
		return 0;
	int m = 0;
	if (!JS_IsUndefined(it.value().position))
		m |= ChanPosition;
	if (!JS_IsUndefined(it.value().scale))
		m |= ChanScale;
	if (!JS_IsUndefined(it.value().rotation))
		m |= ChanRotation;
	if (!JS_IsUndefined(it.value().opacity))
		m |= ChanOpacity;
	return m;
}

void TransformEvaluator::forget(const QString &name)
{
	const auto it = d_->scripts.find(name);
	if (it == d_->scripts.end())
		return;
	d_->release(it.value());
	d_->scripts.erase(it);
}

QString TransformEvaluator::lastError() const
{
	return d_->lastError;
}

void TransformEvaluator::clearError()
{
	d_->lastError.clear();
}

TlTransform TransformEvaluator::apply(const ClipScript &script, const TlTransform &base,
				      const TlClip &clip, qint64 outMs, const ScriptContext &ctx)
{
	// The clip is only ever asked for these two numbers, and a script component
	// already knows both -- so the work lives in the overload and this one is
	// the convenience wrapper, rather than a component having to fabricate a
	// TlClip to get at it.
	return applyAt(script, base, outMs - clip.outStartMs, clip.outDurationMs(), ctx);
}

TlTransform TransformEvaluator::applyAt(const ClipScript &script, const TlTransform &base,
					qint64 tMsIn, qint64 durMsIn, const ScriptContext &ctx)
{
	if (!script.active())
		return base;
	const auto it = d_->scripts.constFind(script.name);
	if (it == d_->scripts.constEnd())
		return base;
	const Impl::Compiled &c = it.value();

	JSContext *jc = d_->ctx;

	// The //@param values are plain globals, so a script reads `zoom` directly.
	for (auto p = script.params.constBegin(); p != script.params.constEnd(); ++p)
		JS_SetPropertyStr(jc, d_->global, p.key().toUtf8().constData(),
				  JS_NewFloat64(jc, p.value()));

	const double durMs = double(std::max<qint64>(1, durMsIn));
	const double tMs = double(std::clamp<qint64>(tMsIn, 0, qint64(durMs)));
	const double t = tMs / 1000.0;
	const double dur = durMs / 1000.0;
	const double u = std::clamp(tMs / durMs, 0.0, 1.0);

	TlTransform out = base;
	bool failed = false;

	// Returns a value the caller must free; on a script error it records the
	// message and flags the whole apply() as failed.
	auto run = [&](JSValueConst fn) -> JSValue {
		JSValue v = d_->callChannel(fn, t, u, dur, ctx, base);
		if (JS_IsException(v)) {
			JS_FreeValue(jc, v);
			d_->lastError = takeError(jc);
			failed = true;
			return JS_UNDEFINED;
		}
		return v;
	};

	if (!JS_IsUndefined(c.position)) {
		JSValue v = run(c.position);
		if (failed)
			return base;
		// An array is also an object in JS, so test for one first.
		if (JS_IsArray(v)) { // [x, y]
			JSValue x = JS_GetPropertyUint32(jc, v, 0);
			JSValue y = JS_GetPropertyUint32(jc, v, 1);
			out.posX = numberOr(jc, x, out.posX);
			out.posY = numberOr(jc, y, out.posY);
			JS_FreeValue(jc, x);
			JS_FreeValue(jc, y);
		} else if (JS_IsObject(v)) { // { x, y } — either field optional
			JSValue x = JS_GetPropertyStr(jc, v, "x");
			JSValue y = JS_GetPropertyStr(jc, v, "y");
			out.posX = numberOr(jc, x, out.posX);
			out.posY = numberOr(jc, y, out.posY);
			JS_FreeValue(jc, x);
			JS_FreeValue(jc, y);
		}
		JS_FreeValue(jc, v);
	}

	auto scalar = [&](JSValueConst fn, double &field) {
		if (JS_IsUndefined(fn) || failed)
			return;
		JSValue v = run(fn);
		if (!failed)
			field = numberOr(jc, v, field);
		JS_FreeValue(jc, v);
	};
	scalar(c.scale, out.scale);
	scalar(c.rotation, out.rotation);
	scalar(c.opacity, out.opacity);
	if (failed)
		return base;

	// Keep a bad script from producing an un-renderable frame.
	out.scale = std::clamp(out.scale, 0.001, 100.0);
	out.opacity = std::clamp(out.opacity, 0.0, 1.0);
	if (!std::isfinite(out.posX) || !std::isfinite(out.posY) || !std::isfinite(out.scale) ||
	    !std::isfinite(out.rotation) || !std::isfinite(out.opacity))
		return base;
	return out;
}

#else // ---- built without a scripting engine -------------------------------

struct TransformEvaluator::Impl {
	QString lastError;
};

bool TransformEvaluator::available()
{
	return false;
}

TransformEvaluator::TransformEvaluator() : d_(std::make_unique<Impl>()) {}
TransformEvaluator::~TransformEvaluator() = default;

bool TransformEvaluator::compile(const QString &, const QString &, QString *err)
{
	d_->lastError = QStringLiteral(
		"This build has no scripting engine, so transform scripts are unavailable.");
	if (err)
		*err = d_->lastError;
	return false;
}

bool TransformEvaluator::has(const QString &) const
{
	return false;
}
void TransformEvaluator::forget(const QString &) {}
QString TransformEvaluator::lastError() const
{
	return d_->lastError;
}
void TransformEvaluator::clearError()
{
	d_->lastError.clear();
}

TlTransform TransformEvaluator::apply(const ClipScript &, const TlTransform &base, const TlClip &,
				      qint64, const ScriptContext &)
{
	return base; // scripts are inert; hand-set transforms and keyframes still work
}

// The header declares both, and TransformScriptComponent calls THIS one -- so
// leaving it out of the stub made a no-QuickJS build fail to link, which is
// exactly what the note at the top of this file promises will not happen.
TlTransform TransformEvaluator::applyAt(const ClipScript &, const TlTransform &base, qint64, qint64,
					const ScriptContext &)
{
	return base;
}

#endif

} // namespace harpia
