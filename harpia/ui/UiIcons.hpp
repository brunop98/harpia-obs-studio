#pragma once

// Button icons, drawn rather than typed.
//
// These used to be Unicode glyphs in the button's text: "↑", "✕", "⏸". That
// works only if the UI font happens to contain the character, and the Windows
// default does not contain all of them — so several buttons showed up empty,
// with no hint of what they did. Which ones vanish depends on the font, so it
// varies by machine and by Windows version.
//
// Drawing them with QPainter removes the question. There is no font involved,
// nothing to fall back to, they are sharp at any DPI because the icon is
// rendered at the size and device pixel ratio actually asked for, and they take
// the colour they are given so a disabled or highlighted button can tint them.
//
// Deliberately plain shapes: these sit at 14-16 px on a toolbar, where anything
// more detailed turns to mush.

#include <QIcon>
#include <QSize>

class QColor;

namespace harpia {

enum class Glyph {
	ArrowUp,      // move up in a list
	ArrowDown,    // move down in a list
	Cross,        // remove / close
	Play,
	Pause,
	Stop,
	Record,       // filled circle
	SkipStart,    // |< back to the beginning
	Undo,
	Redo,
	ChevronDown,  // a menu button, and an expanded section
	ChevronRight, // a collapsed section
	StepBack,     // < previous keyframe
	StepForward,  // > next keyframe
	Diamond,      // a keyframe
	Magnet,       // snapping
	Speaker,
	SpeakerMuted,
	Keyboard,
	Warning,
	Sparkle,      // an effect clip
};

// An icon of `px` logical pixels square in `colour`. The result is cached, so
// asking for the same glyph repeatedly costs one hash lookup.
QIcon uiIcon(Glyph g, int px = 16, const QColor &colour = QColor());

// The same shape painted straight onto a widget — for the places that draw
// their own chrome (the timeline's clip badges) rather than using a button.
void paintGlyph(QPainter &p, Glyph g, const QRectF &box, const QColor &colour);

} // namespace harpia
