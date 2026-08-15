#include "ShaderRenderer.hpp"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <QThread>
#include <QVector3D>

namespace harpia {

namespace {
// Passthrough vertex shader — the fragment shader does all the work using
// gl_FragCoord (ShaderToy convention), so the quad only needs positions.
const char *kVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

// Destroy a QObject on the thread that owns it.
//
// Both of the ones here are owned objects with thread affinity: a
// QOffscreenSurface is backed by a hidden QWindow, and a QOpenGLContext belongs
// to the thread that created it. `delete` from anywhere else is undefined, and
// "undefined" in a teardown path means a crash report that names a random
// destructor rather than this one.
void deleteGlObject(QObject *o)
{
	if (!o)
		return;
	if (o->thread() == QThread::currentThread()) {
		delete o;
		return;
	}
	// deleteLater hands it to the owning thread's event loop. If that thread is
	// gone the object leaks -- which is the right trade here: this only happens
	// on a path where the alternative is undefined behaviour, and the process is
	// on its way out anyway.
	o->deleteLater();
}
} // namespace

ShaderRenderer::ShaderRenderer() = default;

ShaderRenderer::~ShaderRenderer()
{
	release();
}

// GL teardown, separated from ~ShaderRenderer so it can be called EARLY -- while
// the owning thread is still fully alive -- and so the destructor can decline to
// do it when that is no longer safe. See ShaderComponents::releaseThreadResources.
void ShaderRenderer::release()
{
	// The thread check is not a nicety. QOpenGLContext::makeCurrent ends in
	//
	//     qFatal("Cannot make QOpenGLContext current in a different thread")
	//
	// which aborts the process outright -- no exception, no return value, no
	// way for a caller to recover. So it must never be reached, and the only
	// way to guarantee that is to ask first.
	//
	// It was reached. A thread_local ShaderRenderer on the export thread is
	// destroyed as that thread exits, and the export thread is a std::thread,
	// so Qt only knows it as an ADOPTED QThread. Qt tears that adopted thread's
	// data down through its own thread-detach hook, and the order of that
	// against C++ thread_local destructors is unspecified. Lose the race and
	// QThread::currentThread() hands back a different QThread object than the
	// one the context was created on -- on the very same physical thread -- and
	// the qFatal fires. v0.1.286 died exactly there: the export had finished
	// and written a perfectly good file, and the app aborted on the way out.
	//
	// The null check on the current thread matters at process exit, where a
	// thread_local renderer on the main thread is destroyed after Qt has begun
	// packing up. Two null thread pointers would compare EQUAL and send us
	// straight into makeCurrent, which is the branch this exists to avoid.
	QThread *cur = QThread::currentThread();
	const bool sameThread = ctx_ && cur && ctx_->thread() == cur;
	if (ctx_ && surface_ && sameThread && ctx_->makeCurrent(surface_)) {
		if (tex_)
			ctx_->functions()->glDeleteTextures(1, &tex_);
		deletePasses();
		delete fbo_[0];
		delete fbo_[1];
		fbo_[0] = fbo_[1] = nullptr;
		vbo_.destroy();
		vao_.destroy();
		ctx_->doneCurrent();
	} else if (ctx_) {
		// Wrong thread: the GL objects cannot be freed from here, and trying is
		// fatal. Dropping the handles leaks them -- which only happens on a
		// thread that is already dying, taking its context with it, and a leak
		// on a dead thread is a far better outcome than an abort.
		passes_.clear(); // the QOpenGLShaderProgram* go with the context
		fbo_[0] = fbo_[1] = nullptr;
	}
	// Both are QObjects. Deleting one from a thread that does not own it is
	// undefined, and the surface owns a QWindow, which belongs to the GUI
	// thread. Hand them back rather than destroying them here.
	deleteGlObject(ctx_);
	deleteGlObject(surface_);
	ctx_ = nullptr;
	surface_ = nullptr;
	tex_ = 0;
	fboW_ = fboH_ = 0;
	glReady_ = false;
}

void ShaderRenderer::deletePasses()
{
	for (Pass &p : passes_)
		delete p.prog;
	passes_.clear();
}

bool ShaderRenderer::ensureGl()
{
	if (glReady_)
		return true;
	if (glFailed_)
		return false;

	QSurfaceFormat fmt;
	fmt.setRenderableType(QSurfaceFormat::OpenGL);
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setVersion(3, 3);

	surface_ = new QOffscreenSurface();
	surface_->setFormat(fmt);
	surface_->create();
	if (!surface_->isValid()) {
		error_ = QStringLiteral("Could not create an offscreen GL surface.");
		glFailed_ = true;
		return false;
	}

	ctx_ = new QOpenGLContext();
	ctx_->setFormat(fmt);
	if (!ctx_->create() || !ctx_->makeCurrent(surface_)) {
		error_ = QStringLiteral("Could not create an OpenGL 3.3 context.");
		glFailed_ = true;
		return false;
	}

	if (!buildQuad()) {
		error_ = QStringLiteral("Could not initialise GL geometry.");
		glFailed_ = true;
		ctx_->doneCurrent();
		return false;
	}

	auto *f = ctx_->functions();
	f->glGenTextures(1, &tex_);

	ctx_->doneCurrent();
	glReady_ = true;
	return true;
}

bool ShaderRenderer::buildQuad()
{
	static const float verts[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
		-1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
	};
	if (!vao_.create())
		return false;
	vao_.bind();
	if (!vbo_.create()) {
		vao_.release();
		return false;
	}
	vbo_.bind();
	vbo_.allocate(verts, int(sizeof(verts)));
	auto *f = ctx_->functions();
	f->glEnableVertexAttribArray(0);
	f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
	vbo_.release();
	vao_.release();
	return true;
}

bool ShaderRenderer::setChain(const QVector<ShaderLayerSource> &layers, QString *err)
{
	if (layers.isEmpty()) {
		if (glReady_ && ctx_->makeCurrent(surface_)) {
			deletePasses();
			ctx_->doneCurrent();
		} else {
			deletePasses();
		}
		error_.clear();
		if (err)
			err->clear();
		return true;
	}

	if (!ensureGl()) {
		if (err)
			*err = error_;
		return false;
	}
	if (!ctx_->makeCurrent(surface_)) {
		error_ = QStringLiteral("Could not make the GL context current.");
		if (err)
			*err = error_;
		return false;
	}

	// Compile all layers first; only swap in the new chain if every one links.
	QVector<Pass> built;
	built.reserve(layers.size());
	for (int i = 0; i < layers.size(); ++i) {
		auto *prog = new QOpenGLShaderProgram();
		QString log;
		if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert))
			log = prog->log();
		else if (!prog->addShaderFromSourceCode(QOpenGLShader::Fragment, layers[i].wrapped))
			log = prog->log();
		else if (!prog->link())
			log = prog->log();
		if (!log.isEmpty()) {
			delete prog;
			for (Pass &p : built)
				delete p.prog;
			ctx_->doneCurrent();
			error_ = log;
			if (err)
				*err = log;
			return false;
		}
		built.append({prog, layers[i].defs});
	}

	deletePasses();
	passes_ = built;
	error_.clear();
	ctx_->doneCurrent();
	if (err)
		err->clear();
	return true;
}

void ShaderRenderer::ensureFbos(int w, int h)
{
	if (fbo_[0] && fboW_ == w && fboH_ == h)
		return;
	delete fbo_[0];
	delete fbo_[1];
	fbo_[0] = new QOpenGLFramebufferObject(w, h, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D,
					       GL_RGBA8);
	fbo_[1] = new QOpenGLFramebufferObject(w, h, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D,
					       GL_RGBA8);
	auto *f = ctx_->functions();
	for (QOpenGLFramebufferObject *fb : fbo_) {
		f->glBindTexture(GL_TEXTURE_2D, fb->texture());
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	fboW_ = w;
	fboH_ = h;
}

QImage ShaderRenderer::apply(const QImage &src, float iTime, int iFrame,
			     const QVector<QMap<QString, double>> &perLayerParams)
{
	if (!glReady_ || passes_.isEmpty() || src.isNull())
		return src;
	if (!ctx_->makeCurrent(surface_))
		return src;

	const int w = src.width();
	const int h = src.height();
	auto *f = ctx_->functions();
	ensureFbos(w, h);

	// Upload flipped vertically so the shader samples with the ShaderToy
	// bottom-left origin; the FBO read-back (toImage) flips once more, so the
	// final image comes back upright. (QImage::flipped() is Qt 6.9+.)
	const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
	const QImage up = rgba.flipped(Qt::Vertical);
#else
	const QImage up = rgba.mirrored(false, true);
#endif
	f->glBindTexture(GL_TEXTURE_2D, tex_);
	f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, up.constBits());
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	const int n = passes_.size();
	int lastTarget = 0;
	for (int i = 0; i < n; ++i) {
		const unsigned int inputTex = (i == 0) ? tex_ : fbo_[(i - 1) % 2]->texture();
		QOpenGLFramebufferObject *target = fbo_[i % 2];
		lastTarget = i % 2;

		target->bind();
		f->glViewport(0, 0, w, h);
		f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		f->glClear(GL_COLOR_BUFFER_BIT);

		Pass &p = passes_[i];
		p.prog->bind();
		p.prog->setUniformValue("iResolution", QVector3D(float(w), float(h), 1.0f));
		p.prog->setUniformValue("iTime", iTime);
		p.prog->setUniformValue("iFrame", iFrame);
		f->glActiveTexture(GL_TEXTURE0);
		f->glBindTexture(GL_TEXTURE_2D, inputTex);
		p.prog->setUniformValue("iChannel0", 0);
		const QMap<QString, double> &vals =
			(i < perLayerParams.size()) ? perLayerParams[i] : QMap<QString, double>();
		for (const ShaderParam &sp : p.defs) {
			const double v = vals.value(sp.uniform, sp.def);
			const QByteArray name = sp.uniform.toUtf8();
			if (sp.type == ShaderParam::Type::Bool)
				p.prog->setUniformValue(name.constData(), v != 0.0);
			else
				p.prog->setUniformValue(name.constData(), GLfloat(v));
		}

		vao_.bind();
		f->glDrawArrays(GL_TRIANGLES, 0, 6);
		vao_.release();
		p.prog->release();
	}

	QImage out = fbo_[lastTarget]->toImage(); // flips back to top-down, upright
	ctx_->doneCurrent();
	return out.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace harpia
