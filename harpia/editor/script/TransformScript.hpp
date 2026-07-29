#pragma once

// Per-clip transform scripting.
//
// A script is a small JavaScript file in the user's scripts/ folder that
// defines one OPTIONAL function per animatable channel:
//
//     //@param zoom float 1 6 2.5 Zoom level
//
//     function scale(t, u, dur, ctx)    { return 1 + (zoom - 1) * u; }
//     function position(t, u, dur, ctx) { return { x: 0.5, y: 0.5 }; }
//     function rotation(t, u, dur, ctx) { return 15 * Math.sin(t); }
//     function opacity(t, u, dur, ctx)  { return u; }
//
//   t   — seconds into the clip
//   u   — normalised progress through the clip, 0..1
//   dur — clip length in seconds
//   ctx — { clipW, clipH, canvasW, canvasH, fps, index, globalTime,
//           base: { x, y, scale, rotation, opacity } }
//
// A channel with no function falls through to the clip's own value (its
// Inspector setting, or its keyframes), so scripts compose with hand work and
// new channels can be added later without touching existing scripts.
//
// A channel the script DOES define replaces that value — which would make the
// Inspector's Zoom and Position dead controls on a clip running a zoom script.
// `ctx.base` is how a script avoids that: it holds exactly what the clip would
// be framed at without this script (its pose, or its keyframes at this
// instant), so a well-behaved script multiplies or offsets from it rather than
// ignoring it. The bundled scripts all do. Reading it is optional: a script
// that doesn't keeps the plain absolute behaviour.
//
// Scripts MUST be pure — output depending only on the arguments. The preview
// and the exporter call them in different orders, so anything that accumulates
// state across calls would make the render disagree with what you saw.
//
// The engine is a QuickJS runtime, vendored under harpia/third_party/quickjs.
// A runtime is not thread-safe, so each thread that renders (the GUI for the
// preview, the export worker) owns its own TransformEvaluator compiled from the
// same source. Scripts get the language and Math/JSON and nothing else — no
// file, process or network access is reachable from one.
//
// The engine lives entirely behind a PIMPL, so this header pulls in no
// scripting types and the backend can be swapped without touching callers.
// available() is false only in a build made without the engine, where scripting
// no-ops cleanly instead of failing to compile.

#include "../shader/ShaderEffect.hpp" // ShaderParam + parseShaderParams (same //@param format)
#include "../timeline/TimelineModel.hpp"

#include <QMap>
#include <QString>
#include <QVector>

#include <memory>

namespace harpia {

// The per-clip script assignment: which script, and its parameter values.
struct ClipScript {
	QString name;                 // file stem in the scripts folder ("" = none)
	QMap<QString, double> params; // //@param values

	bool active() const { return !name.isEmpty(); }
	bool operator==(const ClipScript &o) const { return name == o.name && params == o.params; }
	bool operator!=(const ClipScript &o) const { return !(*this == o); }
};

// Extra facts a script can read about what it's animating.
struct ScriptContext {
	int clipW = 0, clipH = 0;
	int canvasW = 0, canvasH = 0;
	double fps = 30.0;
	int index = 0;          // clip index on its track
	double globalTime = 0.0; // seconds into the whole timeline
};

class TransformEvaluator {
public:
	TransformEvaluator();
	~TransformEvaluator();
	TransformEvaluator(const TransformEvaluator &) = delete;
	TransformEvaluator &operator=(const TransformEvaluator &) = delete;

	// False when this build has no scripting engine: compile() then fails with a
	// clear message and apply() returns the transform untouched.
	static bool available();

	// Compile `source` under `name`. Returns false + *err on a syntax error or if
	// the script throws while being evaluated. Compiled scripts are cached by
	// name, so re-setting the same name replaces it (used by live reload).
	bool compile(const QString &name, const QString &source, QString *err = nullptr);
	bool has(const QString &name) const;
	void forget(const QString &name);

	// Which channels a compiled script defines, as a Channels mask. The
	// Inspector uses it to mark the rows a script is driving; 0 for a name that
	// isn't compiled.
	enum Channel { ChanPosition = 1, ChanScale = 2, ChanRotation = 4, ChanOpacity = 8 };
	int channelsOf(const QString &name) const;

	// Apply `script` to `base` for a clip at `outMs`. Channels the script doesn't
	// define are left as they are in `base`. Never throws: a script error is
	// recorded in lastError() and the base transform is returned unchanged.
	TlTransform apply(const ClipScript &script, const TlTransform &base, const TlClip &clip,
			  qint64 outMs, const ScriptContext &ctx);

	// The same thing, told the clip-relative time and the duration directly.
	// A script component already knows both -- and the clip is all apply() ever
	// asked them for -- so this is where the work lives and the overload above
	// is the wrapper, rather than a component having to invent a TlClip.
	TlTransform applyAt(const ClipScript &script, const TlTransform &base, qint64 tMs,
			    qint64 durMs, const ScriptContext &ctx);

	QString lastError() const;
	void clearError();

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

} // namespace harpia
