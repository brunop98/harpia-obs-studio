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

	// Free this thread's GL context and surface. Every worker that renders
	// shader components MUST call this before it finishes.
	//
	// Not tidiness -- correctness. The per-thread renderer is a thread_local,
	// and a thread_local is destroyed as the thread exits. Freeing a GL context
	// means making it current, and QOpenGLContext::makeCurrent ends in a qFatal
	// when the calling thread is not the context's own. On the export thread,
	// which is a std::thread and therefore only an ADOPTED QThread to Qt, that
	// check can fail at exit even though the physical thread has not changed:
	// Qt tears the adopted thread's data down through its own hook, and the
	// order of that against C++ thread_local destructors is unspecified.
	//
	// v0.1.286 aborted exactly there, with the export finished and the file
	// written. The destructor now declines to do the unsafe thing, so this is
	// no longer the difference between working and crashing -- it is the
	// difference between freeing the GPU resources and leaking them until the
	// process ends.
	static void releaseThreadResources();
};

} // namespace harpia
