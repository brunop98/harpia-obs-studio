#pragma once

// Offscreen GPU renderer that applies a user GLSL fragment shader to an RGBA
// QImage. The exact same renderer serves the live preview (on the GUI thread)
// and the export encoder (on its worker thread) — construct one per thread and
// call ensureGl() there, since an OpenGL context is bound to the thread that
// makes it current.
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

class ShaderRenderer {
public:
	ShaderRenderer();
	~ShaderRenderer();

	ShaderRenderer(const ShaderRenderer &) = delete;
	ShaderRenderer &operator=(const ShaderRenderer &) = delete;

	// Create the offscreen GL context + surface on the CALLING thread. Idempotent;
	// returns false (and stays false) if OpenGL 3.3 is unavailable here.
	bool ensureGl();

	// Compile+link a wrapped fragment shader (see wrapShaderToy). `params` is kept
	// so apply() can push each uniform with the right type. Returns false and fills
	// *err with the compile/link log on failure; the previous program is retained.
	bool setShader(const QString &wrappedGlsl, const QVector<ShaderParam> &params, QString *err = nullptr);

	// Clear the active shader (apply() then returns frames untouched).
	void clearShader() { hasShader_ = false; }

	// Run the shader over `src`, returning a new RGBA8888 image the same size.
	// Returns `src` unchanged when GL isn't ready or no shader is set.
	QImage apply(const QImage &src, float iTime, int iFrame, const QMap<QString, double> &params);

	bool glAvailable() const { return glReady_; }
	bool hasShader() const { return hasShader_; }
	bool ready() const { return glReady_ && hasShader_; }
	QString lastError() const { return error_; }

private:
	bool buildQuad();

	QOffscreenSurface *surface_ = nullptr;
	QOpenGLContext *ctx_ = nullptr;
	QOpenGLShaderProgram *prog_ = nullptr;
	QOpenGLFramebufferObject *fbo_ = nullptr;
	QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
	QOpenGLVertexArrayObject vao_;
	unsigned int tex_ = 0; // GLuint input texture

	bool glReady_ = false;
	bool glFailed_ = false;
	bool hasShader_ = false;
	QVector<ShaderParam> params_;
	QString error_;
};

} // namespace harpia
