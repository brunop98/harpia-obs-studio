#include "SpotlightGl.hpp"

#include "../timeline/Spotlight.hpp"

#include <QCoreApplication>
#include <QString>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSurfaceFormat>
#include <QVector2D>
#include <QVector4D>

#include <algorithm>
#include <cmath>

namespace harpia {

namespace {

// Below this the upload/read-back round trip costs more than the CPU pass it
// replaces. Small frames are exactly where the CPU path is already fast.
constexpr int kMinPixelsForGl = 320 * 240;

// Taps per direction per side. The kernel is sampled at a stride when its
// support is wider than this, which is safe here because a Gaussian that wide
// varies very slowly between neighbouring texels.
constexpr int kHalfTaps = 48;

const char *kVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

// Separable Gaussian. uDir is (1,0) or (0,1) in texels.
const char *kBlurFrag = R"(#version 330 core
uniform sampler2D uTex;
uniform vec2  uTexel;    // 1/size
uniform vec2  uDir;      // (1,0) or (0,1)
uniform float uSigma;
uniform float uStride;   // texels between taps
uniform int   uTaps;     // per side
out vec4 fragColor;
void main() {
    vec2 uv = gl_FragCoord.xy * uTexel;
    // The centre tap carries weight 1; the rest are added in mirrored pairs so
    // the kernel stays exactly symmetric however the stride works out.
    vec3 acc = texture(uTex, uv).rgb;
    float wsum = 1.0;
    for (int i = 1; i <= uTaps; ++i) {
        float x = float(i) * uStride;
        float w = exp(-0.5 * (x * x) / (uSigma * uSigma));
        vec2 off = uDir * x * uTexel;
        acc += w * (texture(uTex, uv + off).rgb + texture(uTex, uv - off).rgb);
        wsum += 2.0 * w;
    }
    // Alpha is left alone: the CPU blur never touches it either, and a
    // composited frame's alpha is meaningful.
    fragColor = vec4(acc / wsum, texture(uTex, uv).a);
}
)";

// The mask + dim pass. Shapes are evaluated analytically from the same numbers
// Spotlight::maskPath builds its QPainterPath from.
const char *kCompositeFrag = R"(#version 330 core
const int MAXM = 16;
uniform sampler2D uOrig;
uniform sampler2D uBlur;
uniform vec2  uTexel;
uniform vec2  uSize;
uniform int   uCount;
uniform vec4  uMaskA[MAXM];   // cx, cy, halfW, halfH   (pixels, y measured DOWN)
uniform vec4  uMaskB[MAXM];   // rotationRadians, cornerRadiusPx, shapeId, unused
uniform vec3  uDimColor;
uniform float uDimAmount;
uniform float uInvert;        // 1 = dim inside the areas instead
uniform float uHasBlur;
out vec4 fragColor;

float insideMask(int i, vec2 p) {
    vec2  c  = uMaskA[i].xy;
    vec2  hs = uMaskA[i].zw;
    float a  = uMaskB[i].x;
    vec2  d  = p - c;
    // Inverse of Qt's QTransform::rotate in a y-down frame, which maps
    // q -> (q.x cos - q.y sin, q.x sin + q.y cos).
    float ca = cos(a), sa = sin(a);
    vec2  q  = vec2(d.x * ca + d.y * sa, -d.x * sa + d.y * ca);
    int shape = int(uMaskB[i].z + 0.5);
    if (shape == 0) {                       // rectangle
        return (abs(q.x) <= hs.x && abs(q.y) <= hs.y) ? 1.0 : 0.0;
    } else if (shape == 1) {                // rounded rectangle, via its SDF
        float r = min(uMaskB[i].y, min(hs.x, hs.y));
        vec2  e = abs(q) - (hs - vec2(r));
        float dist = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0) - r;
        return dist <= 0.0 ? 1.0 : 0.0;
    }
    vec2 n = q / max(hs, vec2(1e-6));       // circle and ellipse
    return dot(n, n) <= 1.0 ? 1.0 : 0.0;
}

void main() {
    vec2 uv = gl_FragCoord.xy * uTexel;
    // The frame was uploaded flipped so the shader works bottom-left like the
    // rest of the renderer; mask geometry is in Qt's top-down pixels, so y has
    // to come back the other way up before any of it means anything.
    vec2 p = vec2(gl_FragCoord.x, uSize.y - gl_FragCoord.y);

    float cover = 0.0;
    for (int i = 0; i < uCount; ++i)
        cover = max(cover, insideMask(i, p));

    // Hard, not smoothstepped: the CPU path paints with antialiasing off, and a
    // soft edge here would be a visible difference between preview and export.
    float dim = (uInvert > 0.5) ? cover : 1.0 - cover;

    vec4  orig = texture(uOrig, uv);
    vec3  col  = mix(orig.rgb, texture(uBlur, uv).rgb, dim * uHasBlur);
    col = mix(col, uDimColor, dim * uDimAmount);
    fragColor = vec4(col, orig.a);
}
)";

// One offscreen context per thread: a GL context belongs to the thread that
// made it current, and the preview renders on the GUI thread while the export
// renders on its worker.
class GlPass {
public:
	static GlPass &forThisThread()
	{
		static thread_local GlPass inst;
		return inst;
	}

	~GlPass()
	{
		// A thread_local outlives the QGuiApplication on the MAIN thread —
		// thread_locals for it are destroyed after main() returns — and tearing
		// down a QOffscreenSurface then walks into a QScreen that no longer
		// exists, which segfaults on the way out of a run that otherwise
		// succeeded. Nothing here is worth reclaiming at that point: the process
		// is ending and the driver frees it all. Worker threads finish while the
		// application is still up, so they take the real path below.
		if (!QCoreApplication::instance())
			return;
		if (ctx_ && surface_ && ctx_->makeCurrent(surface_)) {
			auto *f = ctx_->functions();
			if (tex_)
				f->glDeleteTextures(1, &tex_);
			delete blur_;
			delete comp_;
			delete fboA_;
			delete fboB_;
			delete fboC_;
			if (vbo_)
				vbo_->destroy();
			if (vao_)
				vao_->destroy();
			ctx_->doneCurrent();
		}
		delete vbo_;
		delete vao_;
		delete ctx_;
		delete surface_;
	}

	bool ready()
	{
		if (ok_)
			return true;
		if (failed_)
			return false;
		failed_ = true; // until proven otherwise; every path below returns

		QSurfaceFormat fmt;
		fmt.setRenderableType(QSurfaceFormat::OpenGL);
		fmt.setProfile(QSurfaceFormat::CoreProfile);
		fmt.setVersion(3, 3);

		surface_ = new QOffscreenSurface();
		surface_->setFormat(fmt);
		surface_->create();
		if (!surface_->isValid())
			return false;
		ctx_ = new QOpenGLContext();
		ctx_->setFormat(fmt);
		if (!ctx_->create() || !ctx_->makeCurrent(surface_))
			return false;

		// A context is not the same thing as a GPU. Measured at 1080p with the
		// full blur: 47 ms on the CPU path, 385 ms through llvmpipe — because a
		// software rasteriser runs this shader's ~200 taps per pixel on the same
		// processor, without the wide texture units the design assumes. Under
		// RDP, in a VM, or on a machine whose driver has fallen back, accepting
		// the context would make the editor eight times slower in the name of
		// acceleration. Better to decline and let the CPU path have it.
		if (isSoftwareRenderer()) {
			ctx_->doneCurrent();
			return false;
		}
		if (!buildQuad() || !buildPrograms()) {
			ctx_->doneCurrent();
			return false;
		}
		ctx_->functions()->glGenTextures(1, &tex_);
		ctx_->doneCurrent();
		failed_ = false;
		ok_ = true;
		return true;
	}

	bool run(QImage &frame, const SpotlightSpec &spec, qint64 outMs);

private:
	// Requires a current context. The names are what the common software
	// rasterisers actually report: Mesa's two, and Windows' fallbacks.
	bool isSoftwareRenderer() const
	{
		// An escape hatch for the equivalence test, which has nothing but
		// llvmpipe to compare against and is checking agreement, not speed.
		if (qEnvironmentVariableIsSet("HARPIA_SPOTLIGHT_GL_FORCE"))
			return false;
		const char *r =
			reinterpret_cast<const char *>(ctx_->functions()->glGetString(GL_RENDERER));
		if (!r)
			return true; // cannot tell: assume the worse case
		const QString name = QString::fromLatin1(r).toLower();
		for (const char *bad : {"llvmpipe", "softpipe", "swrast", "software rasterizer",
					"basic render", "gdi generic"})
			if (name.contains(QLatin1String(bad)))
				return true;
		return false;
	}

	bool buildQuad()
	{
		static const float verts[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f,  1.0f,
					      -1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, 1.0f};
		// Heap-allocated, not members by value: when the process exits, the
		// destructor above has to be able to walk away without touching them,
		// and ~QOpenGLVertexArrayObject makes a QOffscreenSurface of its own to
		// get a context — which is precisely what crashes once the
		// QGuiApplication is gone. A by-value member would run anyway.
		vao_ = new QOpenGLVertexArrayObject();
		vbo_ = new QOpenGLBuffer();
		if (!vao_->create())
			return false;
		vao_->bind();
		if (!vbo_->create()) {
			vao_->release();
			return false;
		}
		vbo_->bind();
		vbo_->allocate(verts, int(sizeof(verts)));
		auto *f = ctx_->functions();
		f->glEnableVertexAttribArray(0);
		f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
		vbo_->release();
		vao_->release();
		return true;
	}

	static QOpenGLShaderProgram *compile(const char *frag)
	{
		auto *p = new QOpenGLShaderProgram();
		if (!p->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) ||
		    !p->addShaderFromSourceCode(QOpenGLShader::Fragment, frag) || !p->link()) {
			delete p;
			return nullptr;
		}
		return p;
	}

	bool buildPrograms()
	{
		blur_ = compile(kBlurFrag);
		comp_ = compile(kCompositeFrag);
		return blur_ && comp_;
	}

	QOpenGLFramebufferObject *makeFbo(int w, int h)
	{
		auto *fb = new QOpenGLFramebufferObject(w, h, QOpenGLFramebufferObject::NoAttachment,
							GL_TEXTURE_2D, GL_RGBA8);
		auto *f = ctx_->functions();
		f->glBindTexture(GL_TEXTURE_2D, fb->texture());
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return fb;
	}

	void ensureFbos(int w, int h)
	{
		if (fboA_ && w_ == w && h_ == h)
			return;
		delete fboA_;
		delete fboB_;
		delete fboC_;
		fboA_ = makeFbo(w, h);
		fboB_ = makeFbo(w, h);
		fboC_ = makeFbo(w, h);
		w_ = w;
		h_ = h;
	}

	void drawQuad()
	{
		vao_->bind();
		ctx_->functions()->glDrawArrays(GL_TRIANGLES, 0, 6);
		vao_->release();
	}

	QOpenGLContext *ctx_ = nullptr;
	QOffscreenSurface *surface_ = nullptr;
	QOpenGLShaderProgram *blur_ = nullptr;
	QOpenGLShaderProgram *comp_ = nullptr;
	QOpenGLFramebufferObject *fboA_ = nullptr;
	QOpenGLFramebufferObject *fboB_ = nullptr;
	QOpenGLFramebufferObject *fboC_ = nullptr;
	QOpenGLVertexArrayObject *vao_ = nullptr;
	QOpenGLBuffer *vbo_ = nullptr;
	unsigned int tex_ = 0;
	int w_ = 0, h_ = 0;
	bool ok_ = false, failed_ = false;
};

bool GlPass::run(QImage &frame, const SpotlightSpec &spec, qint64 outMs)
{
	if (!ctx_->makeCurrent(surface_))
		return false;

	const int w = frame.width();
	const int h = frame.height();
	auto *f = ctx_->functions();
	ensureFbos(w, h);

	// Gather the areas exactly as the CPU path resolves them: poses at this
	// instant, anything faded below half not cut at all.
	QVector4D a[kMaxGlMasks];
	QVector4D b[kMaxGlMasks];
	int count = 0;
	for (const SpotMask &m : spec.masks) {
		if (!m.enabled || count >= kMaxGlMasks)
			continue;
		const SpotPose pose = m.poseAt(outMs);
		if (pose.visible < 0.5)
			continue;
		// Circle: round on SCREEN, so its height comes from the width in
		// pixels — the same rule maskPath uses, not the normalised height.
		const double wpx = std::max(1.0, pose.w * w);
		const double hpx = (m.shape == SpotShape::Circle) ? wpx : std::max(1.0, pose.h * h);
		const double rad = std::clamp(pose.radius, 0.0, 0.5) * std::min(wpx, hpx);
		a[count] = QVector4D(float(pose.cx * w), float(pose.cy * h), float(wpx / 2.0),
				     float(hpx / 2.0));
		b[count] = QVector4D(float(pose.rotation * M_PI / 180.0), float(rad),
				     float(int(m.shape)), 0.0f);
		++count;
	}
	if (count == 0) {
		ctx_->doneCurrent();
		return false; // nothing to cut; let the CPU path take its early out
	}

	const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
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

	const QVector2D texel(1.0f / float(w), 1.0f / float(h));
	const int radius = int(std::lround(std::clamp(spec.blur, 0.0, 1.0) * 0.06 * std::min(w, h)));

	unsigned int blurTex = tex_;
	if (radius > 0) {
		// Match the CPU's three box passes rather than guessing: convolving
		// three boxes of width n=2r+1 gives variance 3(n^2-1)/12, and a
		// Gaussian of that sigma is what those passes approximate.
		const double n = 2.0 * radius + 1.0;
		const double sigma = std::sqrt(3.0 * (n * n - 1.0) / 12.0);
		const double support = 2.5 * sigma;
		const double stride = std::max(1.0, support / kHalfTaps);
		const int taps = int(std::min(double(kHalfTaps), std::ceil(support / stride)));

		auto pass = [&](unsigned int inTex, QOpenGLFramebufferObject *target, float dx,
				float dy) {
			target->bind();
			f->glViewport(0, 0, w, h);
			blur_->bind();
			f->glActiveTexture(GL_TEXTURE0);
			f->glBindTexture(GL_TEXTURE_2D, inTex);
			blur_->setUniformValue("uTex", 0);
			blur_->setUniformValue("uTexel", texel);
			blur_->setUniformValue("uDir", QVector2D(dx, dy));
			blur_->setUniformValue("uSigma", GLfloat(sigma));
			blur_->setUniformValue("uStride", GLfloat(stride));
			blur_->setUniformValue("uTaps", taps);
			drawQuad();
			blur_->release();
			target->release();
		};
		pass(tex_, fboA_, 1.0f, 0.0f);
		pass(fboA_->texture(), fboB_, 0.0f, 1.0f);
		blurTex = fboB_->texture();
	}

	fboC_->bind();
	f->glViewport(0, 0, w, h);
	comp_->bind();
	f->glActiveTexture(GL_TEXTURE0);
	f->glBindTexture(GL_TEXTURE_2D, tex_);
	comp_->setUniformValue("uOrig", 0);
	f->glActiveTexture(GL_TEXTURE1);
	f->glBindTexture(GL_TEXTURE_2D, blurTex);
	comp_->setUniformValue("uBlur", 1);
	comp_->setUniformValue("uTexel", texel);
	comp_->setUniformValue("uSize", QVector2D(float(w), float(h)));
	comp_->setUniformValue("uCount", count);
	comp_->setUniformValueArray("uMaskA", a, count);
	comp_->setUniformValueArray("uMaskB", b, count);
	const QColor dc = spec.dimColor;
	comp_->setUniformValue("uDimColor",
			       QVector3D(float(dc.redF()), float(dc.greenF()), float(dc.blueF())));
	comp_->setUniformValue("uDimAmount", GLfloat(std::clamp(spec.dimOpacity, 0.0, 1.0)));
	comp_->setUniformValue("uInvert", spec.invert ? 1.0f : 0.0f);
	comp_->setUniformValue("uHasBlur", radius > 0 ? 1.0f : 0.0f);
	drawQuad();
	comp_->release();
	fboC_->release();
	f->glActiveTexture(GL_TEXTURE0);

	QImage out = fboC_->toImage(); // flips back upright
	ctx_->doneCurrent();
	if (out.isNull())
		return false;
	frame = out.convertToFormat(QImage::Format_RGBA8888);
	return true;
}

} // namespace

bool SpotlightGl::available()
{
	return GlPass::forThisThread().ready();
}

bool SpotlightGl::tryApply(QImage &frame, const SpotlightSpec &spec, qint64 outMs)
{
	if (frame.isNull() || !spec.active())
		return false;
	if (qint64(frame.width()) * frame.height() < kMinPixelsForGl)
		return false;
	int enabled = 0;
	for (const SpotMask &m : spec.masks)
		if (m.enabled)
			++enabled;
	if (enabled > kMaxGlMasks)
		return false; // truncating areas would be worse than being slow
	GlPass &g = GlPass::forThisThread();
	if (!g.ready())
		return false;
	return g.run(frame, spec, outMs);
}

} // namespace harpia
