#pragma once

// Automatic transitions by overlap, the Sony Vegas way: drag one clip over its
// neighbour on the same track and the region they share BECOMES the transition.
// There is nothing to insert and nothing to delete — the overlap is the
// transition, so moving either clip retimes it and pulling them apart removes
// it.
//
// The transition's settings live on the INCOMING clip (the later one), because
// that is the clip whose arrival the transition describes. Copying a clip
// carries its transition; deleting it takes the transition with it; and no
// separate object can be left dangling over a gap.
//
// Rendered inside TimelineCompositor, the one description the preview and the
// exporter share, so a transition cannot look one way on screen and another in
// the file.

#include "Ease.hpp"

#include <QImage>
#include <QString>

namespace harpia {

enum class TransitionType {
	Crossfade = 0,  // the default: one dissolves into the other
	Dissolve,       // pixel-noise dissolve rather than a smooth blend
	FadeThroughBlack,
	FadeThroughWhite,
	WipeLeft,
	WipeRight,
	WipeUp,
	WipeDown,
	Push,           // the incoming shoves the outgoing off
	Slide,          // the incoming slides over a stationary outgoing
	Zoom,
	Blur,
	Iris,           // a rectangle opening from the centre
	Circle,
	Pixelate,
	Count
};

inline constexpr int kTransitionTypeCount = int(TransitionType::Count);

const char *transitionName(TransitionType t);
TransitionType transitionFromInt(int v);

// One clip's incoming transition. `type` and the curves are the settings; the
// DURATION is not stored — it is however much the two clips overlap.
struct TlTransition {
	TransitionType type = TransitionType::Crossfade;
	bool enabled = true;
	// Each side gets its own curve, so the outgoing can leave at a different
	// rate than the incoming arrives.
	TlEase easeOut = TlEase::Linear; // shapes the outgoing clip's departure
	TlEase easeIn = TlEase::Linear;  // shapes the incoming clip's arrival
	bool reverse = false;            // run the effect the other way (wipes, pushes)
	double softness = 0.0;           // 0..1 edge softness, for the types that have an edge

	bool operator==(const TlTransition &o) const
	{
		return type == o.type && enabled == o.enabled && easeOut == o.easeOut &&
		       easeIn == o.easeIn && reverse == o.reverse && softness == o.softness;
	}
	bool operator!=(const TlTransition &o) const { return !(*this == o); }
};

class Transitions {
public:
	// Blend two already-rendered, canvas-sized layers into one. `u` is raw
	// progress 0..1 through the overlap; the curves are applied inside, so the
	// caller passes linear time and every type shapes it the same way.
	//
	// `outgoing` is the clip that is leaving, `incoming` the one arriving.
	static QImage blend(const QImage &outgoing, const QImage &incoming, double u,
			    const TlTransition &tr);

	// True for types with a moving edge, where `softness` does something.
	static bool hasSoftEdge(TransitionType t);
};

} // namespace harpia
