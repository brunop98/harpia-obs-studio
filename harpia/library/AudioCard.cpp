#include "AudioCard.hpp"

#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QRect>

namespace harpia {

namespace {
// Enough bars to read as a waveform at 160px wide and still be distinct at
// card size; below about 20 it reads as a bar chart.
constexpr int kBars = 34;
} // namespace

void paintAudioCard(QPainter &p, const QRect &rect, const QString &path)
{
	if (rect.isEmpty())
		return;

	p.save();
	p.setRenderHint(QPainter::Antialiasing, true);

	// A quiet panel rather than a bright one: this sits in a grid of video
	// thumbnails and should read as "no picture", not as a picture.
	QLinearGradient bg(rect.topLeft(), rect.bottomLeft());
	bg.setColorAt(0.0, QColor(0x24, 0x28, 0x33));
	bg.setColorAt(1.0, QColor(0x18, 0x1b, 0x22));
	p.fillRect(rect, bg);

	const QVector<float> bars = audioCardBars(path, kBars);
	if (bars.isEmpty()) {
		p.restore();
		return;
	}

	const double slot = double(rect.width()) / bars.size();
	const double barW = std::max(1.0, slot * 0.55);
	const double midY = rect.center().y() + 0.5;
	const double maxH = rect.height() * 0.62;

	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x6c, 0x9a, 0xf5));
	for (int i = 0; i < bars.size(); ++i) {
		const double h = std::max(2.0, bars[i] * maxH);
		const double x = rect.left() + slot * i + (slot - barW) / 2.0;
		// Mirrored about the middle, which is how every audio editor draws a
		// waveform -- including this app's own.
		p.drawRoundedRect(QRectF(x, midY - h / 2.0, barW, h), barW / 2.0, barW / 2.0);
	}

	p.restore();
}

QImage audioCardImage(const QString &path, QSize size)
{
	if (size.isEmpty())
		return QImage();
	QImage img(size, QImage::Format_RGBA8888);
	img.fill(Qt::transparent);
	QPainter p(&img);
	paintAudioCard(p, QRect(QPoint(0, 0), size), path);
	p.end();
	return img;
}

} // namespace harpia
