#pragma once

// A text clip's STYLE as JSON, without its words.
//
// Shared by the project file and the saved style presets so the two can never
// describe a style differently — and so a preset stays a style, applying to
// whatever a clip already says.

#include "TimelineModel.hpp"

#include <QColor>
#include <QJsonObject>

#include <algorithm>

namespace harpia {

inline QJsonObject textStyleToJson(const TlText &t)
{
	QJsonObject o;
	o[QStringLiteral("font")] = t.fontFamily;
	o[QStringLiteral("size")] = t.fontPx;
	o[QStringLiteral("bold")] = t.bold;
	o[QStringLiteral("italic")] = t.italic;
	o[QStringLiteral("color")] = t.color.name(QColor::HexArgb);
	o[QStringLiteral("outlineW")] = t.outlineWidth;
	o[QStringLiteral("outlineColor")] = t.outlineColor.name(QColor::HexArgb);
	o[QStringLiteral("box")] = t.boxEnabled;
	o[QStringLiteral("boxColor")] = t.boxColor.name(QColor::HexArgb);
	o[QStringLiteral("boxPadX")] = t.boxPadX;
	o[QStringLiteral("boxPadY")] = t.boxPadY;
	o[QStringLiteral("boxOpacity")] = t.boxOpacity;
	o[QStringLiteral("boxRadius")] = t.boxRadius;
	o[QStringLiteral("align")] = t.align;
	o[QStringLiteral("case")] = t.textCase;
	return o;
}

// Overwrites the style fields of `t`, leaving t.text (the words) alone.
// Values a hand-edited or older file could leave unusable are brought back into
// range rather than trusted.
inline void applyTextStyleFromJson(const QJsonObject &o, TlText &t)
{
	t.fontFamily = o.value(QStringLiteral("font")).toString();
	t.fontPx = o.value(QStringLiteral("size")).toInt(64);
	t.bold = o.value(QStringLiteral("bold")).toBool(false);
	t.italic = o.value(QStringLiteral("italic")).toBool(false);
	t.color = QColor(o.value(QStringLiteral("color")).toString());
	t.outlineWidth = o.value(QStringLiteral("outlineW")).toDouble(3.0);
	t.outlineColor = QColor(o.value(QStringLiteral("outlineColor")).toString());
	t.boxEnabled = o.value(QStringLiteral("box")).toBool(false);
	t.boxColor = QColor(o.value(QStringLiteral("boxColor")).toString());
	// "boxPad" was one number for both axes; it seeds them both so an older
	// preset or project keeps the spacing it was saved with.
	const int legacyPad = o.value(QStringLiteral("boxPad")).toInt(-1);
	t.boxPadX = o.value(QStringLiteral("boxPadX")).toInt(legacyPad >= 0 ? legacyPad : 22);
	t.boxPadY = o.value(QStringLiteral("boxPadY")).toInt(legacyPad >= 0 ? legacyPad : 12);
	t.boxOpacity = o.value(QStringLiteral("boxOpacity")).toDouble(0.6);
	t.boxRadius = o.value(QStringLiteral("boxRadius")).toInt(6);
	t.align = o.value(QStringLiteral("align")).toInt(1);
	// Absent in every project written before case styles existed, and 0 there
	// means "as typed" -- which is exactly what those captions did.
	t.textCase = o.value(QStringLiteral("case")).toInt(0);

	if (!t.color.isValid())
		t.color = QColor(0xff, 0xff, 0xff);
	if (!t.outlineColor.isValid())
		t.outlineColor = QColor(0x00, 0x00, 0x00);
	if (!t.boxColor.isValid())
		t.boxColor = QColor(0, 0, 0, 150);
	t.fontPx = std::clamp(t.fontPx, 6, 400);
	t.align = std::clamp(t.align, 0, 2);
	// A style this build has no name for would otherwise draw as nothing
	// recognisable; out of range falls back to the words as typed.
	if (t.textCase < 0 || t.textCase >= kTlTextCaseCount)
		t.textCase = TlCaseAsTyped;
	t.outlineWidth = std::clamp(t.outlineWidth, 0.0, 200.0);
	t.boxPadX = std::clamp(t.boxPadX, 0, 400);
	t.boxPadY = std::clamp(t.boxPadY, 0, 400);
	t.boxOpacity = std::clamp(t.boxOpacity, 0.0, 1.0);
	t.boxRadius = std::clamp(t.boxRadius, 0, 200);
}

} // namespace harpia
