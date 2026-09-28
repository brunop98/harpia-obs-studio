#pragma once

// Effects on the GPU.
//
// A frame is uploaded ONCE, every effect in a row runs on it where it lives
// (begin -> fx / blur / maskAlpha / shader ... -> end), and it comes back
// once. That is what makes even the cheap colour effects worth moving: alone,
// a brightness pass is cheaper on the CPU than the round trip; in a stack with
// a blur, it rides along for free.
//
// Same answers as the CPU. The colour effects apply the very tables the CPU
// builds (fxChannelTable / fxMatrixTables), and the blur, sharpen, glow, noise
// and pixelate shaders repeat the CPU's integer arithmetic, so the two paths
// agree to within rounding. The CPU code stays as the fallback: no GL context,
// a shader that will not compile, or HARPIA_NO_GPU_FX=1 in the environment
// and everything runs exactly as before.
//
// Header-only hook, like EditorLog: the app installs the engine at startup
// (installGpuFx in GpuFxGl.cpp); code built without it -- most unit tests --
// finds no engine and takes the CPU path, with nothing to link.

#include "ShaderEffect.hpp" // ShaderParam

#include <QImage>
#include <QMap>
#include <QSize>
#include <QString>
#include <QVector>

namespace harpia {

class GpuFx {
public:
	virtual ~GpuFx() = default;

	// Upload `frame` and make it the frame the calls below work on. False when
	// there is no GPU here, or a frame is already being worked on (a nested
	// caller then takes its CPU path).
	virtual bool begin(const QImage &frame) = 0;
	// The frame as it is now, and the end of the session. Null if begin failed.
	virtual QImage end() = 0;
	virtual bool active() const = 0;
	virtual QSize size() const = 0;

	// One effect (an FxType as int), with its resolved parameters. False when
	// this type has no GPU version; the frame is then unchanged.
	virtual bool fx(int fxType, const QMap<QString, double> &params) = 0;
	// Spotlight::blurInPlace's three box passes, alpha untouched.
	virtual bool blur(int radiusPx) = 0;
	// Multiply alpha by a coverage map (white = keep) at the frame's size,
	// feathered by `featherPx` of blur first; `invert` keeps the outside.
	virtual bool maskAlpha(const QImage &coverage, int featherPx, bool invert) = 0;
	// A user shader (wrapShaderToy source), ShaderToy conventions.
	virtual bool shader(const QString &key, int generation, const QString &wrapped,
			    const QVector<ShaderParam> &defs, const QMap<QString, double> &params, float iTime,
			    int iFrame) = 0;

	// For a component part-way through its In/Out ramp: remember the frame,
	// run the component, then blend the remembered frame back over by
	// `keepSaved` (0..1) -- the GPU form of ComponentStack's cross-fade.
	virtual void saveInput() = 0;
	virtual bool mixWithSaved(double keepSaved) = 0;
};

// Below this many pixels the upload and read-back cost more than the work.
inline constexpr qint64 kGpuFxMinPixels = 16384;

using GpuFxProvider = GpuFx *(*)();
using GpuFxReleaser = void (*)();

inline GpuFxProvider &gpuFxProvider()
{
	static GpuFxProvider p = nullptr;
	return p;
}

inline GpuFxReleaser &gpuFxReleaser()
{
	static GpuFxReleaser r = nullptr;
	return r;
}

// This thread's engine, or null (not installed, or no usable GL here).
inline GpuFx *gpuFx()
{
	const GpuFxProvider p = gpuFxProvider();
	return p ? p() : nullptr;
}

// An idle engine, for a one-off call from code that is not inside a session.
inline GpuFx *gpuFxIdleFor(const QImage &img)
{
	if (qint64(img.width()) * img.height() < kGpuFxMinPixels)
		return nullptr;
	GpuFx *g = gpuFx();
	return (g && !g->active()) ? g : nullptr;
}

// Free this thread's engine; called with ShaderComponents::releaseThreadResources.
inline void releaseGpuFxThread()
{
	if (const GpuFxReleaser r = gpuFxReleaser())
		r();
}

} // namespace harpia
