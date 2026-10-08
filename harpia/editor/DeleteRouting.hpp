#pragma once

// Who does Delete belong to?
//
// The key is claimed by a window-level QShortcut, and Qt consults those BEFORE
// the focused widget's keyPressEvent. So the editor cannot let focus do the
// routing: whatever the shortcut decides is the whole answer. Get it wrong in
// one direction and a mode has no Delete at all (which is what happened -- the
// binding was tied to the Full-editing timeline, so it swallowed the key in
// Multi-Cut and then did nothing); get it wrong in the other and Delete removes
// a clip while you are typing a clip's name.
//
// One function, so the rule is stated once and can be checked without building
// a window.

namespace harpia {

enum class DeleteTarget {
	Nothing,          // nothing selected, or a mode with nothing to delete
	TextCursor,       // a text field has focus: the key belongs to the text
	VoiceoverTake,    // the voiceover lane's selection
	ListKeyframe,     // the row highlighted in the Animation section's keyframe list
	PathKeyframe,     // a key picked on the preview's motion path
	TimelineClips,    // Full editing's selected clips
	MultiCutSegment,  // Multi-Cut's selected cut
};

struct DeleteContext {
	bool editingText = false;       // focus is in a line edit / spin box / text area
	bool voiceoverFocused = false;  // the voiceover lane was the last thing clicked
	bool voiceoverHasSel = false;
	bool keyListFocused = false;    // the Inspector's keyframe list has the keyboard
	bool keyListHasSel = false;     // ... and a row is highlighted
	bool pathKeyPicked = false;     // a motion-path key was the last thing clicked, and is still there
	bool fullEdit = false;
	bool timelineHasSel = false;
	bool multiCut = false;
	bool multiCutHasSel = false;
};

inline DeleteTarget deleteTargetFor(const DeleteContext &c)
{
	// Typing wins over everything. A shortcut that eats Delete inside a text
	// field is not a shortcut, it is a bug that destroys work silently.
	if (c.editingText)
		return DeleteTarget::TextCursor;
	// Then focus, because it is the most specific thing the user did: having
	// just clicked a voiceover take, Delete means that take whatever mode the
	// window is in.
	if (c.voiceoverFocused && c.voiceoverHasSel)
		return DeleteTarget::VoiceoverTake;
	// The keyframe list, likewise: Delete takes the highlighted key. With no
	// row highlighted it does NOTHING rather than falling through to the
	// timeline -- pressing Delete a few times to clear the keys must never take
	// the whole clip with the last press.
	if (c.keyListFocused)
		return c.keyListHasSel ? DeleteTarget::ListKeyframe : DeleteTarget::Nothing;
	// Same idea for a key picked on the preview's motion path: having just
	// clicked a keyframe dot, Delete means that key -- not the whole clip it
	// belongs to, which is what the timeline selection would otherwise say.
	if (c.fullEdit && c.pathKeyPicked)
		return DeleteTarget::PathKeyframe;
	// Otherwise the mode decides, which is what you want after selecting a clip
	// and then going off to drag the playhead or a handle.
	if (c.fullEdit)
		return c.timelineHasSel ? DeleteTarget::TimelineClips : DeleteTarget::Nothing;
	if (c.multiCut)
		return c.multiCutHasSel ? DeleteTarget::MultiCutSegment : DeleteTarget::Nothing;
	// Simple Trim is one clip and two handles: there is nothing to delete.
	return DeleteTarget::Nothing;
}

} // namespace harpia
