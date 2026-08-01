#pragma once

// The one question every auto-resume has to ask: does anything else still
// want this recording paused?
//
// Three systems can pause a recording on their own -- user idle, the target
// app losing focus, the pointer leaving the region -- and each used to resume
// off nothing but its OWN flag. So idle would resume the recording while the
// target app was still unfocused, and the focus tick would pause it again a
// beat later: a slice of the wrong screen, in a file that cannot be re-taken.
//
// The rule is now stated once, here, as a pure function over plain facts, so
// it can be tested without a window, a recorder, or a cursor: an auto-resume
// happens only when NO armed system's pause condition still holds. (A resume
// the USER asks for is different -- a human override clears all three systems,
// which is the rule MainWindow::onPauseButton already implements.)

namespace harpia {

struct AutoPauseState {
	// User-idle: armed when the preset sets a timeout.
	bool idleArmed = false;
	double idleSecs = 0.0;
	int idleTimeoutSec = 0;
	// Focus: armed when "Record only one application" names a target.
	bool focusArmed = false;
	bool targetFocused = true;
	// Region-leave: armed in Custom Region mode with a leave timeout set --
	// and NOT when Follow Mouse owns the cursor, because there the pointer
	// being outside is a thing the region is about to fix, not a reason to
	// stay paused.
	bool regionArmed = false;
	bool pointerInside = true;
};

// True while any armed system's pause condition still holds. Callers gate
// their auto-resume on !autoPauseStillWanted(...). Each caller's own
// condition is false by the time it wants to resume, so no self-exclusion is
// needed -- the check is symmetric on purpose.
inline bool autoPauseStillWanted(const AutoPauseState &s)
{
	if (s.idleArmed && s.idleTimeoutSec > 0 && s.idleSecs >= s.idleTimeoutSec)
		return true;
	if (s.focusArmed && !s.targetFocused)
		return true;
	if (s.regionArmed && !s.pointerInside)
		return true;
	return false;
}

} // namespace harpia
