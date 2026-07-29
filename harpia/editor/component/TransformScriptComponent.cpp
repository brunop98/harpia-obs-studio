#include "TransformScriptComponent.hpp"

#include "../script/TransformScript.hpp"
#include "../shader/ShaderEffect.hpp" // parseShaderParams -- the shared //@param format
#include "Component.hpp"
#include "ComponentRegistry.hpp"

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>

namespace harpia {

namespace {

QAtomicInt g_generation{1};

struct ScriptSource {
	QString name; // the file stem, which is also the evaluator's cache key
	QString source;
	QVector<ShaderParam> params;
	int generation = 0;
};
QMap<QString, ScriptSource> g_sources; // by component id

// One TransformEvaluator per thread: a QuickJS runtime is not thread-safe, and
// the preview renders on the GUI thread while the exporter renders on its
// worker. Each compiles the same source into its own.
class ThreadEval {
public:
	static ThreadEval &get()
	{
		static thread_local ThreadEval e;
		return e;
	}

	// Null when the engine is missing or the script will not compile; the
	// caller then leaves the pose alone rather than failing the render.
	TransformEvaluator *ready(const ScriptSource &src)
	{
		if (!TransformEvaluator::available())
			return nullptr;
		const int have = gens_.value(src.name, -1);
		if (have != src.generation) {
			// A stale compile under this name would otherwise keep running
			// after the file changed on disk.
			eval_.forget(src.name);
			if (!eval_.compile(src.name, src.source)) {
				gens_.insert(src.name, src.generation);
				ok_.insert(src.name, false);
				return nullptr;
			}
			gens_.insert(src.name, src.generation);
			ok_.insert(src.name, true);
		}
		return ok_.value(src.name, false) ? &eval_ : nullptr;
	}

private:
	TransformEvaluator eval_;
	QMap<QString, int> gens_;
	QMap<QString, bool> ok_;
};

class TransformScriptComponent : public IComponent {
public:
	explicit TransformScriptComponent(QString id) : id_(std::move(id)) {}

	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		const auto it = g_sources.constFind(id_);
		if (it == g_sources.constEnd())
			return;
		TransformEvaluator *ev = ThreadEval::get().ready(*it);
		if (!ev)
			return;

		ClipScript cs;
		cs.name = it->name;
		for (const ShaderParam &p : it->params)
			cs.params.insert(p.uniform,
					 ctx.f(p.uniform.toLatin1().constData(), p.def));

		ScriptContext sctx;
		sctx.canvasW = ctx.canvas.width();
		sctx.canvasH = ctx.canvas.height();
		sctx.fps = ctx.fps;
		sctx.globalTime = double(ctx.outMs) / 1000.0;

		// io.xf is what the clip would be posed at WITHOUT this script -- its
		// own settings, its keyframes, and any component that ran before it.
		// Handing that in as the base is what lets `ctx.base` compose rather
		// than each script fighting the last.
		io.xf = ev->applyAt(cs, io.xf, ctx.tMs, ctx.durMs, sctx);
	}

private:
	QString id_;
};

} // namespace

bool TransformScriptComponents::available()
{
	return TransformEvaluator::available();
}

int TransformScriptComponents::loadFolder(const QString &folder, ComponentRegistry &reg,
					  QStringList *errors)
{
	const int gen = g_generation.fetchAndAddOrdered(1) + 1;
	int n = 0;
	for (const QFileInfo &fi :
	     QDir(folder).entryInfoList({QStringLiteral("*.js")}, QDir::Files, QDir::Name)) {
		const QString stem = fi.completeBaseName();
		QFile f(fi.absoluteFilePath());
		if (!f.open(QIODevice::ReadOnly)) {
			if (errors)
				*errors << QStringLiteral("%1.js: could not be read").arg(stem);
			continue;
		}
		const QString source = QString::fromUtf8(f.readAll());
		f.close();

		const QString id = transformScriptComponentId(stem);
		ScriptSource src;
		src.name = stem;
		src.source = source;
		src.params = parseShaderParams(source);
		src.generation = gen;
		g_sources.insert(id, src);

		ComponentType t;
		t.id = id;
		t.displayName = stem.left(1).toUpper() + stem.mid(1);
		t.category = QStringLiteral("Script");
		t.stage = Stage::Transform;
		t.help = QStringLiteral("Drives position/scale/rotation/opacity from %1.js. "
					"Channels the script leaves out keep their value.")
				 .arg(stem);
		for (const ShaderParam &p : src.params) {
			PropDef d;
			d.key = p.uniform;
			d.label = p.label.isEmpty() ? p.uniform : p.label;
			d.type = p.type == ShaderParam::Type::Bool ? PropType::Bool
								   : PropType::Float;
			d.min = p.min;
			d.max = p.max;
			d.def = p.def;
			d.keyframeable = d.type == PropType::Float;
			t.props.append(d);
		}
		t.make = [id] {
			return std::unique_ptr<IComponent>(new TransformScriptComponent(id));
		};
		reg.add(t);
		++n;
	}
	return n;
}

} // namespace harpia
