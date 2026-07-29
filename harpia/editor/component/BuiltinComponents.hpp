#pragma once

// The components that ship with the editor.
//
// They go through the same ComponentRegistry::add() a user's script does, and
// hold no privileges a scripted one lacks. That is the point: if the built-ins
// had a private route, the public one would only ever be as good as somebody
// remembered to keep it, and the first person to find out would be whoever
// tried to write the fourth component.

namespace harpia {

class ComponentRegistry;

// Idempotent — re-registering replaces, which is also how hot reload works.
void registerBuiltinComponents(ComponentRegistry &reg);

} // namespace harpia
