// Who does the Delete key belong to?
//
// Delete is claimed by a window-level QShortcut, and Qt consults those BEFORE
// the focused widget's keyPressEvent. So the shortcut's decision is the whole
// answer -- focus cannot route it afterwards. The binding was tied to the
// Full-editing timeline, which meant it ate the key in Multi-Cut and then did
// nothing: TrackEditor and VoiceoverTrack both had working Delete handlers that
// could never be reached.
//
// Two ways to get this wrong, and they pull in opposite directions:
//   * too narrow, and a mode has no Delete at all -- the bug that shipped;
//   * too broad, and Delete removes a clip while you are typing a clip's name,
//     which destroys work silently and is far worse than the first.
// So the typing case is tested first and hardest.
#include "editor/DeleteRouting.hpp"

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

	std::printf("\n-- typing always wins --\n");
	{
		// Every other flag set as loudly as possible: none of it may matter.
		DeleteContext c;
		c.editingText = true;
		c.voiceoverFocused = c.voiceoverHasSel = true;
		c.fullEdit = c.timelineHasSel = true;
		c.multiCut = c.multiCutHasSel = true;
		ok(deleteTargetFor(c) == DeleteTarget::TextCursor,
		   "a focused text field keeps the key, whatever else is selected");

		// The control for that: the same context with typing off routes
		// somewhere real, so the assertion above is not passing by accident.
		c.editingText = false;
		ok(deleteTargetFor(c) != DeleteTarget::TextCursor,
		   "CONTROL: and without a text field it does route somewhere");
	}

	std::printf("\n-- Multi-Cut, the mode that had no Delete at all --\n");
	{
		DeleteContext c;
		c.multiCut = true;
		c.multiCutHasSel = true;
		ok(deleteTargetFor(c) == DeleteTarget::MultiCutSegment,
		   "a selected cut is deleted");
		c.multiCutHasSel = false;
		ok(deleteTargetFor(c) == DeleteTarget::Nothing, "and nothing selected does nothing");
	}

	std::printf("\n-- Full editing --\n");
	{
		DeleteContext c;
		c.fullEdit = true;
		c.timelineHasSel = true;
		ok(deleteTargetFor(c) == DeleteTarget::TimelineClips, "selected clips are deleted");
		c.timelineHasSel = false;
		ok(deleteTargetFor(c) == DeleteTarget::Nothing, "an empty selection deletes nothing");
	}

	std::printf("\n-- the voiceover lane, whichever mode is showing --\n");
	{
		// Focus is the most specific thing the user did: having just clicked a
		// take, Delete means that take even though a clip is also selected.
		DeleteContext c;
		c.voiceoverFocused = true;
		c.voiceoverHasSel = true;
		c.multiCut = true;
		c.multiCutHasSel = true;
		ok(deleteTargetFor(c) == DeleteTarget::VoiceoverTake,
		   "the focused lane beats the mode's own selection");

		c.fullEdit = true;
		c.multiCut = false;
		c.timelineHasSel = true;
		ok(deleteTargetFor(c) == DeleteTarget::VoiceoverTake, "in Full editing too");

		// But focus alone is not enough -- an empty lane must not swallow the
		// key and leave the selected clip sitting there.
		c.voiceoverHasSel = false;
		ok(deleteTargetFor(c) == DeleteTarget::TimelineClips,
		   "a focused but EMPTY lane falls through to the mode");
	}

	std::printf("\n-- Simple Trim has nothing to delete --\n");
	{
		DeleteContext c; // no mode flags: Simple Trim
		ok(deleteTargetFor(c) == DeleteTarget::Nothing, "one clip and two handles: nothing");
		// Even with stale selection state left over from another mode.
		c.timelineHasSel = true;
		c.multiCutHasSel = true;
		ok(deleteTargetFor(c) == DeleteTarget::Nothing,
		   "and a selection belonging to a mode you are not in is ignored");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
