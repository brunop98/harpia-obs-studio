#include "ShaderComponent.hpp"

#include "../shader/ShaderEffect.hpp"
#include "../shader/GpuFx.hpp"
#include "../shader/ShaderRenderer.hpp"
#include "Component.hpp"
#include "ComponentRegistry.hpp"

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMap>
#include <QReadWriteLock>

namespace harpia {

namespace {

// Bumped on every loadFolder. A cached program carries the generation it was
// compiled at, so a reload invalidates every thread's copy without any of them
// having to be told.
QAtomicInt g_generation{1};

// A shader's source and its parsed parameters. GUI reloads replace snapshots
// under a lock; each render thread holds its own stable value while compiling.
struct ShaderSource {
	QString wrapped;
	QString folder;
	QVector<ShaderParam> defs;
	int generation = 0;
};
QMap<QString, ShaderSource> g_sources; // by component id
QReadWriteLock g_sourcesLock;

bool sourceFor(const QString &id, ShaderSource &source)
{
	QReadLocker lock(&g_sourcesLock);
	const auto it = g_sources.constFind(id);
	if (it == g_sources.constEnd())
		return false;
	source = *it; // implicitly shared; the worker owns a stable generation
	return true;
}

// One ShaderRenderer per thread. An OpenGL context belongs to the thread that
// made it current, and the preview renders on the GUI thread while the exporter
// renders on its worker -- so each compiles the same shader into its own.
//
// A renderer holds ONE chain of one layer here rather than the whole project's
// chain: a component knows only about itself, and the cost of that is a program
// switch per component per frame, which is nothing beside the upload.
class ThreadRenderers {
public:
	static ThreadRenderers &get()
	{
		static thread_local ThreadRenderers r;
		return r;
	}

	ShaderRenderer *rendererFor(const QString &id, const ShaderSource &src, QString &error)
	{
		if (!triedGl_) {
			triedGl_ = true;
			glOk_ = r_.ensureGl();
		}
		if (!glOk_) {
			error = QStringLiteral("OpenGL 3.3 is unavailable. %1 "
				"Update the graphics driver and restart the editor, or disable this shader.")
				.arg(r_.lastError());
			return nullptr;
		}
		const auto failed = failedCompiles_.constFind(id);
		if (failed != failedCompiles_.cend() && failed->first == src.generation) {
			error = failed->second;
			return nullptr;
		}
		if (compiledId_ == id && compiledGen_ == src.generation) {
			if (!compileOk_)
				error = compileError_;
			return compileOk_ ? &r_ : nullptr;
		}
		compiledId_ = id;
		compiledGen_ = src.generation;
		compileOk_ = r_.setChain({ShaderLayerSource{src.wrapped, src.defs}});
		if (!compileOk_) {
			compileError_ = QStringLiteral("The shader could not compile or link. "
				"Fix its .frag file and reload components, or disable this effect.\n%1")
				.arg(r_.lastError());
			error = compileError_;
			if (failedCompiles_.size() >= 128)
				failedCompiles_.erase(failedCompiles_.begin());
			failedCompiles_.insert(id, {src.generation, compileError_});
		} else {
			compileError_.clear();
			failedCompiles_.remove(id);
		}
		return compileOk_ ? &r_ : nullptr;
	}

	bool glUsable()
	{
		if (!triedGl_) {
			triedGl_ = true;
			glOk_ = r_.ensureGl();
		}
		return glOk_;
	}

	// Give the GL objects back while this thread is still fully itself. See
	// ShaderComponents::releaseThreadResources.
	void release()
	{
		r_.release();
		triedGl_ = false;
		glOk_ = false;
		compileOk_ = false;
		compiledId_.clear();
		compileError_.clear();
		failedCompiles_.clear();
		compiledGen_ = -1;
	}

private:
	ShaderRenderer r_;
	bool triedGl_ = false, glOk_ = false, compileOk_ = false;
	QString compiledId_;
	QString compileError_;
	QMap<QString, QPair<int, QString>> failedCompiles_;
	int compiledGen_ = -1;
};

class ShaderComponent : public IComponent {
public:
	explicit ShaderComponent(QString id) : id_(std::move(id)) {}

	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		if (!io.frame || io.frame->isNull())
			return;
		ShaderSource source;
		if (!sourceFor(id_, source)) {
			io.renderError = QStringLiteral("The shader source is missing or unreadable. "
				"Restore its .frag file in the shaders folder and reload components, "
				"or disable/remove this effect.");
			return;
		}
		ShaderRenderer *r = ThreadRenderers::get().rendererFor(id_, source, io.renderError);
		if (!r)
			return;

		// The uniforms come from the resolved properties, so a keyframed
		// parameter animates for free -- the renderer never learns that some of
		// these numbers were interpolated.
		QMap<QString, double> params;
		for (const ShaderParam &p : source.defs)
			params.insert(p.uniform, ctx.f(p.uniform.toLatin1().constData(), p.def));

		// iTime is clip-relative, like every other time a component sees, so a
		// shader animates from where its clip starts rather than from wherever
		// the project happens to begin.
		const float t = float(ctx.tSec());
		const int frame = int(ctx.tSec() * ctx.fps);
		const QImage outImg = r->apply(*io.frame, t, frame, {params});
		if (!outImg.isNull())
			*io.frame = outImg;
		else
			io.renderError = QStringLiteral("The shader could not render this frame. %1 "
				"Try restarting the editor or updating the graphics driver; disable the effect if it still fails.")
				.arg(r->lastError());
	}

	// In the GPU effect engine's session, so a shader next to a blur or a
	// grade shares its upload and read-back instead of doing its own.
	bool hasGpu() const override { return true; }
	bool evaluateGpu(const EvalContext &ctx, GpuFx &gpu) const override
	{
		ShaderSource source;
		if (!sourceFor(id_, source))
			return false; // the CPU fallback reports an actionable failure
		QMap<QString, double> params;
		for (const ShaderParam &p : source.defs)
			params.insert(p.uniform, ctx.f(p.uniform.toLatin1().constData(), p.def));
		return gpu.shader(id_, source.generation, source.wrapped, source.defs, params, float(ctx.tSec()),
				  int(ctx.tSec() * ctx.fps));
	}

private:
	QString id_;
};

} // namespace

bool ShaderComponents::available()
{
	return ThreadRenderers::get().glUsable();
}

void ShaderComponents::releaseThreadResources()
{
	ThreadRenderers::get().release();
	releaseGpuFxThread(); // the effect engine's context on this thread, too
}

int ShaderComponents::generation()
{
	return g_generation.loadAcquire();
}

int ShaderComponents::loadFolder(const QString &folder, ComponentRegistry &reg,
				 QStringList *errors)
{
	const int gen = g_generation.fetchAndAddOrdered(1) + 1;
	const QString absoluteFolder = QDir(folder).absolutePath();
	QMap<QString, ShaderSource> loaded;
	int n = 0;
	for (const QFileInfo &fi : QDir(folder).entryInfoList({QStringLiteral("*.frag")},
							      QDir::Files, QDir::Name)) {
		const QString stem = fi.completeBaseName();
		QFile f(fi.absoluteFilePath());
		if (!f.open(QIODevice::ReadOnly)) {
			if (errors)
				*errors << QStringLiteral("%1.frag: could not be read").arg(stem);
			continue;
		}
		const QString glsl = QString::fromUtf8(f.readAll());
		f.close();

		const QString id = shaderComponentId(stem);
		ShaderSource src;
		src.folder = absoluteFolder;
		src.defs = parseShaderParams(glsl);
		src.wrapped = wrapShaderToy(glsl, src.defs);
		src.generation = gen;
		loaded.insert(id, src);

		ComponentType t;
		t.id = id;
		// The file stem is the name the user gave it; title-casing the first
		// letter is as far as guessing should go. A `//@name Hue Cycle` line
		// names it properly, since a file stem cannot comfortably hold spaces.
		t.displayName = shaderDisplayName(glsl, stem);
		t.category = QStringLiteral("Shader");
		t.stage = Stage::Pixel;
		t.clipKinds = ClipKindVideo | ClipKindImage | ClipKindEffect;
		t.help = QStringLiteral(
				 "GLSL from %1.frag. On a clip it grades that clip; on an effect "
				 "clip it grades everything below.")
				 .arg(stem);
		for (const ShaderParam &p : src.defs) {
			PropDef d;
			d.key = p.uniform;
			d.label = p.label.isEmpty() ? p.uniform : p.label;
			d.type = p.type == ShaderParam::Type::Bool     ? PropType::Bool
			 : p.type == ShaderParam::Type::Color  ? PropType::Color
			 : p.type == ShaderParam::Type::Choice ? PropType::Choice
								       : PropType::Float;
			d.choices = p.choices;
			d.min = p.min;
			d.max = p.max;
			d.def = p.def;
			// Every type, not just Float. A script or shader parameter is no
			// less animatable for being an integer or a switch -- the value
			// resolver handles the kind, and a bool holds between keys.
			d.keyframeable = true;
			t.props.append(d);
		}
		t.make = [id] { return std::unique_ptr<IComponent>(new ShaderComponent(id)); };
		reg.add(t);
		++n;
	}
	// Reloading must retire deleted/unreadable sources as well as replace the
	// surviving files. A worker takes a value snapshot under this lock, so a
	// reload cannot invalidate the source it is currently compiling.
	{
		QWriteLocker lock(&g_sourcesLock);
		for (auto it = g_sources.begin(); it != g_sources.end();) {
			if (it->folder == absoluteFolder) {
				// Keep the registration's name/settings metadata, like an
				// unknown project component. Removing from the registry here
				// would also invalidate another render's ComponentType pointers.
				it = g_sources.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = loaded.cbegin(); it != loaded.cend(); ++it)
			g_sources.insert(it.key(), *it);
	}
	return n;
}

} // namespace harpia
