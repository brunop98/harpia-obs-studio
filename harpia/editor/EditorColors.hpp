#pragma once

// The editor's palette, in one place.
//
// These colours used to be three near-identical copies of the same anonymous
// namespace, in TimelineView.cpp, TrackEditor.cpp and VoiceoverTrack.cpp — the
// same accent blue and the same playhead red written out three times. Anything
// that changed one had to remember the other two.
//
// Now there is one struct. The Developer Panel edits it live, it is persisted
// like every other dev-panel value, and each widget just paints with what it is
// given. The struct's defaults ARE the shipped palette, so "reset" is a
// default-constructed EditorColors and nothing else.

#include <QColor>

namespace harpia {

struct EditorColors {
	// Surfaces.
	QColor timelineBg{0x15, 0x17, 0x1a};  // behind the multi-track timeline
	QColor panelBg{0x20, 0x22, 0x25};     // the Trim / Multi-Cut / voiceover bars
	QColor gutter{0x1b, 0x1e, 0x23};      // track-header column
	QColor lane{0x1f, 0x22, 0x27};        // a track's row
	QColor laneAlt{0x23, 0x27, 0x2d};     // ...and every other one, for banding
	QColor border{0x30, 0x33, 0x38};
	QColor caption{0x9a, 0x9f, 0xa8};     // secondary text
	QColor accent{0x00, 0xae, 0xef};      // selection + "this is where it lands"

	// Clips, by kind, unselected and selected.
	QColor videoClip{0x2e, 0x4d, 0x6e};
	QColor videoClipSel{0x3a, 0x6e, 0xa5};
	QColor audioClip{0x2c, 0x50, 0x45};
	QColor audioClipSel{0x37, 0x74, 0x63};
	QColor textClip{0x4a, 0x3a, 0x5e};
	QColor textClipSel{0x6b, 0x51, 0x8c};
	QColor waveform{0x6f, 0xd0, 0xb0};

	// Overlays. Each is deliberately unlike the others: they mean different
	// things and are often on screen at the same time.
	QColor playhead{0xe5, 0x48, 0x4d};   // where an edit will land
	QColor hover{0xf5, 0xc0, 0x42};      // where the PREVIEW is looking
	QColor marker{0x5c, 0xd6, 0x8a};     // a noted place, not a time now
	QColor snapGuide{0xff, 0xff, 0xff};  // the magnet, while a drag is held on it
	QColor fade{0xff, 0xd9, 0x6b};       // audio fade envelope + its grips

	bool operator==(const EditorColors &o) const
	{
		return timelineBg == o.timelineBg && panelBg == o.panelBg && gutter == o.gutter &&
		       lane == o.lane && laneAlt == o.laneAlt && border == o.border &&
		       caption == o.caption && accent == o.accent && videoClip == o.videoClip &&
		       videoClipSel == o.videoClipSel && audioClip == o.audioClip &&
		       audioClipSel == o.audioClipSel && textClip == o.textClip &&
		       textClipSel == o.textClipSel && waveform == o.waveform &&
		       playhead == o.playhead && hover == o.hover && marker == o.marker &&
		       snapGuide == o.snapGuide && fade == o.fade;
	}
};

} // namespace harpia
