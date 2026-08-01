// The shared auto-resume gate: resume only when nobody still objects.
//
// Three systems can pause a recording on their own -- idle, focus-loss,
// region-leave -- and each used to resume off nothing but its own flag. The
// failure that produced: idle pauses, the target app loses focus, the user
// nudges the mouse -> idle resumes -> focus re-pauses a beat later, and a
// slice of the WRONG SCREEN is in a file that cannot be re-taken.
//
// The rule is a pure function over plain facts, so every combination can be
// stated here without a recorder or a cursor. The matrix matters more than
// any single row: the bug was never one system alone, it was the cross terms.
#include "core/AutoPause.hpp"

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("\n-- nothing armed: nothing can object --\n");
	{
		AutoPauseState s; // all defaults: disarmed, focused, inside
		ok(!autoPauseStillWanted(s), "a bare recording is never held paused");
		s.idleSecs = 9999; // stale junk in a DISARMED system's fields
		s.targetFocused = false;
		s.pointerInside = false;
		ok(!autoPauseStillWanted(s),
		   "disarmed systems cannot object, whatever their stale readings say");
	}

	std::printf("\n-- each system objects exactly on its own condition --\n");
	{
		AutoPauseState s;
		s.idleArmed = true;
		s.idleTimeoutSec = 10;
		s.idleSecs = 9.9;
		ok(!autoPauseStillWanted(s), "idle under the timeout: no objection");
		s.idleSecs = 10.0;
		ok(autoPauseStillWanted(s), "idle at the timeout: objection");

		AutoPauseState f;
		f.focusArmed = true;
		f.targetFocused = true;
		ok(!autoPauseStillWanted(f), "target focused: no objection");
		f.targetFocused = false;
		ok(autoPauseStillWanted(f), "target unfocused: objection");

		AutoPauseState r;
		r.regionArmed = true;
		r.pointerInside = true;
		ok(!autoPauseStillWanted(r), "pointer inside: no objection");
		r.pointerInside = false;
		ok(autoPauseStillWanted(r), "pointer outside: objection");
	}

	std::printf("\n-- the cross terms, which are the actual bug --\n");
	{
		// Idle wants to resume (its own condition cleared) but the target app
		// is still unfocused: the resume must be blocked. This is the exact
		// sequence that used to put the wrong screen in the file.
		AutoPauseState s;
		s.idleArmed = true;
		s.idleTimeoutSec = 10;
		s.idleSecs = 0.0; // user is active again -- idle itself is satisfied
		s.focusArmed = true;
		s.targetFocused = false; // ...but the app is still in the background
		ok(autoPauseStillWanted(s), "active again, target unfocused: still held");
		s.targetFocused = true;
		ok(!autoPauseStillWanted(s), "and released the moment the app comes back");

		// Focus wants to resume but the pointer is still outside the region.
		AutoPauseState t;
		t.focusArmed = true;
		t.targetFocused = true;
		t.regionArmed = true;
		t.pointerInside = false;
		ok(autoPauseStillWanted(t), "focused again, pointer still outside: still held");
		t.pointerInside = true;
		ok(!autoPauseStillWanted(t), "released when the pointer is back too");
	}

	std::printf("\n-- Follow Mouse's exemption is the CALLER's job --\n");
	{
		// The gate itself has no idea Follow Mouse exists: the caller arms the
		// region term only when the region is NOT chasing the pointer. Stated
		// here so the division of labour is pinned: a disarmed region term
		// with the pointer outside must not hold the pause.
		AutoPauseState s;
		s.regionArmed = false; // caller: "Follow Mouse owns the pointer"
		s.pointerInside = false;
		ok(!autoPauseStillWanted(s),
		   "pointer outside a FOLLOWING region holds nothing -- the region will chase it");
	}

	std::printf("\n-- a zero or negative timeout never arms idle --\n");
	{
		AutoPauseState s;
		s.idleArmed = true; // caller armed it, but the timeout says off
		s.idleTimeoutSec = 0;
		s.idleSecs = 9999;
		ok(!autoPauseStillWanted(s), "timeout 0 is 'off', not 'instantly'");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
