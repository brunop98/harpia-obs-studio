#pragma once

// How the Multi-Area layout is drawn on the desktop (MultiAreaOverlay): line
// colour, width and style, badges, and the Arrange mode's dim and highlights.
// Edited live from the Developer Panel's Areas tab, saved to QSettings, and
// part of its Copy / Paste JSON -- so, like MainLayoutParams, one table of
// fields feeds load, save and both JSON directions. Kept apart from the
// overlay and the window so it can be checked on its own.

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <Qt>

#include <algorithm>

namespace harpia {

struct AreaStyle {
	QColor lineColor{0x3d, 0xd6, 0x8c}; // outline (the region frame's green)
	int lineWidth = 2;                  // px, 1..12
	int lineStyle = 1;                  // 0 solid, 1 dash, 2 dot, 3 dash-dot
	int cornerRadius = 0;               // px, 0..40
	int idleOpacity = 85;               // % the outlines show at when idle
	int recordingOpacity = 35;          // % while recording (a guide, not a frame)
	int badgeSize = 26;                 // number badge, px across; 0 hides it
	QColor badgeColor{0x3d, 0xd6, 0x8c};
	QColor badgeTextColor{0, 0, 0};
	int arrangeDimPct = 35;             // Arrange: how dark the rest of the screen goes
	int hoverFillPct = 16;              // Arrange: fill of the area under the mouse
	QColor ghostColor{255, 255, 255};   // Arrange: where a click would stamp the next
	int ghostOpacity = 67;              // %

	Qt::PenStyle penStyle() const
	{
		switch (lineStyle) {
		case 0: return Qt::SolidLine;
		case 2: return Qt::DotLine;
		case 3: return Qt::DashDotLine;
		default: return Qt::DashLine;
		}
	}
};

// One entry per field. Integers carry their range, so a pasted or stored value
// is clamped the same way the panel's spin boxes clamp typing.
struct AreaStyleField {
	const char *key;   // QSettings / JSON name
	const char *label; // Developer Panel row
	int AreaStyle::*intMember = nullptr;
	QColor AreaStyle::*colorMember = nullptr;
	int min = 0, max = 0;
};

inline const QVector<AreaStyleField> &areaStyleFields()
{
	static const QVector<AreaStyleField> f = {
		{"lineColor", "Line color", nullptr, &AreaStyle::lineColor},
		{"lineWidth", "Line width", &AreaStyle::lineWidth, nullptr, 1, 12},
		{"lineStyle", "Line style", &AreaStyle::lineStyle, nullptr, 0, 3},
		{"cornerRadius", "Corner radius", &AreaStyle::cornerRadius, nullptr, 0, 40},
		{"idleOpacity", "Opacity when idle (%)", &AreaStyle::idleOpacity, nullptr, 5, 100},
		{"recordingOpacity", "Opacity while recording (%)", &AreaStyle::recordingOpacity, nullptr, 0, 100},
		{"badgeSize", "Number badge size (0 = off)", &AreaStyle::badgeSize, nullptr, 0, 64},
		{"badgeColor", "Badge color", nullptr, &AreaStyle::badgeColor},
		{"badgeTextColor", "Badge number color", nullptr, &AreaStyle::badgeTextColor},
		{"arrangeDimPct", "Arrange: screen dim (%)", &AreaStyle::arrangeDimPct, nullptr, 0, 90},
		{"hoverFillPct", "Arrange: hover fill (%)", &AreaStyle::hoverFillPct, nullptr, 0, 80},
		{"ghostColor", "Arrange: next-area color", nullptr, &AreaStyle::ghostColor},
		{"ghostOpacity", "Arrange: next-area opacity (%)", &AreaStyle::ghostOpacity, nullptr, 10, 100},
	};
	return f;
}

inline QJsonObject areaStyleToJson(const AreaStyle &s)
{
	QJsonObject o;
	for (const AreaStyleField &f : areaStyleFields()) {
		if (f.intMember)
			o[QString::fromLatin1(f.key)] = s.*(f.intMember);
		else
			o[QString::fromLatin1(f.key)] = (s.*(f.colorMember)).name(QColor::HexRgb);
	}
	return o;
}

// Values in `o` applied onto `s` (clamped; bad colours and unknown names
// ignored). Returns how many were applied.
inline int areaStyleFromJson(const QJsonObject &o, AreaStyle &s)
{
	int n = 0;
	for (const AreaStyleField &f : areaStyleFields()) {
		const QJsonValue v = o.value(QString::fromLatin1(f.key));
		if (f.intMember && v.isDouble()) {
			s.*(f.intMember) = std::clamp(v.toInt(), f.min, f.max);
			++n;
		} else if (f.colorMember && v.isString()) {
			const QColor c(v.toString());
			if (c.isValid()) {
				s.*(f.colorMember) = c;
				++n;
			}
		}
	}
	return n;
}

} // namespace harpia
