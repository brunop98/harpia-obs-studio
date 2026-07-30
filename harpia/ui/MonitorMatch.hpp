#pragma once

// Which Qt screen is the monitor the Display dropdown is pointing at?
//
// Two independent lists describe the same physical displays, and nothing makes
// them agree:
//
//   - OBS's monitor list, which is what the Display dropdown shows and what the
//     recording actually captures (CaptureManager indexes straight into it);
//   - Qt's QScreen list, which is where the region overlay opens, where the
//     countdown and the screen border are drawn, and what canvasForActivePreset()
//     measures to decide the output size.
//
// If those two resolve to different monitors, the dropdown says one display and
// the recording area lives on another -- the region is drawn and clamped
// against the wrong screen's geometry, and the output is sized from the wrong
// resolution. So the mapping is by device IDENTITY, never by position in a list.
//
// The matching itself needs Win32 (EnumDisplayDevices, QueryDisplayConfig), but
// the DECISION does not, and the decision is the part that can be wrong: which
// evidence to trust, in what order, and when to admit there is no match rather
// than return a confident wrong answer. That lives here, as plain data in and
// an index out, so monitormatch_test can check it on any platform.

#include <QPoint>
#include <QString>
#include <QVector>

namespace harpia {

// A Qt screen, as far as matching cares.
struct QtScreenDesc {
	QString name;  // QScreen::name(): a GDI name on Qt 5, a friendly name on Qt 6
	QPoint topLeft; // QScreen::geometry().topLeft(), logical pixels
};

// A Windows display, as far as matching cares.
struct GdiDisplayDesc {
	QString gdiName;  // "\\\\.\\DISPLAY2"
	QString friendly; // "SMT22A550"; empty when unknown, and NOT unique for twins
	QPoint topLeft;   // native (unscaled) position in the virtual desktop
};

// The Display dropdown's index addresses the OBS monitor list, so that is the
// list it has to be range-checked against.
//
// Clamping it against Qt's screen count instead -- which is what this code used
// to do -- silently rewrites a valid choice to 0 whenever OBS enumerates more
// displays than Qt does. The recording still follows the real index, so the
// overlay ends up on the first monitor while the capture is somewhere else:
// exactly the disagreement this file exists to prevent.
int clampMonitorIndex(int index, int obsMonitorCount);

// Index into `screens` of the one that IS `target`, or -1 when nothing matches
// confidently. `allGdi` is every active Windows display, needed only for the
// arrangement fallback.
//
// -1 is a real answer, not a failure to try: a wrong screen silently puts the
// recording area on the wrong monitor, whereas -1 lets the caller fall back and
// say so in the log.
int matchQtScreen(const QVector<QtScreenDesc> &screens, const GdiDisplayDesc &target,
		  const QVector<GdiDisplayDesc> &allGdi);

// Why a match was chosen, for the log line. Valid until the next call.
const char *lastMatchMethod();

} // namespace harpia
