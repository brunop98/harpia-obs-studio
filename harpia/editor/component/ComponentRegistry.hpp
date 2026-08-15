#pragma once

// Every kind of component the editor knows about, and the rules for running a
// clip's list of them in the right order.
//
// Built-in and user-authored components register through the SAME door. That is
// not tidiness for its own sake: it is the only way the plugin API stays honest,
// because if the built-ins had a private shortcut then the public path would
// only ever be as good as somebody remembered to keep it.

#include "Component.hpp"

#include <QHash>
#include <QStringList>
#include <QVector>

namespace harpia {

class ComponentRegistry {
public:
	// One per process. Built-ins register into it at startup; scripted ones are
	// added and replaced as their files are loaded and reloaded.
	static ComponentRegistry &instance();

	// Replaces any existing type with the same id, which is what makes hot
	// reload work: recompile the script, re-register, done. Returns false and
	// changes nothing if the record is unusable (no id, or no factory).
	bool add(const ComponentType &type);
	void remove(const QString &id);

	const ComponentType *find(const QString &id) const;
	QVector<ComponentType> all() const; // sorted by category then display name
	QStringList categories() const;

	// Build the behaviour for an instance. Null when the type is unknown — the
	// caller shows a "missing component" row rather than dropping it.
	std::unique_ptr<IComponent> make(const QString &typeId) const;

	// Test seam: a registry that is not the process-wide one.
	void clear();

private:
	QVector<ComponentType> types_;
	// id -> index into types_.
	//
	// find() is not an occasional call. Building one clip's stack asks about
	// FIVE times per component -- resolveOrder alone asks twice -- and a stack
	// is built per clip per frame, so a linear scan was walking every
	// registered type (the built-ins, all fifteen effects, every shader and
	// every script) with a string compare each, thirty times a second per clip.
	QHash<QString, int> byId_;
	void reindex(); // after any change that can move existing indices
	int indexOf(const QString &id) const;
};

// The order a clip's components actually run in, as indices into `list`.
//
// Stage first — that part is not negotiable and not configurable, because it is
// the pipeline's real shape. Within a stage, a component that declares
// requiresIds runs after those, resolved by a topological sort. Anything left
// keeps the order the user put it in, so dragging a row still means something.
//
// A cycle, or a requirement on a component that is not present, cannot be
// satisfied: those components still run (in list order, at the end of their
// stage) and the reason lands in `warnings` for the Inspector to show. Refusing
// to render is the wrong answer — a project that will not open because two
// plugins disagree is worse than one that renders slightly wrong and says so.
QVector<int> resolveOrder(const QVector<ComponentInstance> &list, const ComponentRegistry &reg,
			  QStringList *warnings = nullptr);

// Conflicts are advisory: both components still run. Reported for the Inspector.
QStringList findConflicts(const QVector<ComponentInstance> &list, const ComponentRegistry &reg);

// True when every enabled component on the clip is pure — so its frames may be
// rendered in any order, on any thread, and cached.
bool allPure(const QVector<ComponentInstance> &list, const ComponentRegistry &reg);

} // namespace harpia
