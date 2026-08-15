#include "ShaderComponent.hpp"

#include "../shader/ShaderEffect.hpp"
#include "../shader/ShaderRenderer.hpp"
#include "Component.hpp"
#include "ComponentRegistry.hpp"

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMap>

namespace harpia {

namespace {

// Bumped on every loadFolder. A cached program carries the generation it was
// compiled at, so a reload invalidates every thread's copy without any of them
// having to be told.
QAtomicInt g_generation{1};

// A shader's source and its parsed parameters, shared by every thread. Written
// only from loadFolder (GUI thread, before any render of the new generation)
// and read from the render threads; the generation bump is what orders the two.
struct ShaderSource {
	QString wrapped;
	QVector<ShaderParam> defs;
	int generation = 0;
};
QMap<QString, ShaderSource> g_sources; // by component id

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

	// Null when GL is unavailable here, or when this shader will not compile;
	// the caller then leaves the frame untouched rather than failing the render.
	ShaderRenderer *rendererFor(const QString &id, const ShaderSource &src)
	{
		if (!triedGl_) {
			triedGl_ = true;
			glOk_ = r_.ensureGl();
		}
		if (!glOk_)
			return nullptr;
		if (compiledId_ == id && compiledGen_ == src.generation)
			return compileOk_ ? &r_ : nullptr;
		compiledId_ = id;
		compiledGen_ = src.generation;
		compileOk_ = r_.setChain({ShaderLayerSource{src.wrapped, src.defs}});
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
		compiledGen_ = -1;
	}

private:
	ShaderRenderer r_;
	bool triedGl_ = false, glOk_ = false, compileOk_ = false;
	QString compiledId_;
	int compiledGen_ = -1;
};

class ShaderComponent : public IComponent {
public:
	explicit ShaderComponent(QString id) : id_(std::move(id)) {}

	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		if (!io.frame || io.frame->isNull())
			return;
		const auto it = g_sources.constFind(id_);
		if (it == g_sources.constEnd())
			return;
		ShaderRenderer *r = ThreadRenderers::get().rendererFor(id_, *it);
		if (!r)
			return; // no GL here: pass the frame through rather than fail

		// The uniforms come from the resolved properties, so a keyframed
		// parameter animates for free -- the renderer never learns that some of
		// these numbers were interpolated.
		QMap<QString, double> params;
		for (const ShaderParam &p : it->defs)
			params.insert(p.uniform, ctx.f(p.uniform.toLatin1().constData(), p.def));

		// iTime is clip-relative, like every other time a component sees, so a
		// shader animates from where its clip starts rather than from wherever
		// the project happens to begin.
		const float t = float(ctx.tSec());
		const int frame = int(ctx.tSec() * ctx.fps);
		const QImage outImg = r->apply(*io.frame, t, frame, {params});
		if (!outImg.isNull())
			*io.frame = outImg;
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
}

int ShaderComponents::loadFolder(const QString &folder, ComponentRegistry &reg,
				 QStringList *errors)
{
	const int gen = g_generation.fetchAndAddOrdered(1) + 1;
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
		src.defs = parseShaderParams(glsl);
		src.wrapped = wrapShaderToy(glsl, src.defs);
		src.generation = gen;
		g_sources.insert(id, src);

		ComponentType t;
		t.id = id;
		// The file stem is the name the user gave it; title-casing the first
		// letter is as far as guessing should go.
		t.displayName = stem.left(1).toUpper() + stem.mid(1);
		t.category = QStringLiteral("Shader");
		t.stage = Stage::Pixel;
		t.help = QStringLiteral(
				 "GLSL from %1.frag. On a clip it grades that clip; on an effect "
				 "clip it grades everything below.")
				 .arg(stem);
		for (const ShaderParam &p : src.defs) {
			PropDef d;
			d.key = p.uniform;
			d.label = p.label.isEmpty() ? p.uniform : p.label;
			d.type = p.type == ShaderParam::Type::Bool    ? PropType::Bool
			 : p.type == ShaderParam::Type::Color ? PropType::Color
							      : PropType::Float;
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
	return n;
}

} // namespace harpia
