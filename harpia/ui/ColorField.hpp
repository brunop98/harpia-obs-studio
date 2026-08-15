#pragma once

// THE COLOUR CONVENTION FOR THIS PROJECT.
//
// A colour is chosen from a colour field -- a swatch you click -- and the thing
// it colours updates WHILE you choose it. Never three sliders, and never a
// picker that only answers on OK.
//
// Both halves matter, and for the same reason: a colour is judged by looking at
// it in place. Hue/Saturation/Brightness sliders make you solve backwards for a
// colour you can already picture, and Qt's static QColorDialog::getColor tells
// you nothing until you commit -- so you pick, press OK, look at the result,
// and go back in. On a caption's text colour, a spotlight dim or a track's
// stripe, that round trip is the whole job.
//
// Everything here is inline on purpose: the picker is a dozen lines and half a
// dozen widgets across the app want it, several of which are pulled into unit
// tests that link a hand-written file list. A header keeps the convention one
// #include away from anywhere rather than a link-line edit away.
//
// Callers that need the dialog to stay open while the rest of the UI is used
// (the component Inspector, where the swatch belongs to a property row) build a
// non-modal QColorDialog directly, wired to currentColorChanged the same way.
// The rule is live-updating, not this particular function.

#include <QColor>
#include <QColorDialog>
#include <QDialog>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <functional>

namespace harpia {

// Where a pick lands once the dialog closes: the chosen colour, or -- on Cancel
// -- the colour that was there before, undoing whatever the live preview
// painted in the meantime. Cancel means "as if I never opened it".
//
// Split out from pickColorLive so the rule can be tested without driving a
// modal dialog, which is exactly the case that is easy to get subtly wrong (a
// Cancel that keeps the last hovered colour is a colour you cannot get back).
inline QColor settledColor(bool accepted, const QColor &picked, const QColor &original)
{
	return (accepted && picked.isValid()) ? picked : original;
}

// Paint `b` as a colour field: filled with `c`, labelled with its hex in a
// foreground that stays readable whichever way the colour goes.
//
// `showHex` off leaves the button a plain block of colour, for narrow rows
// (property lists) where the label already names the field and the hex would
// not fit anyway.
inline void paintColorSwatch(QPushButton *b, const QColor &c, bool showHex = true)
{
	if (!b)
		return;
	const QColor use = c.isValid() ? c : QColor(Qt::white);
	b->setProperty("harpiaColor", use);
	b->setToolTip(use.alpha() == 255 ? use.name(QColor::HexRgb).toUpper()
					 : use.name(QColor::HexArgb).toUpper());
	b->setText(showHex ? use.name(QColor::HexRgb).toUpper() : QString());
	// Lightness, not value: a saturated blue is dark to the eye at a value
	// where a yellow is already blinding, and this label sits ON the colour.
	const QString fg = (use.lightness() > 140) ? QStringLiteral("#101214")
						   : QStringLiteral("#f0f0f0");
	b->setStyleSheet(QStringLiteral("QPushButton{background:%1; color:%2; "
					"border:1px solid #4a4f58; border-radius:4px; padding:4px 8px;}"
					"QPushButton:hover{border:1px solid #6c9af5;}")
				 .arg(use.name(QColor::HexRgb), fg));
}

// A colour field standing for several selected things that do not agree. Shows
// a dash rather than one arbitrary member's colour, which would read as "they
// are all this" and be wrong.
inline void paintMixedSwatch(QPushButton *b)
{
	if (!b)
		return;
	b->setProperty("harpiaColor", QColor());
	b->setText(QStringLiteral("—"));
	b->setToolTip(QStringLiteral("Different colours in this selection."));
	b->setStyleSheet(QStringLiteral(
		"QPushButton{border:1px solid #4a4f58; border-radius:4px; color:#8a8f98;}"));
}

// Run a colour picker that previews live.
//
// `apply` is called on every movement inside the dialog, and once more when it
// closes with the settled colour -- so the caller lands on exactly one final
// value and can do its saving, undo snapshot or repaint there rather than on
// every mouse move. Returns true if the user accepted.
inline bool pickColorLive(QWidget *parent, const QString &title, const QColor &initial,
			  const std::function<void(const QColor &)> &apply, bool allowAlpha = false)
{
	const QColor original = initial.isValid() ? initial : QColor(Qt::white);
	QColorDialog dlg(original, parent);
	dlg.setWindowTitle(title);
	dlg.setOption(QColorDialog::ShowAlphaChannel, allowAlpha);
	if (apply)
		QObject::connect(&dlg, &QColorDialog::currentColorChanged, &dlg,
				 [&apply](const QColor &c) {
					 if (c.isValid())
						 apply(c);
				 });
	const bool accepted = dlg.exec() == QDialog::Accepted;
	if (apply)
		apply(settledColor(accepted, dlg.selectedColor(), original));
	return accepted;
}

} // namespace harpia
