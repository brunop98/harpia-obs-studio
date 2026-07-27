#include "TransformScript.hpp"

#include <algorithm>
#include <cmath>

// HARPIA_HAVE_QJS is set by CMake when the Qt build actually provides the Qml
// module (QJSEngine). Trimmed Qt builds — including the obs-deps Qt used for
// Windows releases — omit it, so the whole engine compiles out and scripting
// degrades to a no-op with a clear message instead of breaking the build.
#if defined(HARPIA_HAVE_QJS) && HARPIA_HAVE_QJS
#include <QJSEngine>
#include <QJSValue>
#endif

namespace harpia {

#if defined(HARPIA_HAVE_QJS) && HARPIA_HAVE_QJS

struct TransformEvaluator::Impl {
	struct Compiled {
		QJSValue position, scale, rotation, opacity;
	};
	QJSEngine engine;
	QMap<QString, Compiled> scripts;
	QString lastError;

	QJSValue callChannel(const QJSValue &fn, double t, double u, double dur,
			     const ScriptContext &ctx)
	{
		if (!fn.isCallable())
			return QJSValue(QJSValue::UndefinedValue);
		QJSValue c = engine.newObject();
		c.setProperty(QStringLiteral("clipW"), ctx.clipW);
		c.setProperty(QStringLiteral("clipH"), ctx.clipH);
		c.setProperty(QStringLiteral("canvasW"), ctx.canvasW);
		c.setProperty(QStringLiteral("canvasH"), ctx.canvasH);
		c.setProperty(QStringLiteral("fps"), ctx.fps);
		c.setProperty(QStringLiteral("index"), ctx.index);
		c.setProperty(QStringLiteral("globalTime"), ctx.globalTime);
		return const_cast<QJSValue &>(fn).call({t, u, dur, c});
	}
};

bool TransformEvaluator::available()
{
	return true;
}

TransformEvaluator::TransformEvaluator() : d_(std::make_unique<Impl>())
{
	// Math/JSON only — no filesystem, network or process access is exposed, so a
	// shared script can't reach outside the animation it's meant to describe.
	d_->engine.installExtensions(QJSEngine::ConsoleExtension);
}

TransformEvaluator::~TransformEvaluator() = default;

bool TransformEvaluator::compile(const QString &name, const QString &source, QString *err)
{
	// Each script gets its own scope so two scripts can define the same function
	// names without colliding.
	const QString wrapped = QStringLiteral("(function(){\n%1\n;return {"
					       "position: typeof position === 'function' ? position : null,"
					       "scale:    typeof scale    === 'function' ? scale    : null,"
					       "rotation: typeof rotation === 'function' ? rotation : null,"
					       "opacity:  typeof opacity  === 'function' ? opacity  : null"
					       "};})()")
				       .arg(source);

	const QJSValue result = d_->engine.evaluate(wrapped, name);
	auto bail = [&](const QString &msg) {
		d_->lastError = msg;
		if (err)
			*err = msg;
		return false;
	};
	if (result.isError())
		return bail(QStringLiteral("%1 (line %2)")
				    .arg(result.property(QStringLiteral("message")).toString())
				    .arg(result.property(QStringLiteral("lineNumber")).toInt()));
	if (!result.isObject())
		return bail(QStringLiteral("The script did not evaluate to anything usable."));

	Impl::Compiled c;
	c.position = result.property(QStringLiteral("position"));
	c.scale = result.property(QStringLiteral("scale"));
	c.rotation = result.property(QStringLiteral("rotation"));
	c.opacity = result.property(QStringLiteral("opacity"));
	if (!c.position.isCallable() && !c.scale.isCallable() && !c.rotation.isCallable() &&
	    !c.opacity.isCallable())
		return bail(QStringLiteral(
			"No channel functions found — define at least one of position(), scale(), "
			"rotation() or opacity()."));

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

void TransformEvaluator::forget(const QString &name)
{
	d_->scripts.remove(name);
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
	if (!script.active())
		return base;
	const auto it = d_->scripts.constFind(script.name);
	if (it == d_->scripts.constEnd())
		return base;
	const Impl::Compiled &c = it.value();

	// The //@param values are plain globals, so a script reads `zoom` directly.
	for (auto p = script.params.constBegin(); p != script.params.constEnd(); ++p)
		d_->engine.globalObject().setProperty(p.key(), p.value());

	const double durMs = double(std::max<qint64>(1, clip.outDurationMs()));
	const double tMs = double(std::clamp<qint64>(outMs - clip.outStartMs, 0, qint64(durMs)));
	const double t = tMs / 1000.0;
	const double dur = durMs / 1000.0;
	const double u = std::clamp(tMs / durMs, 0.0, 1.0);

	TlTransform out = base;
	auto failed = [&](const QJSValue &v) {
		if (v.isError()) {
			d_->lastError = v.property(QStringLiteral("message")).toString();
			return true;
		}
		return false;
	};
	auto num = [](const QJSValue &v, double fallback) {
		return v.isNumber() ? v.toNumber() : fallback;
	};

	if (c.position.isCallable()) {
		const QJSValue v = d_->callChannel(c.position, t, u, dur, ctx);
		if (failed(v))
			return base;
		// An array is also an object in JS, so test isArray() first.
		if (v.isArray()) { // [x, y]
			out.posX = num(v.property(0), out.posX);
			out.posY = num(v.property(1), out.posY);
		} else if (v.isObject()) { // { x, y } — either field optional
			out.posX = num(v.property(QStringLiteral("x")), out.posX);
			out.posY = num(v.property(QStringLiteral("y")), out.posY);
		}
	}
	if (c.scale.isCallable()) {
		const QJSValue v = d_->callChannel(c.scale, t, u, dur, ctx);
		if (failed(v))
			return base;
		out.scale = num(v, out.scale);
	}
	if (c.rotation.isCallable()) {
		const QJSValue v = d_->callChannel(c.rotation, t, u, dur, ctx);
		if (failed(v))
			return base;
		out.rotation = num(v, out.rotation);
	}
	if (c.opacity.isCallable()) {
		const QJSValue v = d_->callChannel(c.opacity, t, u, dur, ctx);
		if (failed(v))
			return base;
		out.opacity = num(v, out.opacity);
	}

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
		"This build has no scripting engine (its Qt provides no Qml module), so transform "
		"scripts are unavailable.");
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

#endif

} // namespace harpia
