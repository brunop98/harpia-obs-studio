// Unchanged Inspector readings must not repeatedly repolish colour buttons.
// The helper still repairs text, tooltip, colour metadata and externally
// overridden styling, including mixed and translucent selections.
#include "ui/ColorField.hpp"

#include <QApplication>
#include <QEvent>
#include <QPushButton>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

class StyleChanges : public QObject {
public:
	int count = 0;

protected:
	bool eventFilter(QObject *, QEvent *event) override
	{
		if (event->type() == QEvent::StyleChange)
			++count;
		return false;
	}
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setStyle(QStringLiteral("Fusion"));
	QPushButton button;
	button.show();
	QApplication::processEvents();
	StyleChanges changes;
	button.installEventFilter(&changes);

	const QColor first(240, 150, 30);
	paintColorSwatch(&button, first);
	check(changes.count > 0 && button.property("harpiaColor").value<QColor>() == first,
	      "the first colour paints and stores the chosen value");
	check(button.text() == QStringLiteral("#F0961E") && button.toolTip() == button.text(),
	      "opaque colour labels and tooltips retain their existing hex spelling");
	const QString firstCss = button.styleSheet();
	changes.count = 0;
	for (int index = 0; index < 100; ++index)
		paintColorSwatch(&button, first);
	check(changes.count == 0 && button.styleSheet() == firstCss,
	      "identical playback readings do not repolish the colour button");

	paintColorSwatch(&button, first, false);
	check(button.text().isEmpty() && changes.count == 0,
	      "hiding the hex label still updates the text without repainting its style");
	QColor transparent = first;
	transparent.setAlpha(80);
	paintColorSwatch(&button, transparent, false);
	check(button.property("harpiaColor").value<QColor>() == transparent &&
	      button.toolTip() == transparent.name(QColor::HexArgb).toUpper(),
	      "alpha-only changes refresh the exact colour and ARGB tooltip");
	check(changes.count == 0, "an unchanged RGB appearance does not require a style change");

	const QColor second(20, 30, 180);
	paintColorSwatch(&button, second, false);
	check(changes.count > 0 && button.styleSheet() != firstCss &&
	      button.property("harpiaColor").value<QColor>() == second,
	      "a different colour replaces the style and stored value");
	const QString secondCss = button.styleSheet();
	button.setStyleSheet(QStringLiteral("QPushButton{background:#abcdef;}"));
	changes.count = 0;
	paintColorSwatch(&button, second, false);
	check(changes.count > 0 && button.styleSheet() == secondCss,
	      "an external style override is repaired even when the colour is unchanged");
	button.setProperty("harpiaColor", QColor(Qt::red));
	button.setText(QStringLiteral("stale"));
	button.setToolTip(QStringLiteral("stale"));
	changes.count = 0;
	paintColorSwatch(&button, second, false);
	check(button.property("harpiaColor").value<QColor>() == second && button.text().isEmpty() &&
	      button.toolTip() == second.name(QColor::HexRgb).toUpper() && changes.count == 0,
	      "metadata and text refresh independently of unchanged button styling");

	paintMixedSwatch(&button);
	check(changes.count > 0 && !button.property("harpiaColor").value<QColor>().isValid() &&
	      button.text() == QStringLiteral("—"),
	      "a mixed selection replaces the colour with a dash and no chosen value");
	const QString mixedCss = button.styleSheet();
	changes.count = 0;
	for (int index = 0; index < 100; ++index)
		paintMixedSwatch(&button);
	check(changes.count == 0 && button.styleSheet() == mixedCss,
	      "repeated mixed readings do not repolish the button either");
	button.setStyleSheet(firstCss);
	changes.count = 0;
	paintMixedSwatch(&button);
	check(changes.count > 0 && button.styleSheet() == mixedCss,
	      "mixed styling also repairs an external override");
	paintColorSwatch(&button, first);
	check(button.property("harpiaColor").value<QColor>() == first && button.styleSheet() == firstCss,
	      "returning from mixed to a real colour restores the original appearance");
	paintColorSwatch(&button, QColor());
	check(button.property("harpiaColor").value<QColor>() == QColor(Qt::white) &&
	      button.text() == QStringLiteral("#FFFFFF"),
	      "an invalid colour still uses the existing white fallback");

	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
