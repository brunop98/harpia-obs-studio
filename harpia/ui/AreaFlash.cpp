#include "ui/AreaFlash.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>

namespace harpia {

namespace {

class AreaFlashLayer : public QWidget {
public:
	AreaFlashLayer(QScreen *screen, const QRectF &local, const QString &caption)
		: QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
					   Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus),
		  local_(local), caption_(caption)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_DeleteOnClose);
		setObjectName(QStringLiteral("areaFlashLayer"));
		if (screen)
			setGeometry(screen->geometry());
	}

	QRectF area() const { return local_; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QColor accent(0xff, 0x8a, 0x3d);
		// Outside the line, so the line itself is never part of the picture.
		p.setPen(QPen(accent, 4));
		p.setBrush(Qt::NoBrush);
		p.drawRect(local_.adjusted(-2, -2, 2, 2));
		if (caption_.isEmpty())
			return;
		QFont f = font();
		f.setPixelSize(13);
		f.setBold(true);
		p.setFont(f);
		const QFontMetrics fm(f);
		const QRectF pill(local_.left(), local_.top() - fm.height() - 12, fm.horizontalAdvance(caption_) + 16,
				  fm.height() + 8);
		const QRectF where = pill.top() >= 0 ? pill : pill.translated(0, local_.height() + pill.height() + 16);
		p.setPen(Qt::NoPen);
		p.setBrush(accent);
		p.drawRoundedRect(where, 4, 4);
		p.setPen(Qt::black);
		p.drawText(where, Qt::AlignCenter, caption_);
	}

private:
	QRectF local_;
	QString caption_;
};

} // namespace

QRectF AreaFlash::toLocal(const QRect &areaPx, const QRect &screenPhysical, double dpr)
{
	const double d = dpr > 0 ? dpr : 1.0;
	return QRectF((areaPx.x() - screenPhysical.x()) / d, (areaPx.y() - screenPhysical.y()) / d,
		      areaPx.width() / d, areaPx.height() / d);
}

QVector<QWidget *> AreaFlash::show(const QRect &areaPx, const QVector<ScreenArea> &screens, int ms,
				   const QString &caption)
{
	struct Hit {
		QScreen *screen;
		QRect phys;
		double dpr;
	};
	QVector<Hit> hits;
	for (const ScreenArea &s : screens) {
		if (!s.screen)
			continue;
		const double dpr = s.screen->devicePixelRatio();
		const QRect geo = s.screen->geometry();
		const QRect phys = s.physical.isNull() ? QRect(geo.topLeft() * dpr, geo.size() * dpr) : s.physical;
		if (phys.intersects(areaPx))
			hits.push_back({s.screen, phys, dpr});
	}
	// The caption once: on the screen holding the area's top-left corner, or
	// the first one the area touches.
	int captionAt = hits.isEmpty() ? -1 : 0;
	for (int i = 0; i < hits.size(); ++i)
		if (hits[i].phys.contains(areaPx.topLeft()))
			captionAt = i;
	QVector<QWidget *> layers;
	for (int i = 0; i < hits.size(); ++i) {
		auto *layer = new AreaFlashLayer(hits[i].screen, toLocal(areaPx, hits[i].phys, hits[i].dpr),
						 i == captionAt ? caption : QString());
		layer->show();
		QTimer::singleShot(ms, layer, &QWidget::close);
		layers << layer;
	}
	return layers;
}

} // namespace harpia
