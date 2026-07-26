#pragma once

// Offscreen GPU renderer that applies a CHAIN of user GLSL fragment shaders to
// an RGBA QImage (each layer's output feeds the next, via ping-pong FBOs). The
// same renderer serves the live preview (GUI thread) and the export encoder
// (worker thread) — construct one per thread and call ensureGl() there, since
// an OpenGL context is bound to the thread that makes it current.
//
// Not a QObject: it holds only GL resources and is driven synchronously.

#include <QImage>
#include <QMap>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QString>
#include <QVector>

#include "ShaderEffect.hpp"

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLShaderProgram;
class QOpenGLFramebufferObject;

namespace harpia {

// One compiled effect in the chain: the fully wrapped fragment shader plus the
// parameter definitions (so apply() can push each uniform with the right type).
struct ShaderLayerSource {
	QString wrapped;
	QVector<ShaderParam> defs;
};

class ShaderRenderer {
public:
	ShaderRenderer();
	~ShaderRenderer();

	ShaderRenderer(const ShaderRenderer &) = delete;
	ShaderRenderer &operator=(const ShaderRenderer &) = delete;

	// Create the offscreen GL context + surface on the CALLING thread. Idempotent;
	// returns false (and stays false) if OpenGL 3.3 is unavailable here.
	bool ensureGl();

	// Compile a chain of effects (applied in order). Returns false and fills *err
	// on the first layer that fails to compile; the previous chain is left intact.
	// An empty list clears the chain (apply() then returns frames untouched).
	bool setChain(const QVector<ShaderLayerSource> &layers, QString *err = nullptr);

	// Run the chain over `src`. perLayerParams[i] holds the values for layer i
	// (missing keys fall back to that layer's declared default). Returns `src`
	// unchanged when GL isn't ready or the chain is empty.
	QImage apply(const QImage &src, float iTime, int iFrame,
		     const QVector<QMap<QString, double>> &perLayerParams);

	bool glAvailable() const { return glReady_; }
	bool hasChain() const { return !passes_.isEmpty(); }
	int passCount() const { return passes_.size(); }
	QString lastError() const { return error_; }

private:
	bool buildQuad();
	void deletePasses();
	void ensureFbos(int w, int h);

	struct Pass {
		QOpenGLShaderProgram *prog = nullptr;
		QVector<ShaderParam> defs;
	};

	QOffscreenSurface *surface_ = nullptr;
	QOpenGLContext *ctx_ = nullptr;
	QVector<Pass> passes_;
	QOpenGLFramebufferObject *fbo_[2] = {nullptr, nullptr};
	int fboW_ = 0, fboH_ = 0;
	QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
	QOpenGLVertexArrayObject vao_;
	unsigned int tex_ = 0; // GLuint input texture (uploaded source frame)

	bool glReady_ = false;
	bool glFailed_ = false;
	QString error_;
};

} // namespace harpia
