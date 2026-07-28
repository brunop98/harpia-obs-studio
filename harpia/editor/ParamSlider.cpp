#include "ParamSlider.hpp"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>

#include <algorithm>
#include <cmath>

namespace harpia {

namespace {

// 0..1000 gives a tenth of a percent of the range per step: finer than a
// screen pixel at any inspector width, so the slider is never the limit.
constexpr int kSteps = 1000;

// Clicking the groove of a stock QSlider pages towards the click. For a value
// control that is wrong — you point at a value because you want that value.
class JumpSlider : public QSlider {
public:
	explicit JumpSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {}

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton) {
			QSlider::mousePressEvent(e);
			return;
		}
		QStyleOptionSlider opt;
		initStyleOption(&opt);
		const QRect handle =
			style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
		// A press ON the handle starts a normal drag; anywhere else jumps first
		// and then drags from there, so one gesture can do both.
		if (!handle.contains(e->position().toPoint())) {
			const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
								     QStyle::SC_SliderGroove, this);
			const int span = std::max(1, groove.width() - handle.width());
			const int pos = int(e->position().x()) - groove.x() - handle.width() / 2;
			setValue(QStyle::sliderValueFromPosition(minimum(), maximum(),
								 std::clamp(pos, 0, span), span));
		}
		QSlider::mousePressEvent(e);
	}
};

} // namespace

ParamSlider::ParamSlider(double lo, double hi, int decimals, QWidget *parent)
	: QWidget(parent), lo_(lo), hi_(hi > lo ? hi : lo + 1.0)
{
	auto *h = new QHBoxLayout(this);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(6);

	slider_ = new JumpSlider(this);
	slider_->setRange(0, kSteps);
	slider_->setSingleStep(kSteps / 100);
	slider_->setPageStep(kSteps / 10);

	spin_ = new QDoubleSpinBox(this);
	spin_->setRange(lo_, hi_);
	spin_->setDecimals(decimals);
	spin_->setSingleStep((hi_ - lo_) / 40.0);
	// Without this the box emits on every digit typed, so "0.5" passes through
	// 0 and then 0.5 — and on a value driving a render, that is a wasted frame
	// and a visible flicker.
	spin_->setKeyboardTracking(false);
	spin_->setMinimumWidth(72);
	spin_->setButtonSymbols(QAbstractSpinBox::NoButtons); // the slider is the nudger

	h->addWidget(slider_, 1);
	h->addWidget(spin_);

	connect(slider_, &QSlider::valueChanged, this, [this](int v) {
		if (syncing_)
			return;
		syncing_ = true;
		const double d = fromSlider(v);
		spin_->setValue(d);
		syncing_ = false;
		emit valueChanged(d);
	});
	connect(spin_, &QDoubleSpinBox::valueChanged, this, [this](double d) {
		if (syncing_)
			return;
		syncing_ = true;
		slider_->setValue(toSlider(d));
		syncing_ = false;
		emit valueChanged(d);
	});
}

int ParamSlider::toSlider(double v) const
{
	const double t = (v - lo_) / (hi_ - lo_);
	return int(std::lround(std::clamp(t, 0.0, 1.0) * kSteps));
}

double ParamSlider::fromSlider(int v) const
{
	return lo_ + (double(v) / kSteps) * (hi_ - lo_);
}

double ParamSlider::value() const
{
	// The spin box is the truth: it holds the typed precision, where the slider
	// has only a thousand positions.
	return spin_->value();
}

void ParamSlider::setValue(double v)
{
	const bool was = syncing_;
	syncing_ = true;
	spin_->setValue(std::clamp(v, lo_, hi_));
	slider_->setValue(toSlider(v));
	syncing_ = was;
}

} // namespace harpia
