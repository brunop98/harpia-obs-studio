#include "ScriptComponent.hpp"

#include "../shader/ShaderEffect.hpp" // parseShaderParams -- the same //@param format

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>

#if HARPIA_HAVE_QJS
#include "quickjs.h"
#endif

namespace harpia {

namespace {

// A script that runs away must not take the preview with it. Generous enough
// that no honest component notices, short enough that a `while(true)` shows up
// as a stutter and an error rather than a hang.
constexpr qint64 kCallBudgetMs = 50;

QString annotation(const QString &src, const char *tag, const QString &fallback = QString())
{
	QRegularExpression re(QStringLiteral("^\\s*//@%1\\s+(.+)$").arg(QLatin1String(tag)),
			      QRegularExpression::MultilineOption);
	const auto m = re.match(src);
	return m.hasMatch() ? m.captured(1).trimmed() : fallback;
}

bool stageFromName(const QString &name, Stage *out)
{
	for (int i = 0; i < kStageCount; ++i)
		if (name.compare(QString::fromLatin1(stageName(Stage(i))), Qt::CaseInsensitive) == 0) {
			*out = Stage(i);
			return true;
		}
	return false;
}

// The //@param format is shared with the shaders and the transform scripts, so
// one syntax covers all three and the Inspector renders them the same way.
QVector<PropDef> propsFrom(const QString &src)
{
	QVector<PropDef> out;
	for (const ShaderParam &p : parseShaderParams(src)) {
		PropDef d;
		d.key = p.uniform;
		d.label = p.label.isEmpty() ? p.uniform : p.label;
		d.type = p.type == ShaderParam::Type::Bool ? PropType::Bool : PropType::Float;
		d.min = p.min;
		d.max = p.max;
		d.def = p.def;
		d.keyframeable = d.type == PropType::Float;
		out.append(d);
	}
	return out;
}

} // namespace

bool ScriptComponents::looksImpure(const QString &source)
{
	// Anything assigned at the top level is state that survives between calls,
	// which is the one way a script can quietly break scrubbing. Declarations
	// used as constants are the common innocent case, so only `let`/`var` and
	// bare assignment count; `const` does not.
	static const QRegularExpression re(
		QStringLiteral("^\\s*(let|var)\\s+\\w+\\s*=|^\\s*\\w+\\s*(\\+|-|\\*|/)?=[^=]"),
		QRegularExpression::MultilineOption);
	return re.match(source).hasMatch();
}

#if HARPIA_HAVE_QJS

namespace {

QString toQString(JSContext *ctx, JSValueConst v)
{
	const char *s = JS_ToCString(ctx, v);
	const QString out = s ? QString::fromUtf8(s) : QString();
	if (s)
		JS_FreeCString(ctx, s);
	return out;
}

QString takeError(JSContext *ctx)
{
	JSValue e = JS_GetException(ctx);
	QString msg = toQString(ctx, e);
	JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
	if (!JS_IsUndefined(stack))
		msg += QStringLiteral("\n") + toQString(ctx, stack);
	JS_FreeValue(ctx, stack);
	JS_FreeValue(ctx, e);
	return msg;
}

// One QuickJS runtime per thread. A runtime is not thread-safe and the preview
// renders on the GUI thread while the exporter renders on its worker, so each
// compiles the same sources into its own. Keyed by the component's id, which is
// also what makes hot reload work: re-register, bump the generation, and every
// thread recompiles on its next frame.
class ThreadHost {
public:
	static ThreadHost &get()
	{
		static thread_local ThreadHost h;
		return h;
	}

	~ThreadHost()
	{
		for (auto it = fns_.begin(); it != fns_.end(); ++it)
			JS_FreeValue(ctx_, it.value());
		if (ctx_)
			JS_FreeContext(ctx_);
		if (rt_)
			JS_FreeRuntime(rt_);
	}

	// Returns JS_UNDEFINED if this script will not compile here; the caller
	// then leaves the frame untouched rather than failing the render.
	JSValue fnFor(const QString &id, const QString &source, int generation)
	{
		if (!ensure())
			return JS_UNDEFINED;
		const QString key = id + QChar('#') + QString::number(generation);
		const auto it = fns_.find(key);
		if (it != fns_.end())
			return it.value();

		// Drop older generations of the same component so a long editing
		// session does not accumulate one compiled copy per save.
		for (auto i = fns_.begin(); i != fns_.end();) {
			if (i.key().startsWith(id + QChar('#'))) {
				JS_FreeValue(ctx_, i.value());
				i = fns_.erase(i);
			} else {
				++i;
			}
		}

		// Wrapped in an IIFE so two scripts can both define `evaluate` without
		// colliding in the global scope, and so the function comes back as a
		// value rather than having to be fished out of the globals.
		const QString wrapped =
			QStringLiteral("(function(){\n%1\n;return typeof evaluate==='function'?"
				       "evaluate:null;})()")
				.arg(source);
		const QByteArray utf8 = wrapped.toUtf8();
		arm();
		JSValue v = JS_Eval(ctx_, utf8.constData(), size_t(utf8.size()),
				    id.toUtf8().constData(), JS_EVAL_TYPE_GLOBAL);
		disarm();
		if (JS_IsException(v)) {
			JS_FreeValue(ctx_, v);
			takeError(ctx_); // swallowed: reported at load time, not per frame
			fns_.insert(key, JS_UNDEFINED);
			return JS_UNDEFINED;
		}
		if (!JS_IsFunction(ctx_, v)) {
			JS_FreeValue(ctx_, v);
			v = JS_UNDEFINED;
		}
		fns_.insert(key, v);
		return v;
	}

	JSContext *ctx() { return ctx_; }
	void arm()
	{
		JS_UpdateStackTop(rt_); // this thread's stack, not whoever built the runtime
		watchdog_.restart();
	}
	void disarm() { watchdog_.invalidate(); }

private:
	bool ensure()
	{
		if (ctx_)
			return true;
		rt_ = JS_NewRuntime();
		if (!rt_)
			return false;
		JS_SetInterruptHandler(rt_, &ThreadHost::interrupted, this);
		ctx_ = JS_NewContext(rt_);
		return ctx_ != nullptr;
	}
	static int interrupted(JSRuntime *, void *opaque)
	{
		auto *self = static_cast<ThreadHost *>(opaque);
		if (!self->watchdog_.isValid())
			return 0;
		return self->watchdog_.elapsed() > kCallBudgetMs ? 1 : 0;
	}

	JSRuntime *rt_ = nullptr;
	JSContext *ctx_ = nullptr;
	QMap<QString, JSValue> fns_;
	QElapsedTimer watchdog_;
};

// The IComponent the registry hands out. Holds only the source and which
// generation of it this is — no JS state at all, which is what lets one
// instance be shared across every thread and every frame.
class ScriptComponent : public IComponent {
public:
	ScriptComponent(QString id, QString source, int generation)
		: id_(std::move(id)), source_(std::move(source)), gen_(generation)
	{
	}

	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		ThreadHost &h = ThreadHost::get();
		JSValue fn = h.fnFor(id_, source_, gen_);
		if (JS_IsUndefined(fn) || !h.ctx())
			return; // did not compile on this thread; leave the frame alone
		JSContext *c = h.ctx();

		JSValue jctx = JS_NewObject(c);
		JS_SetPropertyStr(c, jctx, "t", JS_NewFloat64(c, ctx.tSec()));
		JS_SetPropertyStr(c, jctx, "u", JS_NewFloat64(c, ctx.u()));
		JS_SetPropertyStr(c, jctx, "dur", JS_NewFloat64(c, double(ctx.durMs) / 1000.0));
		JS_SetPropertyStr(c, jctx, "outTime", JS_NewFloat64(c, double(ctx.outMs) / 1000.0));
		JS_SetPropertyStr(c, jctx, "fps", JS_NewFloat64(c, ctx.fps));
		JS_SetPropertyStr(c, jctx, "canvasW", JS_NewInt32(c, ctx.canvas.width()));
		JS_SetPropertyStr(c, jctx, "canvasH", JS_NewInt32(c, ctx.canvas.height()));
		JSValue p = JS_NewObject(c);
		for (auto it = ctx.p.cbegin(); it != ctx.p.cend(); ++it) {
			const QByteArray k = it.key().toUtf8();
			if (it->typeId() == QMetaType::Bool)
				JS_SetPropertyStr(c, p, k.constData(), JS_NewBool(c, it->toBool()));
			else
				JS_SetPropertyStr(c, p, k.constData(),
						  JS_NewFloat64(c, it->toDouble()));
		}
		JS_SetPropertyStr(c, jctx, "p", p);

		// io goes in carrying what the clip would be WITHOUT this component, so
		// `io.scale *= 1.1` composes instead of overwriting. Same bargain the
		// transform scripts' ctx.base offers.
		JSValue jio = JS_NewObject(c);
		JS_SetPropertyStr(c, jio, "timeScale", JS_NewFloat64(c, io.timeScale));
		JS_SetPropertyStr(c, jio, "x", JS_NewFloat64(c, io.xf.posX));
		JS_SetPropertyStr(c, jio, "y", JS_NewFloat64(c, io.xf.posY));
		JS_SetPropertyStr(c, jio, "scale", JS_NewFloat64(c, io.xf.scale));
		JS_SetPropertyStr(c, jio, "rotation", JS_NewFloat64(c, io.xf.rotation));
		JS_SetPropertyStr(c, jio, "opacity", JS_NewFloat64(c, io.xf.opacity));

		JSValue argv[2] = {jctx, jio};
		h.arm();
		JSValue r = JS_Call(c, fn, JS_UNDEFINED, 2, argv);
		h.disarm();

		if (JS_IsException(r)) {
			takeError(c); // a throwing component is a no-op, not a broken render
		} else {
			readBack(c, jio, io);
		}
		JS_FreeValue(c, r);
		JS_FreeValue(c, jctx);
		JS_FreeValue(c, jio);
	}

private:
	static double num(JSContext *c, JSValueConst o, const char *k, double fallback)
	{
		JSValue v = JS_GetPropertyStr(c, o, k);
		double d = fallback;
		// A script that sets a field to NaN or a string would otherwise poison
		// the transform for every frame after it.
		if (JS_ToFloat64(c, &d, v) < 0 || !std::isfinite(d))
			d = fallback;
		JS_FreeValue(c, v);
		return d;
	}
	static void readBack(JSContext *c, JSValueConst jio, ClipState &io)
	{
		io.timeScale = std::max(0.01, num(c, jio, "timeScale", io.timeScale));
		io.xf.posX = num(c, jio, "x", io.xf.posX);
		io.xf.posY = num(c, jio, "y", io.xf.posY);
		io.xf.scale = num(c, jio, "scale", io.xf.scale);
		io.xf.rotation = num(c, jio, "rotation", io.xf.rotation);
		io.xf.opacity = std::clamp(num(c, jio, "opacity", io.xf.opacity), 0.0, 1.0);
	}

	QString id_, source_;
	int gen_ = 0;
};

// Bumped on every (re)load so each thread recompiles rather than serving a
// cached function from the previous version of the file.
int nextGeneration()
{
	static int g = 0;
	return ++g;
}

} // namespace

bool ScriptComponents::available()
{
	return true;
}

bool ScriptComponents::loadSource(const QString &source, ComponentRegistry &reg, QString *err)
{
	auto fail = [err](const QString &m) {
		if (err)
			*err = m;
		return false;
	};

	const QString id = annotation(source, "component");
	if (id.isEmpty())
		return fail(QStringLiteral(
			"No //@component line. A component script must start with one, e.g. "
			"//@component acme.pulse"));
	if (!id.contains(QChar('.')))
		return fail(QStringLiteral("Component id \"%1\" needs a namespace, e.g. "
					   "acme.%1 — bare names collide with built-ins.")
				    .arg(id));

	ComponentType t;
	t.id = id;
	t.displayName = annotation(source, "name", id.section(QChar('.'), -1));
	t.category = annotation(source, "category", QStringLiteral("Custom"));
	t.help = annotation(source, "help");
	t.props = propsFrom(source);
	t.version = annotation(source, "version", QStringLiteral("1")).toInt();
	if (t.version < 1)
		t.version = 1;

	const QString stageText = annotation(source, "stage", QStringLiteral("Transform"));
	if (!stageFromName(stageText, &t.stage))
		return fail(QStringLiteral("Unknown stage \"%1\". Use one of Time, Source, "
					   "Transform, Pixel, Composite, Audio.")
				    .arg(stageText));
	if (t.stage == Stage::Pixel || t.stage == Stage::Source)
		return fail(QStringLiteral(
			"Scripts cannot serve the %1 stage. A per-pixel callback would be "
			"two million calls a frame; write pixel work as a shader instead.")
				    .arg(QLatin1String(stageName(t.stage))));

	const QString needs = annotation(source, "requires");
	if (!needs.isEmpty())
		t.requiresIds = needs.split(QRegularExpression(QStringLiteral("[\\s,]+")),
					    Qt::SkipEmptyParts);

	// Compile once here so a syntax error is reported at load, pointing at the
	// file, rather than silently doing nothing on every frame.
	ThreadHost &h = ThreadHost::get();
	const int gen = nextGeneration();
	if (JS_IsUndefined(h.fnFor(id, source, gen)))
		return fail(QStringLiteral(
			"Script did not compile, or defines no evaluate(ctx, io) function."));

	const QString src = source;
	t.make = [id, src, gen] {
		return std::unique_ptr<IComponent>(new ScriptComponent(id, src, gen));
	};
	if (!reg.add(t))
		return fail(QStringLiteral("Registry refused \"%1\".").arg(id));
	if (err)
		err->clear();
	return true;
}

#else // no scripting engine in this build

bool ScriptComponents::available()
{
	return false;
}

bool ScriptComponents::loadSource(const QString &, ComponentRegistry &, QString *err)
{
	if (err)
		*err = QStringLiteral(
			"This build has no scripting engine, so custom components are unavailable.");
	return false;
}

#endif

int ScriptComponents::loadFolder(const QString &folder, ComponentRegistry &reg, QStringList *errors)
{
	QDir dir(folder);
	if (!dir.exists())
		return 0;
	int n = 0;
	// One bad file must not cost the user the other nine, so each is reported
	// and skipped rather than aborting the sweep.
	for (const QFileInfo &fi :
	     dir.entryInfoList({QStringLiteral("*.js")}, QDir::Files, QDir::Name)) {
		QFile f(fi.absoluteFilePath());
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			if (errors)
				errors->append(QStringLiteral("%1: cannot be read").arg(fi.fileName()));
			continue;
		}
		QTextStream in(&f);
		const QString src = in.readAll();
		QString err;
		if (loadSource(src, reg, &err)) {
			++n;
			if (errors && looksImpure(src))
				errors->append(
					QStringLiteral("%1: assigns to a variable outside "
						       "evaluate(). State kept between frames makes "
						       "scrubbing disagree with playback.")
						.arg(fi.fileName()));
		} else if (errors) {
			errors->append(QStringLiteral("%1: %2").arg(fi.fileName(), err));
		}
	}
	return n;
}

} // namespace harpia
