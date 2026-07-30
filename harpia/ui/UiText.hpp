#pragma once

// One place that decides how big the app's text is.
//
// The UI had grown a lot of explanatory text — long tooltips especially — and
// at the stock font it reads as heavy. Rather than nudge sizes widget by
// widget, the base font is set once at startup and every role size is derived
// from it, so shrinking the app is a single number and the hierarchy (caption <
// body < heading < timecode) survives the change instead of collapsing.
//
// Tooltips get their own, smaller size. They are the most verbose text here and
// the least deliberately read — a tooltip is glanced at, and at body size a
// three-line one covers what you were pointing at.

#include <QtGlobal>

class QApplication;

#include <QString>

namespace harpia {

// Set the app-wide base font and the tooltip font. Call once, before any
// window is constructed — role sizes below are read off the base font, so
// widgets built earlier would be sized against the un-scaled one.
void applyTextScale(QApplication &app);

// A size in pixels for a stylesheet, as a multiple of the base font. Use the
// named roles below rather than passing raw numbers, so a size means the same
// thing everywhere.
int uiTextPx(double scale);

inline int uiCaptionPx()  { return uiTextPx(0.92); } // errors, hints, status
inline int uiHeadingPx()  { return uiTextPx(1.18); } // dialog headline
inline int uiTimecodePx() { return uiTextPx(1.45); } // the editor's big clock
inline int uiReadoutPx()  { return uiTextPx(1.05); } // inspector value fields

// A byte count as a person reads it: "812 KB", "24.8 MB".
//
// One of these, because there were three, and they did not agree. Two used
// 1024 and one used 1000, so the export dialog's estimate and the size the
// finished file reported afterwards were in different units under the same
// word -- a 5% discrepancy that reads as the estimate being wrong.
//
// 1024, because this is a Windows-first app and Explorer's "MB" is 1024-based.
// Matching the file manager the user is about to check the file in matters more
// than matching the SI committee.
QString humanFileSize(qint64 bytes);

} // namespace harpia
