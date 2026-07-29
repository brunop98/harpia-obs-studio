#pragma once

// User-written components, in JavaScript.
//
// A file in the user's components/ folder becomes a component that is, from the
// editor's side, indistinguishable from a built-in one: same registry, same
// Inspector, same keyframes, same serialisation.
//
//     //@component acme.pulse
//     //@name Pulse
//     //@category Transform
//     //@stage Transform
//     //@param bpm float 30 240 120 Beats per minute
//     //@param depth float 0 1 0.05 Depth
//
//     function evaluate(ctx, io) {
//         const beat = Math.sin(ctx.t * Math.PI * ctx.p.bpm / 30) * 0.5 + 0.5;
//         io.scale *= 1 + ctx.p.depth * beat;
//     }
//
// ctx — { t, u, dur, outTime, fps, canvasW, canvasH, p:{...} }
// io  — { timeScale, x, y, scale, rotation, opacity }, seeded with what the
//       clip would be without this component, so `io.scale *= …` composes with
//       hand-set values and other components instead of overwriting them. This
//       is the same bargain ctx.base offers the transform scripts.
//
// WHY JAVASCRIPT. QuickJS is already vendored here, it is the language most
// people asking for this will already know, the sandbox is a solved problem
// (a script sees the language, Math and JSON — no file, process or network),
// and hot reload is just re-evaluating a string. C# would mean shipping a
// runtime larger than the entire application and a much harder isolation
// story; LuaJIT, the fast Lua, has a weak ARM64 future.
//
// WHAT SCRIPTS DELIBERATELY CANNOT DO: touch pixels. A per-pixel callback at
// 1080p is two million calls a frame, and a native box blur already measures
// 40 ms — interpreted, it would be a hundred times worse and the feature would
// exist only to disappoint. Scripts compute parameters and pose; pixel work
// belongs to a shader or a built-in. A script declaring a Pixel stage is
// refused with that explanation rather than accepted and left to crawl.
//
// PURITY IS NOT ENFORCEABLE, only asked for. A script can keep state in a
// closure, and if it does, its clip renders differently when scrubbed than when
// played. The runtime cannot detect that; the documentation says so, and
// scriptPurityHint() exists so the loader can flag the obvious cases.

#include "ComponentRegistry.hpp"

#include <QString>
#include <QStringList>

namespace harpia {

class ScriptComponents {
public:
	// False in a build made without the scripting engine; loading then fails
	// with a clear message rather than silently registering nothing.
	static bool available();

	// Parse one script and register it. Replaces any type with the same id,
	// which is how hot reload lands. Returns false and fills *err on a bad
	// header, a syntax error, or a stage scripts cannot serve.
	static bool loadSource(const QString &source, ComponentRegistry &reg, QString *err = nullptr);

	// Every *.js in `folder`. Returns how many registered; a file that fails is
	// skipped and its reason appended to *errors, so one broken script does not
	// cost the user the other nine.
	static int loadFolder(const QString &folder, ComponentRegistry &reg,
			      QStringList *errors = nullptr);

	// A crude look for the obvious way to write an impure component — assigning
	// to something outside evaluate(). Not a guarantee, just a warning the
	// loader can surface before the user wonders why scrubbing misbehaves.
	static bool looksImpure(const QString &source);
};

} // namespace harpia
