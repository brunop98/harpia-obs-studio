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
// A QJSEngine is not thread-safe, so each thread that renders (the GUI for the
// preview, the export worker) owns its own TransformEvaluator compiled from the
// same source.

#include "../shader/ShaderEffect.hpp" // ShaderParam + parseShaderParams (same //@param format)
#include "../timeline/TimelineModel.hpp"

#include <QJSEngine>
#include <QJSValue>
#include <QMap>
#include <QString>
#include <QVector>

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

	// Compile `source` under `name`. Returns false + *err on a syntax error or if
	// the script throws while being evaluated. Compiled scripts are cached by
	// name, so re-setting the same name replaces it (used by live reload).
	bool compile(const QString &name, const QString &source, QString *err = nullptr);
	bool has(const QString &name) const { return scripts_.contains(name); }
	void forget(const QString &name) { scripts_.remove(name); }

	// Apply `script` to `base` for a clip at `outMs`. Channels the script doesn't
	// define are left as they are in `base`. Never throws: a script error is
	// recorded in lastError() and the base transform is returned unchanged.
	TlTransform apply(const ClipScript &script, const TlTransform &base, const TlClip &clip,
			  qint64 outMs, const ScriptContext &ctx);

	QString lastError() const { return lastError_; }
	void clearError() { lastError_.clear(); }

private:
	struct Compiled {
		QJSValue position, scale, rotation, opacity;
		QJSValue paramsObj; // the //@param values, injected before each call
	};
	QJSEngine engine_;
	QMap<QString, Compiled> scripts_;
	QString lastError_;

	// Call one channel function; returns an undefined QJSValue when absent.
	QJSValue callChannel(const QJSValue &fn, double t, double u, double dur,
			     const ScriptContext &ctx);
};

} // namespace harpia
