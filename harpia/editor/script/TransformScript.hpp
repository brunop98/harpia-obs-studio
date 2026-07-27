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
//   ctx — { clipW, clipH, canvasW, canvasH, fps, index, globalTime }
//
// A channel with no function falls through to the clip's own value (its
// Inspector setting, or its keyframes), so scripts compose with hand work and
// new channels can be added later without touching existing scripts.
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

	// Apply `script` to `base` for a clip at `outMs`. Channels the script doesn't
	// define are left as they are in `base`. Never throws: a script error is
	// recorded in lastError() and the base transform is returned unchanged.
	TlTransform apply(const ClipScript &script, const TlTransform &base, const TlClip &clip,
			  qint64 outMs, const ScriptContext &ctx);

	QString lastError() const;
	void clearError();

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

} // namespace harpia
