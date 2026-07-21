#pragma once

#include <QWidget>

namespace harpia {

// A small horizontal audio level meter: a green→amber→red bar driven by an RMS
// level with a decaying peak-hold marker. Fed by AudioRecorder::level().
class LevelMeter : public QWidget {
	Q_OBJECT
public:
	explicit LevelMeter(QWidget *parent = nullptr);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

public slots:
	// Both 0..1 (linear). Updates the bar (rms) and the peak-hold marker.
	void setLevel(qreal rms, qreal peak);
	void reset(); // clear to silence (call when capture stops)

protected:
	void paintEvent(QPaintEvent *) override;

private:
	qreal rms_ = 0.0;
	qreal peakHold_ = 0.0;
};

} // namespace harpia
