#pragma once

// The components that ship with the editor.
//
// They go through the same ComponentRegistry::add() a user's script does, and
// hold no privileges a scripted one lacks. That is the point: if the built-ins
// had a private route, the public one would only ever be as good as somebody
// remembered to keep it, and the first person to find out would be whoever
// tried to write the fourth component.

#include <QString>

namespace harpia {

class ComponentRegistry;
enum class FxType;

// The component id for an effect, e.g. FxType::HueShift -> "harpia.fx.hueShift".
// Derived from the effect's own name, so an effect cannot end up with one id
// here and a different one where projects are migrated.
QString effectComponentId(FxType t);

// Idempotent — re-registering replaces, which is also how hot reload works.
void registerBuiltinComponents(ComponentRegistry &reg);

} // namespace harpia
