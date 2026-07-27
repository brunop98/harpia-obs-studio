#include "TransformScript.hpp"

#include <QJSValueIterator>

#include <algorithm>
#include <cmath>

namespace harpia {

TransformEvaluator::TransformEvaluator()
{
	// Math/JSON only — no filesystem, network or process access is exposed, so a
	// shared script can't reach outside the animation it's meant to describe.
	engine_.installExtensions(QJSEngine::ConsoleExtension);
}

bool TransformEvaluator::compile(const QString &name, const QString &source, QString *err)
{
	// Each script gets its own scope object so two scripts can define functions
	// with the same names without colliding.
	const QString wrapped = QStringLiteral("(function(){\n%1\n;return {"
					       "position: typeof position === 'function' ? position : null,"
					       "scale:    typeof scale    === 'function' ? scale    : null,"
					       "rotation: typeof rotation === 'function' ? rotation : null,"
					       "opacity:  typeof opacity  === 'function' ? opacity  : null"
					       "};})()")
				       .arg(source);

	const QJSValue result = engine_.evaluate(wrapped, name);
	if (result.isError()) {
		const QString msg = QStringLiteral("%1 (line %2)")
					    .arg(result.property(QStringLiteral("message")).toString())
					    .arg(result.property(QStringLiteral("lineNumber")).toInt());
		lastError_ = msg;
		if (err)
			*err = msg;
		return false;
	}
	if (!result.isObject()) {
		const QString msg = QStringLiteral("The script did not evaluate to anything usable.");
		lastError_ = msg;
		if (err)
			*err = msg;
		return false;
	}

	Compiled c;
	c.position = result.property(QStringLiteral("position"));
	c.scale = result.property(QStringLiteral("scale"));
	c.rotation = result.property(QStringLiteral("rotation"));
	c.opacity = result.property(QStringLiteral("opacity"));
	if (!c.position.isCallable() && !c.scale.isCallable() && !c.rotation.isCallable() &&
	    !c.opacity.isCallable()) {
		const QString msg = QStringLiteral(
			"No channel functions found — define at least one of position(), scale(), "
			"rotation() or opacity().");
		lastError_ = msg;
		if (err)
			*err = msg;
		return false;
	}
	scripts_.insert(name, c);
	lastError_.clear();
	if (err)
		err->clear();
	return true;
}

QJSValue TransformEvaluator::callChannel(const QJSValue &fn, double t, double u, double dur,
					 const ScriptContext &ctx)
{
	if (!fn.isCallable())
		return QJSValue(QJSValue::UndefinedValue);
	QJSValue c = engine_.newObject();
	c.setProperty(QStringLiteral("clipW"), ctx.clipW);
	c.setProperty(QStringLiteral("clipH"), ctx.clipH);
	c.setProperty(QStringLiteral("canvasW"), ctx.canvasW);
	c.setProperty(QStringLiteral("canvasH"), ctx.canvasH);
	c.setProperty(QStringLiteral("fps"), ctx.fps);
	c.setProperty(QStringLiteral("index"), ctx.index);
	c.setProperty(QStringLiteral("globalTime"), ctx.globalTime);
	return const_cast<QJSValue &>(fn).call({t, u, dur, c});
}

TlTransform TransformEvaluator::apply(const ClipScript &script, const TlTransform &base,
				      const TlClip &clip, qint64 outMs, const ScriptContext &ctx)
{
	if (!script.active())
		return base;
	const auto it = scripts_.constFind(script.name);
	if (it == scripts_.constEnd())
		return base;
	const Compiled &c = it.value();

	// The //@param values are plain globals, so a script reads `zoom` directly.
	for (auto p = script.params.constBegin(); p != script.params.constEnd(); ++p)
		engine_.globalObject().setProperty(p.key(), p.value());

	const double durMs = double(std::max<qint64>(1, clip.outDurationMs()));
	const double tMs = double(std::clamp<qint64>(outMs - clip.outStartMs, 0, qint64(durMs)));
	const double t = tMs / 1000.0;
	const double dur = durMs / 1000.0;
	const double u = std::clamp(tMs / durMs, 0.0, 1.0);

	TlTransform out = base;
	auto fail = [&](const QJSValue &v) {
		if (v.isError()) {
			lastError_ = v.property(QStringLiteral("message")).toString();
			return true;
		}
		return false;
	};
	auto num = [](const QJSValue &v, double fallback) {
		return v.isNumber() ? v.toNumber() : fallback;
	};

	if (c.position.isCallable()) {
		const QJSValue v = callChannel(c.position, t, u, dur, ctx);
		if (fail(v))
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
		const QJSValue v = callChannel(c.scale, t, u, dur, ctx);
		if (fail(v))
			return base;
		out.scale = num(v, out.scale);
	}
	if (c.rotation.isCallable()) {
		const QJSValue v = callChannel(c.rotation, t, u, dur, ctx);
		if (fail(v))
			return base;
		out.rotation = num(v, out.rotation);
	}
	if (c.opacity.isCallable()) {
		const QJSValue v = callChannel(c.opacity, t, u, dur, ctx);
		if (fail(v))
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

} // namespace harpia
