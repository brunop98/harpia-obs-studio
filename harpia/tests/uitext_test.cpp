// The app-wide text scale.
//
// Three things can go wrong and none of them are visible in a screenshot of one
// widget: the scale silently does nothing (setting a point size on a
// pixel-sized font, or vice versa), it compounds on a second call until the UI
// is unreadable, or the role sizes stop being ordered so a caption ends up
// larger than the heading it sits under.
#include "ui/UiText.hpp"

#include <QApplication>
#include <QFont>
#include <QFontInfo>
#include <QLabel>
#include <QToolTip>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// Compare fonts by the size the platform actually resolved them to: one may be
// specified in points and the other in pixels, and the raw fields would not
// line up even when the rendered text does.
static int px(const QFont &f)
{
	return QFontInfo(f).pixelSize();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	const QFont before = app.font();
	const int beforePx = px(before);
	const int tipBefore = px(QToolTip::font());
	std::printf("\n-- it actually shrinks something --\n");
	std::printf("     platform base: %dpx\n", beforePx);
	ok(beforePx > 0, "the platform gave us a resolvable font");

	applyTextScale(app);
	const int afterPx = px(app.font());
	const int tipPx = px(QToolTip::font());
	std::printf("     after scaling: %dpx, tooltips %dpx (was %dpx)\n", afterPx, tipPx, tipBefore);

	// The whole point. A scale that sets the field the font is not using leaves
	// the size untouched, and everything downstream still "passes".
	ok(afterPx < beforePx, "the base font is smaller than the platform default");
	ok(tipPx < afterPx, "and tooltips are smaller again than body text");

	// A QFont carries either a point size or a pixel size; the untouched one
	// reads -1. Scaling must not have swapped which is in use.
	ok((before.pointSizeF() > 0) == (app.font().pointSizeF() > 0),
	   "it stayed in the same unit the platform chose");

	std::printf("\n-- calling it twice does not shrink twice --\n");
	applyTextScale(app);
	std::printf("     after a second call: %dpx\n", px(app.font()));
	ok(px(app.font()) == afterPx, "the second call is a no-op, not another notch down");

	std::printf("\n-- the roles stay in order --\n");
	std::printf("     caption %d  base %d  readout %d  heading %d  timecode %d\n", uiCaptionPx(),
		    uiTextPx(1.0), uiReadoutPx(), uiHeadingPx(), uiTimecodePx());
	ok(uiCaptionPx() < uiTextPx(1.0), "a caption is below body text");
	ok(uiTextPx(1.0) <= uiReadoutPx(), "a readout is at least body text");
	ok(uiReadoutPx() < uiHeadingPx(), "a heading is above a readout");
	ok(uiHeadingPx() < uiTimecodePx(), "and the timecode is the largest thing on screen");

	std::printf("\n-- nothing shrinks into illegibility --\n");
	// Asked for something absurd, the floor holds: a stylesheet with a 1px font
	// renders as an unreadable smear rather than failing loudly.
	std::printf("     uiTextPx(0.01) = %d\n", uiTextPx(0.01));
	ok(uiTextPx(0.01) >= 9, "a tiny scale clamps to a legible floor");

	std::printf("\n-- widgets built after the call inherit it --\n");
	{
		QLabel l(QStringLiteral("x"));
		ok(px(l.font()) == afterPx, "a plain label picks up the scaled base font");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
