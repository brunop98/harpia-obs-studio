// The OpenGL engine behind GpuFx.hpp. See that header for the contract.
//
// One engine per thread (the preview composes on the GUI thread, the exporter
// on its worker), each with its own offscreen 3.3 context -- the same rules
// ShaderRenderer lives by, including freeing on the right thread.
//
// Orientation: texture row r IS image row r, and every pass reads the texel at
// gl_FragCoord, so nothing is ever flipped -- except around a user shader,
// which expects ShaderToy's bottom-left origin and gets it from a flip in and a
// flip out.
//
// Exactness: colours are read as integers 0..255 (texelFetch, round(c*255))
// and every effect repeats the CPU's integer arithmetic -- the same tables for
// the colour effects, the same running box for the blur, the same shifts and
// truncations -- so a frame graded here matches one graded on the CPU to
// within rounding, and the CPU stays a drop-in fallback.

#include "GpuFxGl.hpp"
#include "GpuFx.hpp"

#include "../timeline/EffectClip.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>
#include <QSurfaceFormat>
#include <QThread>
#include <QVector3D>
#include <QVector4D>
#include <QColor>

#include <cmath>
#include <memory>

#ifndef GL_RG32I
#define GL_RG32I 0x823B
#endif
#ifndef GL_R32I
#define GL_R32I 0x8235
#endif
#ifndef GL_RG_INTEGER
#define GL_RG_INTEGER 0x8228
#endif
#ifndef GL_RGBA32I
#define GL_RGBA32I 0x8D82
#endif
#ifndef GL_RGBA_INTEGER
#define GL_RGBA_INTEGER 0x8D99
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif

namespace harpia {

namespace {

const char *kVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

// Shared by every built-in pass: integer reads with edge clamping, as the CPU
// clamps its indices.
const char *kPrelude = R"(#version 330 core
out vec4 fragOut;
uniform sampler2D uSrc;
uniform ivec2 uSize;
ivec2 P() { return ivec2(gl_FragCoord.xy); }
ivec4 L(sampler2D s, ivec2 p) { return ivec4(round(texelFetch(s, clamp(p, ivec2(0), uSize - 1), 0) * 255.0)); }
vec4 W(ivec4 v) { return vec4(clamp(v, 0, 255)) / 255.0; }
)";

// For the passes that write integer sums (the blur's running sums).
const char *kPreludeI = R"(#version 330 core
out ivec4 fragI;
uniform sampler2D uSrc;
uniform isampler2D uI;
uniform ivec2 uSize;
ivec2 P() { return ivec2(gl_FragCoord.xy); }
ivec4 L(sampler2D s, ivec2 p) { return ivec4(round(texelFetch(s, clamp(p, ivec2(0), uSize - 1), 0) * 255.0)); }
)";

// A running sum along a row (or column), built in log2(n) doubling steps, and
// the box read off it: sum = S[hi] - S[lo], plus the edge pixel repeated for
// any part of the window past the edge -- exactly the CPU's clamped window, at
// a cost that no longer grows with the radius.
const char *kScanInit = "void main(){ fragI = L(uSrc, P()); }";
const char *kScanStep = R"(uniform int uOff; uniform int uDx; uniform int uDy;
void main(){ ivec2 p = P(); ivec2 dir = ivec2(uDx, uDy); int c = uDx != 0 ? p.x : p.y;
 ivec4 v = texelFetch(uI, p, 0);
 if (c >= uOff) v += texelFetch(uI, p - dir * uOff, 0);
 fragI = v; })";
const char *kBoxScan = R"(#version 330 core
out vec4 fragOut;
uniform sampler2D uSrc; uniform isampler2D uI; uniform ivec2 uSize;
uniform int uR; uniform int uDx; uniform int uDy;
ivec2 P() { return ivec2(gl_FragCoord.xy); }
ivec4 L(ivec2 p) { return ivec4(round(texelFetch(uSrc, clamp(p, ivec2(0), uSize - 1), 0) * 255.0)); }
ivec2 at(ivec2 p, int k) { return uDx != 0 ? ivec2(k, p.y) : ivec2(p.x, k); }
void main(){ ivec2 p = P(); int n = uDx != 0 ? uSize.x : uSize.y; int c = uDx != 0 ? p.x : p.y;
 int lo = c - uR - 1, hi = min(c + uR, n - 1);
 ivec3 s = texelFetch(uI, at(p, hi), 0).rgb - (lo >= 0 ? texelFetch(uI, at(p, lo), 0).rgb : ivec3(0));
 s += max(0, uR - c) * L(at(p, 0)).rgb + max(0, c + uR - (n - 1)) * L(at(p, n - 1)).rgb;
 int win = 2 * uR + 1;
 fragOut = vec4(clamp((s + win / 2) / win, 0, 255), L(p).a) / 255.0; })";

struct PassSrc {
	const char *name;
	const char *body;
};

const PassSrc kPasses[] = {
	{"copy", "void main(){ fragOut = texelFetch(uSrc, P(), 0); }"},
	{"flip", "void main(){ ivec2 p = P(); fragOut = texelFetch(uSrc, ivec2(p.x, uSize.y - 1 - p.y), 0); }"},
	// Point effects: the CPU's own per-channel table, looked up.
	{"lut", R"(uniform sampler2D uLut;
void main(){ ivec4 c = L(uSrc, P());
 fragOut = vec4(texelFetch(uLut, ivec2(c.r, 0), 0).r, texelFetch(uLut, ivec2(c.g, 0), 0).g,
                texelFetch(uLut, ivec2(c.b, 0), 0).b, float(c.a) / 255.0); })"},
	// Saturation: the CPU's nine 16.16 tables, summed and rounded the same way.
	{"matrix", R"(uniform isampler2D uM;
int M(int i, int v) { return texelFetch(uM, ivec2(v, i), 0).r; }
void main(){ ivec4 c = L(uSrc, P());
 int nr = M(0, c.r) + M(1, c.g) + M(2, c.b);
 int ng = M(3, c.r) + M(4, c.g) + M(5, c.b);
 int nb = M(6, c.r) + M(7, c.g) + M(8, c.b);
 fragOut = W(ivec4((nr + 32768) >> 16, (ng + 32768) >> 16, (nb + 32768) >> 16, c.a)); })"},
	// Hue: EffectClip.cpp's rotateHue, line for line.
	{"hue", R"(uniform float uDeg6;
void main(){ ivec4 c = L(uSrc, P()); int r = c.r, g = c.g, b = c.b;
 int mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
 if (d == 0) { fragOut = W(c); return; }
 float dInv = 1.0 / float(d); float h6;
 if (mx == r) h6 = float(g - b) * dInv; else if (mx == g) h6 = 2.0 + float(b - r) * dInv; else h6 = 4.0 + float(r - g) * dInv;
 h6 += uDeg6; if (h6 < 0.0) h6 += 6.0; if (h6 >= 6.0) h6 -= 6.0;
 int sector = int(h6); float f = h6 - float(sector);
 int m1 = int(float(mn) + float(d) * f + 0.5), m2 = int(float(mn) + float(d) * (1.0 - f) + 0.5);
 ivec3 o;
 if (sector == 0) o = ivec3(mx, m1, mn); else if (sector == 1) o = ivec3(m2, mx, mn);
 else if (sector == 2) o = ivec3(mn, mx, m1); else if (sector == 3) o = ivec3(mn, m2, mx);
 else if (sector == 4) o = ivec3(m1, mn, mx); else o = ivec3(mx, mn, m2);
 fragOut = W(ivec4(o, c.a)); })"},
	// Vignette: the CPU's 8.8 falloff factor, per pixel.
	{"vignette", R"(uniform vec2 uC; uniform float uInner, uInvSpan, uAmount;
void main(){ ivec2 p = P(); ivec4 c = L(uSrc, p);
 float dx = float(p.x) - uC.x, dy = float(p.y) - uC.y;
 float d = sqrt(dx * dx + dy * dy);
 float k = clamp((d - uInner) * uInvSpan, 0.0, 1.0); k = k * k * (3.0 - 2.0 * k);
 int fac = int((1.0 - uAmount * k) * 256.0 + 0.5);
 fragOut = W(ivec4((c.rgb * fac) >> 8, c.a)); })"},
	// Noise: the same 16-bit hash, so the grain is the CPU's grain.
	{"noise", R"(uniform int uAmt; uniform int uMono;
uint H(int x, int y, int s) { uint n = uint(x) * 374761393u + uint(y) * 668265263u + uint(s) * 1274126177u;
 n = (n ^ (n >> 13u)) * 1274126177u; return (n ^ (n >> 16u)) & 0xffffu; }
int G(ivec2 p, int s) { return ((int(H(p.x, p.y, s)) - 32768) * uAmt) >> 16; }
void main(){ ivec2 p = P(); ivec4 c = L(uSrc, p);
 ivec3 n = uMono != 0 ? ivec3(G(p, 1)) : ivec3(G(p, 1), G(p, 2), G(p, 3));
 fragOut = W(ivec4(c.rgb + n, c.a)); })"},
	// Chromatic aberration: the CPU's source-column/row tables.
	{"chromatic", R"(uniform isampler2D uTx; uniform isampler2D uTy;
void main(){ ivec2 p = P(); ivec4 c = L(uSrc, p);
 ivec2 tx = texelFetch(uTx, ivec2(p.x, 0), 0).rg, ty = texelFetch(uTy, ivec2(p.y, 0), 0).rg;
 int r = L(uSrc, ivec2(tx.x, ty.x)).r, b = L(uSrc, ivec2(tx.y, ty.y)).b;
 fragOut = W(ivec4(r, c.g, b, c.a)); })"},
	// Pixelate, down: one texel per block, the integer mean of its pixels.
	{"pixDown", R"(uniform int uBlock;
void main(){ ivec2 b = P() * uBlock; ivec2 e = min(b + uBlock, uSize); ivec3 s = ivec3(0); int n = 0;
 for (int y = b.y; y < e.y; ++y) for (int x = b.x; x < e.x; ++x) { s += L(uSrc, ivec2(x, y)).rgb; ++n; }
 fragOut = W(ivec4(s / max(n, 1), 255)); })"},
	// ...and up: each pixel takes its block's colour, keeping its own alpha.
	{"pixUp", R"(uniform sampler2D uSmall; uniform int uBlock;
void main(){ ivec2 p = P(); ivec4 c = L(uSrc, p);
 fragOut = vec4(texelFetch(uSmall, p / uBlock, 0).rgb, float(c.a) / 255.0); })"},
	// The blur: one running-box pass across (uDir = 1,0) or down (0,1), the
	// same window, rounding and edge clamp as Spotlight::blurInPlace. Alpha is
	// carried through untouched, as there.
	{"box", R"(uniform int uR; uniform int uDx; uniform int uDy;
void main(){ ivec2 p = P(); ivec2 dir = ivec2(uDx, uDy); ivec3 s = ivec3(0);
 for (int i = -uR; i <= uR; ++i) s += L(uSrc, p + dir * i).rgb;
 int win = 2 * uR + 1;
 fragOut = W(ivec4((s + win / 2) / win, L(uSrc, p).a)); })"},
	{"sharpen", R"(uniform sampler2D uSoft; uniform int uAmt;
void main(){ ivec2 p = P(); ivec4 a = L(uSrc, p); ivec3 s = L(uSoft, p).rgb;
 fragOut = W(ivec4(a.rgb + ((uAmt * (a.rgb - s)) >> 8), a.a)); })"},
	{"glowBright", R"(uniform int uCut;
void main(){ ivec4 c = L(uSrc, P()); int l = 218 * c.r + 732 * c.g + 74 * c.b;
 fragOut = W(l <= uCut ? ivec4(0, 0, 0, c.a) : c); })"},
	{"glowAdd", R"(uniform sampler2D uGlow; uniform int uAmt;
void main(){ ivec2 p = P(); ivec4 a = L(uSrc, p); ivec3 s = L(uGlow, p).rgb;
 fragOut = W(ivec4(a.rgb + ((uAmt * s) >> 8), a.a)); })"},
	{"maskMul", R"(uniform sampler2D uCover; uniform int uInvert;
void main(){ ivec2 p = P(); ivec4 c = L(uSrc, p); int cov = L(uCover, p).r;
 int a = uInvert != 0 ? 255 - cov : cov;
 fragOut = W(ivec4(c.rgb, (c.a * a + 127) / 255)); })"},
	{"mix", R"(uniform sampler2D uSaved; uniform float uKeep;
void main(){ ivec2 p = P(); fragOut = mix(texelFetch(uSrc, p, 0), texelFetch(uSaved, p, 0), uKeep); })"},
};

// Destroy a QObject on the thread that owns it (see ShaderRenderer.cpp).
void deleteGlObject(QObject *o)
{
	if (!o)
		return;
	if (o->thread() == QThread::currentThread())
		delete o;
	else
		o->deleteLater();
}

class GlFx final : public GpuFx {
public:
	~GlFx() override { release(); }

	bool usable()
	{
		if (ready_)
			return true;
		if (failed_)
			return false;
		if (qEnvironmentVariableIntValue("HARPIA_NO_GPU_FX") != 0) {
			failed_ = true;
			return false;
		}
		QSurfaceFormat fmt;
		fmt.setRenderableType(QSurfaceFormat::OpenGL);
		fmt.setProfile(QSurfaceFormat::CoreProfile);
		fmt.setVersion(3, 3);
		surface_ = new QOffscreenSurface();
		surface_->setFormat(fmt);
		surface_->create();
		ctx_ = new QOpenGLContext();
		ctx_->setFormat(fmt);
		if (!surface_->isValid() || !ctx_->create() || !ctx_->makeCurrent(surface_) ||
		    ctx_->format().majorVersion() < 3) {
			failed_ = true;
			return false;
		}
		f_ = ctx_->extraFunctions();
		// A software rasteriser (no GPU driver, a VM, a remote session) runs
		// these passes many times slower than the CPU code it would replace --
		// measured at 25x on llvmpipe. Leave the effects on the CPU there.
		{
			const QByteArray ren = QByteArray(reinterpret_cast<const char *>(f_->glGetString(GL_RENDERER)));
			const QByteArray low = ren.toLower();
			const bool software = low.contains("llvmpipe") || low.contains("softpipe") ||
					      low.contains("software") || low.contains("swiftshader") ||
					      low.contains("basic render") || low.contains("gdi generic");
			renderer_ = QString::fromLatin1(ren);
			if (software && qEnvironmentVariableIntValue("HARPIA_FORCE_GPU_FX") == 0) {
				qInfo("harpia: GPU effects off -- '%s' is a software renderer", ren.constData());
				ctx_->doneCurrent();
				failed_ = true;
				return false;
			}
			qInfo("harpia: GPU effects on '%s'", ren.constData());
		}
		static const float verts[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
		vao_.create();
		vao_.bind();
		vbo_.create();
		vbo_.bind();
		vbo_.allocate(verts, int(sizeof verts));
		f_->glEnableVertexAttribArray(0);
		f_->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
		vbo_.release();
		vao_.release();
		f_->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		f_->glPixelStorei(GL_PACK_ALIGNMENT, 4);
		ctx_->doneCurrent();
		ready_ = true;
		return true;
	}

	void release()
	{
		// At process exit the thread_local engine outlives the application
		// object, and freeing a context then crashes inside Qt. The process
		// is going; let the driver take everything back.
		if (!QCoreApplication::instance()) {
			progs_.clear();
			ctx_ = nullptr;
			surface_ = nullptr;
			ready_ = active_ = false;
			return;
		}
		QThread *cur = QThread::currentThread();
		const bool same = ctx_ && cur && ctx_->thread() == cur;
		if (ctx_ && surface_ && same && ctx_->makeCurrent(surface_)) {
			for (auto &kv : progs_)
				delete kv.prog;
			progs_.clear();
			freeTextures();
			vbo_.destroy();
			vao_.destroy();
			ctx_->doneCurrent();
		} else {
			progs_.clear(); // they go with the context
		}
		deleteGlObject(ctx_);
		deleteGlObject(surface_);
		ctx_ = nullptr;
		surface_ = nullptr;
		ready_ = false;
		active_ = false;
		w_ = h_ = 0;
	}

	// ---- GpuFx ---------------------------------------------------------
	bool active() const override { return active_; }
	QSize size() const override { return QSize(w_, h_); }

	bool begin(const QImage &frame) override
	{
		if (active_ || frame.isNull() || !usable() || !current())
			return false;
		const QImage rgba = frame.format() == QImage::Format_RGBA8888
					    ? frame
					    : frame.convertToFormat(QImage::Format_RGBA8888);
		ensureTextures(rgba.width(), rgba.height());
		f_->glBindTexture(GL_TEXTURE_2D, tex_[0]);
		f_->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
		cur_ = 0;
		active_ = true;
		return true;
	}

	QImage end() override
	{
		if (!active_)
			return QImage();
		active_ = false;
		if (!current())
			return QImage();
		QImage out(w_, h_, QImage::Format_RGBA8888);
		f_->glBindFramebuffer(GL_FRAMEBUFFER, fbo_[cur_]);
		f_->glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, out.bits());
		f_->glBindFramebuffer(GL_FRAMEBUFFER, 0);
		ctx_->doneCurrent();
		return out;
	}

	bool fx(int type, const QMap<QString, double> &p) override
	{
		if (!active_ || !current())
			return false;
		const FxType t = FxType(type);
		auto v = [&](const char *k, double d = 0.0) { return p.value(QString::fromLatin1(k), d); };
		switch (t) {
		case FxType::Brightness:
		case FxType::Contrast:
		case FxType::Exposure:
		case FxType::ColorBalance: {
			unsigned char lut[3][256];
			fxChannelTable(t, p, lut);
			uchar rgba[256 * 4];
			for (int i = 0; i < 256; ++i) {
				rgba[i * 4 + 0] = lut[0][i];
				rgba[i * 4 + 1] = lut[1][i];
				rgba[i * 4 + 2] = lut[2][i];
				rgba[i * 4 + 3] = 255;
			}
			uploadAux(lutTex_, 256, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
			return pass("lut", {{"uLut", lutTex_}}, {}, {});
		}
		case FxType::Saturation: {
			static thread_local int m[9][256];
			fxMatrixTables(t, p, m);
			uploadAux(matTex_, 256, 9, GL_R32I, GL_RED_INTEGER, GL_INT, m);
			return pass("matrix", {{"uM", matTex_}}, {}, {});
		}
		case FxType::HueShift: {
			float deg6 = std::fmod(float(v("degrees")) / 60.0f, 6.0f);
			if (deg6 < 0.0f)
				deg6 += 6.0f;
			return pass("hue", {}, {}, {{"uDeg6", deg6}});
		}
		case FxType::Blur:
		case FxType::GaussianBlur:
			return blur(std::max(1, fxRadiusPx(v("radius") * (t == FxType::GaussianBlur ? 1.6 : 1.0), w_, h_)));
		case FxType::Sharpen: {
			// Unsharp mask: a blurred copy, then the difference added back.
			if (!copyTo(3) || !blurTex(3, std::max(1, fxRadiusPx(0.06, w_, h_))))
				return false;
			const int amt = int(std::lround(std::clamp(v("amount"), 0.0, 8.0) * 256.0));
			return pass("sharpen", {{"uSoft", tex_[3]}}, {{"uAmt", amt}}, {});
		}
		case FxType::Vignette: {
			const double cx = w_ / 2.0, cy = h_ / 2.0;
			const double maxR = std::sqrt(cx * cx + cy * cy);
			const double inner = std::max(0.001, v("size", 0.75)) * maxR;
			const double outer = inner + std::max(0.01, v("softness", 0.45)) * maxR;
			return pass("vignette", {}, {},
				    {{"uInner", float(inner)},
				     {"uInvSpan", 1.0f / float(std::max(1.0, outer - inner))},
				     {"uAmount", float(v("amount"))}},
				    {{"uC", float(cx), float(cy)}});
		}
		case FxType::Glow: {
			const int cut = int(std::lround(std::clamp(v("threshold", 0.6), 0.0, 1.0) * 255.0)) * 1024;
			if (!passInto("glowBright", tex_[cur_], 3, {}, {{"uCut", cut}}, {}))
				return false;
			if (!blurTex(3, std::max(1, fxRadiusPx(v("radius", 0.25), w_, h_))))
				return false;
			const int amt = int(std::lround(std::clamp(v("amount"), 0.0, 4.0) * 256.0));
			return pass("glowAdd", {{"uGlow", tex_[3]}}, {{"uAmt", amt}}, {});
		}
		case FxType::Pixelate: {
			const int block = fxPixelateBlock(v("size", 0.02), w_, h_);
			const int sw = (w_ + block - 1) / block, sh = (h_ + block - 1) / block;
			if (!ensureSmall(sw, sh))
				return false;
			if (!drawPass("pixDown", smallFbo_, sw, sh, tex_[cur_], {}, {{"uBlock", block}}, {}, {}))
				return false;
			return pass("pixUp", {{"uSmall", smallTex_}}, {{"uBlock", block}}, {});
		}
		case FxType::Noise: {
			const int amt = int(std::lround(std::clamp(v("amount"), 0.0, 1.0) * 128.0));
			return pass("noise", {}, {{"uAmt", amt}, {"uMono", v("mono", 1.0) >= 0.5 ? 1 : 0}}, {});
		}
		case FxType::ChromaticAberration: {
			const ChromaticTables ct = fxChromaticTables(v("amount"), w_, h_);
			if (ct.empty())
				return true; // under half a pixel: nothing moves, as on the CPU
			std::vector<int> tx(size_t(w_) * 2), ty(size_t(h_) * 2);
			for (int x = 0; x < w_; ++x) {
				tx[size_t(x) * 2] = ct.rx[size_t(x)];
				tx[size_t(x) * 2 + 1] = ct.bx[size_t(x)];
			}
			for (int y = 0; y < h_; ++y) {
				ty[size_t(y) * 2] = ct.ry[size_t(y)];
				ty[size_t(y) * 2 + 1] = ct.by[size_t(y)];
			}
			uploadAux(chromX_, w_, 1, GL_RG32I, GL_RG_INTEGER, GL_INT, tx.data());
			uploadAux(chromY_, h_, 1, GL_RG32I, GL_RG_INTEGER, GL_INT, ty.data());
			return pass("chromatic", {{"uTx", chromX_}, {"uTy", chromY_}}, {}, {});
		}
		case FxType::InverseSelection:
		case FxType::Count:
			break;
		}
		return false;
	}

	bool blur(int radius) override
	{
		if (!active_ || !current())
			return false;
		if (radius < 1)
			return true;
		// In place on the current texture, via the other ping-pong one.
		return blurTex(-1, radius);
	}

	bool maskAlpha(const QImage &coverage, int featherPx, bool invert) override
	{
		if (!active_ || !current() || coverage.size() != QSize(w_, h_))
			return false;
		const QImage cov = coverage.format() == QImage::Format_RGBA8888
					   ? coverage
					   : coverage.convertToFormat(QImage::Format_RGBA8888);
		f_->glBindTexture(GL_TEXTURE_2D, tex_[3]);
		f_->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, cov.constBits());
		if (featherPx > 0 && !blurTex(3, featherPx))
			return false;
		return pass("maskMul", {{"uCover", tex_[3]}}, {{"uInvert", invert ? 1 : 0}}, {});
	}

	bool shader(const QString &key, int generation, const QString &wrapped, const QVector<ShaderParam> &defs,
		    const QMap<QString, double> &params, float iTime, int iFrame) override
	{
		if (!active_ || !current())
			return false;
		const QString id = QStringLiteral("user:%1:%2").arg(key).arg(generation);
		Prog *pr = program(id, wrapped.toUtf8(), true);
		if (!pr)
			return false;
		// ShaderToy's origin is the bottom-left: flip in, run, flip out.
		if (!passInto("flip", tex_[cur_], 3, {}, {}, {}))
			return false;
		f_->glBindFramebuffer(GL_FRAMEBUFFER, fbo_[4]);
		f_->glViewport(0, 0, w_, h_);
		f_->glClearColor(0, 0, 0, 1);
		f_->glClear(GL_COLOR_BUFFER_BIT);
		pr->prog->bind();
		f_->glActiveTexture(GL_TEXTURE0);
		f_->glBindTexture(GL_TEXTURE_2D, tex_[3]);
		pr->prog->setUniformValue(pr->loc("iChannel0"), 0);
		pr->prog->setUniformValue(pr->loc("iResolution"), QVector3D(float(w_), float(h_), 1.0f));
		pr->prog->setUniformValue(pr->loc("iTime"), iTime);
		pr->prog->setUniformValue(pr->loc("iFrame"), iFrame);
		for (const ShaderParam &sp : defs) {
			const int loc = pr->loc(sp.uniform.toUtf8());
			if (loc < 0)
				continue;
			const double val = params.value(sp.uniform, sp.def);
			if (sp.type == ShaderParam::Type::Bool)
				pr->prog->setUniformValue(loc, val != 0.0);
			else if (sp.type == ShaderParam::Type::Color) {
				const QColor c = QColor::fromRgba(QRgb(quint32(std::llround(val))));
				pr->prog->setUniformValue(loc, QVector4D(float(c.redF()), float(c.greenF()),
									float(c.blueF()), float(c.alphaF())));
			} else
				pr->prog->setUniformValue(loc, GLfloat(val));
		}
		vao_.bind();
		f_->glDrawArrays(GL_TRIANGLES, 0, 6);
		vao_.release();
		pr->prog->release();
		return pass("flip", {}, {}, {}, {}, tex_[4]);
	}

	void saveInput() override
	{
		if (active_ && current())
			copyTo(2);
	}

	bool mixWithSaved(double keep) override
	{
		if (!active_ || !current())
			return false;
		return pass("mix", {{"uSaved", tex_[2]}}, {}, {{"uKeep", float(std::clamp(keep, 0.0, 1.0))}});
	}

private:
	struct Prog {
		QOpenGLShaderProgram *prog = nullptr;
		QHash<QByteArray, int> locs;
		int loc(const QByteArray &name)
		{
			auto it = locs.find(name);
			if (it == locs.end())
				it = locs.insert(name, prog->uniformLocation(name.constData()));
			return it.value();
		}
	};
	struct Tex {
		const char *name;
		GLuint id;
	};
	struct IntU {
		const char *name;
		int v;
	};
	struct FloatU {
		const char *name;
		float v;
	};
	struct Vec2U {
		const char *name;
		float x, y;
	};

	bool current() { return ready_ && ctx_ && (QOpenGLContext::currentContext() == ctx_ || ctx_->makeCurrent(surface_)); }

	Prog *program(const QString &name, const QByteArray &fragSrc, bool raw)
	{
		auto it = progs_.find(name);
		if (it != progs_.end())
			return it->prog ? &it.value() : nullptr;
		Prog pr;
		pr.prog = new QOpenGLShaderProgram();
		const QByteArray frag = raw ? fragSrc : QByteArray(kPrelude) + fragSrc;
		if (!pr.prog->addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, kVert) ||
		    !pr.prog->addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, frag) || !pr.prog->link()) {
			qWarning("harpia: GPU effect '%s' did not compile, using the CPU: %s", qUtf8Printable(name),
				 qUtf8Printable(pr.prog->log()));
			delete pr.prog;
			pr.prog = nullptr; // remembered, so it is not retried every frame
		}
		it = progs_.insert(name, pr);
		return it->prog ? &it.value() : nullptr;
	}

	Prog *builtin(const char *name)
	{
		for (const PassSrc &ps : kPasses)
			if (qstrcmp(ps.name, name) == 0)
				return program(QString::fromLatin1(name), QByteArray(ps.body), false);
		return nullptr;
	}

	// Draw `name` reading `src` into `fbo` (w x h), with extra textures on
	// units 1.. and the given uniforms.
	bool drawPass(const char *name, GLuint fbo, int w, int h, GLuint src, std::initializer_list<Tex> texs,
		      std::initializer_list<IntU> ints, std::initializer_list<FloatU> floats,
		      std::initializer_list<Vec2U> vec2s)
	{
		Prog *pr = builtin(name);
		if (!pr)
			return false;
		f_->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		f_->glViewport(0, 0, w, h);
		pr->prog->bind();
		f_->glActiveTexture(GL_TEXTURE0);
		f_->glBindTexture(GL_TEXTURE_2D, src);
		pr->prog->setUniformValue(pr->loc("uSrc"), 0);
		f_->glUniform2i(pr->loc("uSize"), w_, h_);
		int unit = 1;
		for (const Tex &t : texs) {
			f_->glActiveTexture(GL_TEXTURE0 + unit);
			f_->glBindTexture(GL_TEXTURE_2D, t.id);
			pr->prog->setUniformValue(pr->loc(t.name), unit);
			++unit;
		}
		for (const IntU &u : ints)
			pr->prog->setUniformValue(pr->loc(u.name), u.v);
		for (const FloatU &u : floats)
			pr->prog->setUniformValue(pr->loc(u.name), u.v);
		for (const Vec2U &u : vec2s)
			pr->prog->setUniformValue(pr->loc(u.name), u.x, u.y);
		vao_.bind();
		f_->glDrawArrays(GL_TRIANGLES, 0, 6);
		vao_.release();
		pr->prog->release();
		f_->glActiveTexture(GL_TEXTURE0);
		return true;
	}

	// Current (or `src`) -> the other ping-pong texture, which becomes current.
	bool pass(const char *name, std::initializer_list<Tex> texs, std::initializer_list<IntU> ints,
		  std::initializer_list<FloatU> floats, std::initializer_list<Vec2U> vec2s = {}, GLuint src = 0)
	{
		const int dst = 1 - cur_;
		if (!drawPass(name, fbo_[dst], w_, h_, src ? src : tex_[cur_], texs, ints, floats, vec2s))
			return false;
		cur_ = dst;
		return true;
	}

	// `src` -> texture slot `dst` (2..4), current unchanged.
	bool passInto(const char *name, GLuint src, int dst, std::initializer_list<Tex> texs,
		      std::initializer_list<IntU> ints, std::initializer_list<FloatU> floats)
	{
		return drawPass(name, fbo_[dst], w_, h_, src, texs, ints, floats, {});
	}

	bool copyTo(int slot) { return passInto("copy", tex_[cur_], slot, {}, {}, {}); }

	// Three box passes across and down, as Spotlight::blurInPlace. `slot` -1
	// blurs the current frame (ping-pong); 3 blurs slot 3 through slot 4.
	bool blurTex(int slot, int r)
	{
		// Direct windows read 2r+1 texels a pixel; the running sum about
		// 2*log2(n)+3 whatever the radius. Take whichever reads fewer.
		const auto logn = [](int n) { int k = 0; while ((1 << k) < n) ++k; return k; };
		if (2 * r + 1 > 2 * std::max(logn(w_), logn(h_)) + 4 && ensureIntTextures())
			return blurTexScan(slot, r);
		for (int i = 0; i < 3; ++i) {
			if (slot < 0) {
				if (!pass("box", {}, {{"uR", r}, {"uDx", 1}, {"uDy", 0}}, {}) ||
				    !pass("box", {}, {{"uR", r}, {"uDx", 0}, {"uDy", 1}}, {}))
					return false;
			} else {
				if (!passInto("box", tex_[slot], 4, {}, {{"uR", r}, {"uDx", 1}, {"uDy", 0}}, {}) ||
				    !passInto("box", tex_[4], slot, {}, {{"uR", r}, {"uDx", 0}, {"uDy", 1}}, {}))
					return false;
			}
		}
		return true;
	}

	// One direction of one box pass via running sums: `src` (RGBA8) -> `dstFbo`.
	bool boxScanPass(GLuint src, GLuint dstFbo, int r, int dx, int dy)
	{
		Prog *init = program(QStringLiteral("scanInit"), QByteArray(kPreludeI) + kScanInit, true);
		Prog *step = program(QStringLiteral("scanStep"), QByteArray(kPreludeI) + kScanStep, true);
		Prog *box = program(QStringLiteral("boxScan"), QByteArray(kBoxScan), true);
		if (!init || !step || !box)
			return false;
		const auto draw = [&](Prog *pr, GLuint fbo, GLuint rgba, GLuint ints, std::initializer_list<IntU> us) {
			f_->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			f_->glViewport(0, 0, w_, h_);
			pr->prog->bind();
			f_->glActiveTexture(GL_TEXTURE0);
			f_->glBindTexture(GL_TEXTURE_2D, rgba);
			pr->prog->setUniformValue(pr->loc("uSrc"), 0);
			f_->glActiveTexture(GL_TEXTURE1);
			f_->glBindTexture(GL_TEXTURE_2D, ints);
			pr->prog->setUniformValue(pr->loc("uI"), 1);
			f_->glUniform2i(pr->loc("uSize"), w_, h_);
			for (const IntU &u : us)
				pr->prog->setUniformValue(pr->loc(u.name), u.v);
			vao_.bind();
			f_->glDrawArrays(GL_TRIANGLES, 0, 6);
			vao_.release();
			pr->prog->release();
			f_->glActiveTexture(GL_TEXTURE0);
		};
		draw(init, ifbo_[0], src, 0, {});
		int in = 0;
		const int n = dx ? w_ : h_;
		for (int off = 1; off < n; off <<= 1) {
			draw(step, ifbo_[1 - in], 0, itex_[in], {{"uOff", off}, {"uDx", dx}, {"uDy", dy}});
			in = 1 - in;
		}
		draw(box, dstFbo, src, itex_[in], {{"uR", r}, {"uDx", dx}, {"uDy", dy}});
		return true;
	}

	bool blurTexScan(int slot, int r)
	{
		for (int i = 0; i < 3; ++i) {
			if (slot < 0) {
				if (!boxScanPass(tex_[cur_], fbo_[1 - cur_], r, 1, 0))
					return false;
				cur_ = 1 - cur_;
				if (!boxScanPass(tex_[cur_], fbo_[1 - cur_], r, 0, 1))
					return false;
				cur_ = 1 - cur_;
			} else {
				if (!boxScanPass(tex_[slot], fbo_[4], r, 1, 0) || !boxScanPass(tex_[4], fbo_[slot], r, 0, 1))
					return false;
			}
		}
		return true;
	}

	bool ensureIntTextures()
	{
		if (itex_[0] && iw_ == w_ && ih_ == h_)
			return true;
		if (!itex_[0]) {
			f_->glGenTextures(2, itex_);
			f_->glGenFramebuffers(2, ifbo_);
		}
		for (int i = 0; i < 2; ++i) {
			f_->glBindTexture(GL_TEXTURE_2D, itex_[i]);
			f_->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, w_, h_, 0, GL_RGBA_INTEGER, GL_INT, nullptr);
			setParams(GL_NEAREST);
			f_->glBindFramebuffer(GL_FRAMEBUFFER, ifbo_[i]);
			f_->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, itex_[i], 0);
			if (f_->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				f_->glBindFramebuffer(GL_FRAMEBUFFER, 0);
				return false; // no integer targets here: the direct windows it is
			}
		}
		f_->glBindFramebuffer(GL_FRAMEBUFFER, 0);
		iw_ = w_;
		ih_ = h_;
		return true;
	}

	void ensureTextures(int w, int h)
	{
		if (w == w_ && h == h_ && tex_[0])
			return;
		freeFrameTextures();
		w_ = w;
		h_ = h;
		f_->glGenTextures(5, tex_);
		f_->glGenFramebuffers(5, fbo_);
		for (int i = 0; i < 5; ++i) {
			f_->glBindTexture(GL_TEXTURE_2D, tex_[i]);
			f_->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			setParams(GL_LINEAR);
			f_->glBindFramebuffer(GL_FRAMEBUFFER, fbo_[i]);
			f_->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_[i], 0);
		}
		f_->glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	bool ensureSmall(int w, int h)
	{
		if (smallTex_ && smallW_ == w && smallH_ == h)
			return true;
		if (!smallTex_) {
			f_->glGenTextures(1, &smallTex_);
			f_->glGenFramebuffers(1, &smallFbo_);
		}
		f_->glBindTexture(GL_TEXTURE_2D, smallTex_);
		f_->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		setParams(GL_NEAREST);
		f_->glBindFramebuffer(GL_FRAMEBUFFER, smallFbo_);
		f_->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, smallTex_, 0);
		f_->glBindFramebuffer(GL_FRAMEBUFFER, 0);
		smallW_ = w;
		smallH_ = h;
		return true;
	}

	void setParams(GLint filter)
	{
		f_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		f_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		f_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		f_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	void uploadAux(GLuint &tex, int w, int h, GLint internal, GLenum fmt, GLenum type, const void *data)
	{
		if (!tex)
			f_->glGenTextures(1, &tex);
		f_->glBindTexture(GL_TEXTURE_2D, tex);
		f_->glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, data);
		setParams(GL_NEAREST);
	}

	void freeFrameTextures()
	{
		if (tex_[0]) {
			f_->glDeleteTextures(5, tex_);
			f_->glDeleteFramebuffers(5, fbo_);
			for (int i = 0; i < 5; ++i)
				tex_[i] = fbo_[i] = 0;
		}
		w_ = h_ = 0;
	}

	void freeTextures()
	{
		freeFrameTextures();
		if (itex_[0]) {
			f_->glDeleteTextures(2, itex_);
			f_->glDeleteFramebuffers(2, ifbo_);
			itex_[0] = itex_[1] = ifbo_[0] = ifbo_[1] = 0;
			iw_ = ih_ = 0;
		}
		for (GLuint *t : {&lutTex_, &matTex_, &chromX_, &chromY_, &smallTex_})
			if (*t) {
				f_->glDeleteTextures(1, t);
				*t = 0;
			}
		if (smallFbo_) {
			f_->glDeleteFramebuffers(1, &smallFbo_);
			smallFbo_ = 0;
		}
	}

	QOffscreenSurface *surface_ = nullptr;
	QOpenGLContext *ctx_ = nullptr;
	QOpenGLExtraFunctions *f_ = nullptr;
	QOpenGLVertexArrayObject vao_;
	QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
	QHash<QString, Prog> progs_;
	bool ready_ = false, failed_ = false, active_ = false;
	int w_ = 0, h_ = 0, cur_ = 0;
	// 0,1: the frame, ping-pong. 2: saved input (ramps). 3,4: scratch.
	GLuint tex_[5] = {0, 0, 0, 0, 0};
	GLuint fbo_[5] = {0, 0, 0, 0, 0};
	GLuint lutTex_ = 0, matTex_ = 0, chromX_ = 0, chromY_ = 0;
	GLuint smallTex_ = 0, smallFbo_ = 0;
	int smallW_ = 0, smallH_ = 0;
	GLuint itex_[2] = {0, 0}, ifbo_[2] = {0, 0}; // running sums, RGBA32I
	int iw_ = 0, ih_ = 0;
	QString renderer_;
};

GlFx &threadEngine()
{
	static thread_local GlFx e;
	return e;
}

GpuFx *provide()
{
	GlFx &e = threadEngine();
	return e.usable() ? &e : nullptr;
}

void releaseThread()
{
	threadEngine().release();
}

} // namespace

void installGpuFx()
{
	gpuFxProvider() = &provide;
	gpuFxReleaser() = &releaseThread;
}

} // namespace harpia
