#pragma once

// The small ⓘ that replaced the explanatory paragraphs.
//
// Every panel used to open with a few grey lines saying what it was for.
// Read once, they were in the way ever after, and they pushed the controls
// down. The text is still there, word for word -- as the tooltip of a small
// info mark, shown on hover -- and the screen holds only the controls.
//
// Status lines (what just happened, what is selected) are not explanations
// and stay visible; this is for text that never changes.

#include <QLabel>
#include <QString>

namespace harpia {

inline QLabel *infoHint(const QString &text, QWidget *parent = nullptr)
{
	auto *l = new QLabel(QStringLiteral("ⓘ"), parent); // ⓘ
	l->setToolTip(text);
	l->setToolTipDuration(-1);
	l->setCursor(Qt::WhatsThisCursor);
	l->setStyleSheet(QStringLiteral("color:#8a9099; font-size:13px; padding:0 2px;"));
	l->setFixedWidth(18);
	l->setAlignment(Qt::AlignCenter);
	return l;
}

} // namespace harpia
