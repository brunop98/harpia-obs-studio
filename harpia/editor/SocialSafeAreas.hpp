#pragma once

// Approximate interface margins for vertical social videos. Platforms change
// their UI and vary it by device: these are composition guides, never a crop
// or part of the rendered/exported frame.

#include <QRectF>
#include <QString>
#include <QVector>

namespace harpia {

enum class SocialSafeArea { Off = 0, TikTok, InstagramReels, YouTubeShorts };

struct SocialSafeAreaLayout {
	QString label;
	QRectF content; // normalised to the output canvas, not the selected clip
	QVector<QRectF> interfaceAreas;
};

inline SocialSafeAreaLayout socialSafeAreaLayout(SocialSafeArea mode)
{
	SocialSafeAreaLayout layout;
	switch (mode) {
	case SocialSafeArea::TikTok:
		layout.label = QStringLiteral("TikTok");
		layout.content = QRectF(0.06, 0.10, 0.76, 0.66);
		break;
	case SocialSafeArea::InstagramReels:
		layout.label = QStringLiteral("Instagram Reels");
		layout.content = QRectF(0.07, 0.13, 0.76, 0.64);
		break;
	case SocialSafeArea::YouTubeShorts:
		layout.label = QStringLiteral("YouTube Shorts");
		layout.content = QRectF(0.06, 0.09, 0.76, 0.71);
		break;
	case SocialSafeArea::Off:
	default:
		return layout;
	}
	const QRectF &safe = layout.content;
	// Non-overlapping regions keep corners from being shaded twice.
	layout.interfaceAreas = {
		QRectF(0, 0, 1, safe.top()),
		QRectF(0, safe.bottom(), 1, 1 - safe.bottom()),
		QRectF(0, safe.top(), safe.left(), safe.height()),
		QRectF(safe.right(), safe.top(), 1 - safe.right(), safe.height()),
	};
	return layout;
}

inline QRectF mapSocialSafeRect(const QRectF &normalised, const QRectF &canvas)
{
	if (normalised.isEmpty() || canvas.isEmpty())
		return {};
	return QRectF(canvas.x() + normalised.x() * canvas.width(),
		      canvas.y() + normalised.y() * canvas.height(),
		      normalised.width() * canvas.width(), normalised.height() * canvas.height());
}

} // namespace harpia
