#pragma once

// GLSL shaders as components.
//
// Every *.frag in the user's shaders folder becomes its own component type —
// "harpia.shader.crt", "harpia.shader.vignette" — registered exactly like a
// built-in one. A shader's `//@param` lines become its properties, so it gets
// Inspector sliders, keyframes, serialisation and multi-clip editing without a
// line of code per shader.
//
// ONE TYPE PER FILE, rather than one "Shader" component with a file picker.
// The picker version would need a property list that changes when the file
// changes, which the whole system is built on NOT needing: a type declares its
// properties once and everything downstream reads them. It also gives the Add
// Component menu the useful shape — a Shader category listing the shaders by
// name — instead of one entry that reveals nothing until you add it.
//
// WHERE IT RUNS is now the user's choice rather than the program's. The old
// panel applied the chain to the finished picture, whole timeline, always. As a
// Pixel component the same shader grades one clip when it sits on that clip,
// and everything below it when it sits on an effect clip — which is the general
// case the old behaviour was one point in.
//
// GL lives on the thread that renders. One ShaderRenderer per thread, keyed by
// shader name, so the preview thread and the export worker each compile once
// and neither can touch the other's context. Where OpenGL 3.3 is unavailable
// the component passes the frame through untouched rather than failing the
// render.

#include <QString>
#include <QStringList>

namespace harpia {

class ComponentRegistry;

// "crt" -> "harpia.shader.crt". Derived from the file stem, so a shader cannot
// end up with one id here and another where projects are migrated.
inline QString shaderComponentId(const QString &stem)
{
	return QStringLiteral("harpia.shader.") + stem;
}

class ShaderComponents {
public:
	// Register every *.frag in `folder`. Returns how many registered; a file
	// that cannot be read is skipped and its reason appended to *errors, so one
	// unreadable shader does not cost the user the rest.
	//
	// Idempotent — re-registering replaces, which is also how hot reload lands.
	// Each reload bumps a generation counter that makes every thread's cached
	// program recompile the next time it is asked for.
	static int loadFolder(const QString &folder, ComponentRegistry &reg,
			      QStringList *errors = nullptr);

	// False when this build or this machine has no usable OpenGL 3.3. Shader
	// components still register and still serialise; they just render as a
	// pass-through, so opening a project on a machine without GL does not
	// silently throw the shaders away.
	static bool available();
};

} // namespace harpia
