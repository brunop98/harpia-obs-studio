#pragma once

// A number you can drag or type: a slider with a spin box beside it.
//
// Effect parameters were spin boxes alone. Every one of them has a declared
// range, which is exactly the case a slider is for — you want to sweep an
// amount and watch the picture, not guess a number and re-guess. But the spin
// box has to stay: a slider cannot express "exactly 0.5", and it cannot be read
// off at a glance either.
//
// Two things this gets right that a hand-rolled pair usually does not. Clicking
// the groove JUMPS to that spot rather than paging towards it, which is what
// everyone expects of a value slider and is not Qt's default. And the two
// halves cannot fight: an edit from either updates the other with signals
// blocked, so nothing echoes back and no drag is interrupted by its own effect.

#include <QWidget>

class QDoubleSpinBox;
class QSlider;

namespace harpia {

class ParamSlider : public QWidget {
	Q_OBJECT
public:
	ParamSlider(double lo, double hi, int decimals = 3, QWidget *parent = nullptr);

	double value() const;
	// Set without emitting — for pushing a model value into the control.
	void setValue(double v);

	// The selected clips disagree about this value. Unity's em dash: show that
	// there IS no single value rather than picking one of them and quietly
	// implying the others match. Cleared by setValue, and by the user typing —
	// at which point the value they typed becomes everyone's.
	void setMixed();
	bool isMixed() const { return mixed_; }

signals:
	void valueChanged(double v);

private:
	int toSlider(double v) const;
	double fromSlider(int v) const;

	QSlider *slider_ = nullptr;
	QDoubleSpinBox *spin_ = nullptr;
	double lo_ = 0.0, hi_ = 1.0;
	bool syncing_ = false;
	bool mixed_ = false;
};

} // namespace harpia
