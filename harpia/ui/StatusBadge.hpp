#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QTimer;

namespace harpia {

// A compact, passive status indicator: a small (optionally pulsing) colored dot
// followed by a short label, on a faint tinted pill. Deliberately *not* styled
// like a button — it communicates state (Ready / Recording / Paused / Error)
// through color + icon, never invites a click.
class StatusBadge : public QWidget {
	Q_OBJECT
public:
	explicit StatusBadge(QWidget *parent = nullptr);

	// Set the label text, dot color, and whether the dot gently pulses (used for
	// live states like Ready/Recording). Pulsing stops for static states.
	void setStatus(const QString &text, const QColor &color, bool pulse);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QString text_;
	QColor color_{0x3f, 0xb9, 0x50};
	bool pulse_ = true;
	double phase_ = 0.0;
	QTimer *timer_ = nullptr;
};

} // namespace harpia
