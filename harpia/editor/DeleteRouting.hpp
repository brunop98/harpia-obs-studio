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
	TimelineClips,    // Full editing's selected clips
	MultiCutSegment,  // Multi-Cut's selected cut
};

struct DeleteContext {
	bool editingText = false;       // focus is in a line edit / spin box / text area
	bool voiceoverFocused = false;  // the voiceover lane was the last thing clicked
	bool voiceoverHasSel = false;
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
