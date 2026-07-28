#include "UiText.hpp"

#include <QApplication>
#include <QFont>
#include <QFontInfo>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace harpia {

namespace {

// The whole UI, one notch down from the platform default (Segoe UI 9pt on
// Windows), and tooltips a further notch below that.
constexpr double kBaseScale = 0.90;
constexpr double kTooltipScale = 0.86;

// Below this the text stops being smaller and starts being unreadable, and on
// a low-DPI display it turns to mush. Nothing derived here goes under it.
constexpr int kMinPx = 9;

QFont scaled(const QFont &f, double k)
{
	QFont out = f;
	// A font carries EITHER a point size or a pixel size; the other reads -1,
	// and setting the wrong one silently does nothing.
	if (f.pointSizeF() > 0.0)
		out.setPointSizeF(std::max(6.5, f.pointSizeF() * k));
	else if (f.pixelSize() > 0)
		out.setPixelSize(std::max(kMinPx, int(std::lround(f.pixelSize() * k))));
	return out;
}

} // namespace

void applyTextScale(QApplication &app)
{
	// Always scale from the font the platform gave us, never from whatever is
	// installed now — otherwise a second call (a settings screen re-applying,
	// say) would shrink the shrunken font and the UI would step down each time.
	static const QFont base = app.font();
	app.setFont(scaled(base, kBaseScale));
	// Scale the tooltip from the ORIGINAL base, not the shrunken one, so the two
	// scales stay independent — otherwise tooltips compound and vanish.
	QToolTip::setFont(scaled(base, kTooltipScale));
}

int uiTextPx(double scale)
{
	// QFontInfo, not QFont: the app font is usually specified in points, and a
	// stylesheet needs the pixel size the platform actually resolved it to.
	const int base = QFontInfo(QApplication::font()).pixelSize();
	return std::max(kMinPx, int(std::lround(base * scale)));
}

} // namespace harpia
