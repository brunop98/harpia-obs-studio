#pragma once

// Running one clip's components for one instant.
//
// This is the whole renderer-facing surface of the system: the compositor asks
// for the clip's state at a time, and gets back the time scale, the pose and
// (for the pixel stage) a mutated frame. It does not know what a component is.
//
// Split into two calls because the caller needs the first answer BEFORE it can
// produce the input to the second: time and transform are settled with no
// pixels in hand, then the frame is decoded, then the pixel stage runs on it.
// Collapsing them into one call would force the compositor to decode a frame it
// might be about to discard.
//
// Behaviours are built once per stack and reused across frames. They hold no
// per-frame state — everything arrives in EvalContext — so one instance is safe
// to call from the GUI thread and an export worker at the same time.

#include "Component.hpp"
#include "ComponentRegistry.hpp"

#include <QStringList>
#include <QVector>

class QImage;

namespace harpia {

class ComponentStack {
public:
	// `reg` must outlive the stack. Resolves the run order once; call again
	// after the component list changes.
	//
	// `conflicts` off skips the conflict scan, which builds a set of every
	// enabled type id to answer a question only the Inspector asks. The
	// renderer builds a stack per clip per FRAME and never reads warnings(),
	// so it was paying for that set thirty times a second per clip to throw
	// the (almost always empty) answer away. On by default: a caller that
	// wants the diagnostics gets them by asking for nothing.
	//
	// Only the CONFLICT scan is optional. Missing types and unresolvable
	// requirements still land in warnings(), and pure() is always computed --
	// a purity flag that silently reads "pure" because someone turned
	// diagnostics off is a trap, not an optimisation.
	ComponentStack(const QVector<ComponentInstance> &list, const ComponentRegistry &reg,
		       bool conflicts = true);

	// Time + Transform, evaluated in resolved order. `seed` carries the clip's
	// own pose so a component that offsets rather than overwrites composes with
	// hand-set values.
	ClipState evaluatePose(const EvalContext &base, const TlTransform &seed) const;

	// The Pixel stage, over a frame that now exists.
	void evaluatePixels(const EvalContext &base, QImage &frame) const;

	// Reasons the Inspector should show: missing types, cycles, conflicts.
	const QStringList &warnings() const { return warnings_; }
	// True when every enabled component is pure, so this clip's frames may be
	// rendered in any order and cached.
	bool pure() const { return pure_; }

	// True when every ENABLED Pixel component only ever reads the pixel it is
	// writing. Such a stack gives the same answer on a sub-rect as on the whole
	// frame, which is what lets an area-bounded effect grade just its region.
	// An empty Pixel stage answers true: there is nothing that could disagree.
	bool pixelStageIsPointOp() const;
	bool isEmpty() const { return entries_.isEmpty(); }

private:
	struct Entry {
		int index = -1; // into the caller's list, for diagnostics
		Stage stage = Stage::Pixel;
		const ComponentType *type = nullptr;
		const ComponentInstance *inst = nullptr;
		std::shared_ptr<IComponent> impl;
	};
	void run(Stage stage, const EvalContext &base, ClipState &io) const;

	QVector<ComponentInstance> owned_; // a stack outlives the caller's vector
	QVector<Entry> entries_;
	QStringList warnings_;
	bool pure_ = true;
};

} // namespace harpia
