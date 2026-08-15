// The colour convention: a swatch you click, previewing while you pick.
//
// Two things here are worth a regression guard, and neither is the dialog:
//
//  - What a Cancel lands on. A picker that previews live has already painted
//    the user's hovering all over the project; if Cancel keeps the last thing
//    hovered rather than the colour they started with, the original is simply
//    gone. That rule is settledColor(), split out of pickColorLive precisely so
//    it can be checked without driving a modal dialog.
//  - That the swatch actually carries its colour -- both to the eye (the
//    stylesheet) and to the code that reads it back when the field is clicked
//    (the "harpiaColor" property). A swatch that looks right but hands back a
//    default is a colour field that silently resets what it is pointed at.
#include "ui/ColorField.hpp"

#include <QApplication>
#include <QPushButton>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	const QColor original(0x20, 0x40, 0x60);
	const QColor picked(0xF0, 0x10, 0x10);

	std::printf("\n-- where a live pick lands --\n");
	ok(settledColor(true, picked, original) == picked, "accept keeps the chosen colour");
	ok(settledColor(false, picked, original) == original,
	   "and Cancel puts back the one that was there, not the last one hovered");
	ok(settledColor(true, QColor(), original) == original,
	   "an accept with no valid colour is not a colour: the original stands");

	std::printf("\n-- the swatch carries its colour --\n");
	{
		QPushButton b;
		paintColorSwatch(&b, picked);
		ok(b.property("harpiaColor").value<QColor>() == picked,
		   "the click handler reads back exactly what was painted");
		ok(b.styleSheet().contains(picked.name(QColor::HexRgb)),
		   "and the eye sees it too");
		ok(b.text() == picked.name(QColor::HexRgb).toUpper(), "labelled with its hex");

		// Alpha is a real part of a colour here (a shader colour parameter
		// carries it), and the property must not quietly drop it.
		const QColor ghost(0x10, 0x20, 0x30, 0x80);
		paintColorSwatch(&b, ghost);
		ok(b.property("harpiaColor").value<QColor>().alpha() == 0x80, "alpha survives");

		// The label sits ON the colour, so it has to flip.
		paintColorSwatch(&b, QColor(Qt::white));
		const bool darkOnLight = b.styleSheet().contains(QStringLiteral("#101214"));
		paintColorSwatch(&b, QColor(Qt::black));
		const bool lightOnDark = b.styleSheet().contains(QStringLiteral("#f0f0f0"));
		ok(darkOnLight && lightOnDark, "and stays readable on white and on black");

		paintColorSwatch(&b, picked, /*showHex=*/false);
		ok(b.text().isEmpty() && b.styleSheet().contains(picked.name(QColor::HexRgb)),
		   "a narrow row drops the hex but keeps the colour");
	}

	std::printf("\n-- and a selection that does not agree --\n");
	{
		QPushButton b;
		paintColorSwatch(&b, picked);
		paintMixedSwatch(&b);
		ok(b.text() == QStringLiteral("—"), "shows a dash");
		ok(!b.property("harpiaColor").value<QColor>().isValid(),
		   "and hands back no colour, so an untouched mixed field cannot write "
		   "one member's colour onto all of them");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
