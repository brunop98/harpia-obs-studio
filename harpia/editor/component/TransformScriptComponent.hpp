#pragma once

// Transform scripts as components.
//
// Every *.js in the user's scripts folder becomes its own component type —
// "harpia.script.zoom-in", "harpia.script.shake" — registered like a built-in
// one, so a script gets Inspector sliders from its `//@param` lines, keyframes,
// serialisation and multi-clip editing with no code per script.
//
// These are the PER-CHANNEL scripts documented in script/TransformScript.hpp
// (`function scale(t, u, dur, ctx)`), not the `evaluate(ctx, io)` components in
// ScriptComponent.hpp. Both stay, because they are different jobs rather than
// two spellings of one: a channel script says what a clip's scale IS and reads
// `ctx.base` to compose, while a component script mutates a shared `io` through
// a whole stack. Rewriting the bundled scripts into the other form would have
// changed what they mean, and every script anyone has already written with
// them, to save one page of glue.
//
// ORDER IS THE STACK'S ORDER. The old panel had its own list with its own drag
// handles; as components they sit in the one component list, and moving one up
// or down there is what decides which script wins a channel they both drive.
//
// One type per file for the same reason as the shaders: a type declares its
// properties once, and the Add Component menu gets a Script category listing
// them by name.

#include <QString>
#include <QStringList>

namespace harpia {

class ComponentRegistry;

// "zoom-in" -> "harpia.script.zoom-in". Derived from the file stem, so a script
// cannot have one id here and another where projects are migrated.
inline QString transformScriptComponentId(const QString &stem)
{
	return QStringLiteral("harpia.script.") + stem;
}

class TransformScriptComponents {
public:
	// False in a build made without the scripting engine. Scripts then still
	// register and still serialise, and render as a pass-through, so opening a
	// project in such a build does not throw the scripts away.
	static bool available();

	// Register every *.js in `folder`. Returns how many registered; a file that
	// cannot be read is skipped and its reason appended to *errors. Idempotent —
	// re-registering replaces, and bumps a generation counter so every thread's
	// compiled copy is recompiled on its next frame.
	static int loadFolder(const QString &folder, ComponentRegistry &reg,
			      QStringList *errors = nullptr);
};

} // namespace harpia
