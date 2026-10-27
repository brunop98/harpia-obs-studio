#pragma once

// Familiar social-video looks made from the same editable text properties the
// Inspector already exposes. The chosen installed font is copied into the clip,
// so the preview, saved project and export all describe the same style.

#include "TimelineModel.hpp"

#include <QFontDatabase>
#include <QFontInfo>
#include <QStringList>
#include <QVector>

namespace harpia {
namespace text_style_presets {

struct Preset {
	QString id;
	QString name;
	QString description;
	TlText style;
};

namespace detail {
inline QString chooseFont(const QStringList &preferred, const QStringList &installed,
			  const QString &fallback)
{
	for (const QString &candidate : preferred)
		for (const QString &family : installed)
			if (family.compare(candidate, Qt::CaseInsensitive) == 0)
				return family;
	return fallback;
}
} // namespace detail

// Call after the GUI application is constructed. Native platform families are
// used when present; common local sans-serif fonts keep every preset usable
// without downloading or redistributing a font.
inline const QVector<Preset> &builtins()
{
	static const QVector<Preset> presets = []() {
		const QStringList installed = QFontDatabase::families();
		const QString fallback = QFontInfo(QFontDatabase::systemFont(QFontDatabase::GeneralFont)).family();
		const QString instagramFont = detail::chooseFont(
			{QStringLiteral("Instagram Sans"), QStringLiteral("Avenir Next"),
			 QStringLiteral("Avenir"), QStringLiteral("Helvetica Neue"),
			 QStringLiteral("Arial"), QStringLiteral("Segoe UI"),
			 QStringLiteral("Noto Sans"), QStringLiteral("Liberation Sans"),
			 QStringLiteral("DejaVu Sans")}, installed, fallback);
		const QString tiktokFont = detail::chooseFont(
			{QStringLiteral("TikTok Sans"), QStringLiteral("TikTokSans"),
			 QStringLiteral("Proxima Nova"), QStringLiteral("Arial"),
			 QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans"),
			 QStringLiteral("Liberation Sans"), QStringLiteral("DejaVu Sans")}, installed, fallback);
		const QString youtubeFont = detail::chooseFont(
			{QStringLiteral("Roboto"), QStringLiteral("Noto Sans"),
			 QStringLiteral("Arial"), QStringLiteral("Segoe UI"),
			 QStringLiteral("Liberation Sans"), QStringLiteral("DejaVu Sans")}, installed, fallback);

		TlText instagram;
		instagram.fontFamily = instagramFont;
		instagram.fontPx = 64;
		instagram.outlineWidth = 1.5;

		TlText instagramBox = instagram;
		instagramBox.color = QColor(0, 0, 0);
		instagramBox.outlineWidth = 0;
		instagramBox.boxEnabled = true;
		instagramBox.boxShape = TlTextBoxFitText;
		instagramBox.boxColor = QColor(255, 255, 255);
		instagramBox.boxOpacity = 1.0;
		instagramBox.boxPadX = 22;
		instagramBox.boxPadY = 12;
		instagramBox.boxRadius = 12;

		TlText tiktok;
		tiktok.fontFamily = tiktokFont;
		tiktok.fontPx = 64;
		tiktok.bold = true;
		tiktok.outlineWidth = 3.0;

		TlText tiktokBox = tiktok;
		tiktokBox.outlineWidth = 0;
		tiktokBox.boxEnabled = true;
		tiktokBox.boxShape = TlTextBoxFitText;
		tiktokBox.boxColor = QColor(0, 0, 0);
		tiktokBox.boxOpacity = 1.0;
		tiktokBox.boxPadX = 20;
		tiktokBox.boxPadY = 10;
		tiktokBox.boxRadius = 8;

		TlText youtube;
		youtube.fontFamily = youtubeFont;
		youtube.fontPx = 56;
		youtube.outlineWidth = 1.0;
		youtube.boxEnabled = true;
		youtube.boxColor = QColor(0, 0, 0);
		youtube.boxOpacity = 0.75;
		youtube.boxPadX = 12;
		youtube.boxPadY = 8;
		youtube.boxRadius = 0;

		TlText youtubeBold = youtube;
		youtubeBold.fontPx = 72;
		youtubeBold.bold = true;
		youtubeBold.outlineWidth = 3.0;
		youtubeBold.boxEnabled = false;

		return QVector<Preset>{
			{QStringLiteral("instagram-classic"), QStringLiteral("Instagram Classic"),
			 QStringLiteral("Clean white text with a fine dark outline. Every setting stays editable."), instagram},
			{QStringLiteral("instagram-classic-box"), QStringLiteral("Instagram Classic Box"),
			 QStringLiteral("Black text on a white rounded background that follows each line."), instagramBox},
			{QStringLiteral("tiktok-classic"), QStringLiteral("TikTok Classic"),
			 QStringLiteral("Bold white text with a strong black outline. Every setting stays editable."), tiktok},
			{QStringLiteral("tiktok-classic-box"), QStringLiteral("TikTok Classic Box"),
			 QStringLiteral("Bold white text on a black rounded background that follows each line."), tiktokBox},
			{QStringLiteral("youtube-classic"), QStringLiteral("YouTube Classic"),
			 QStringLiteral("White captions on a translucent black background. Every setting stays editable."), youtube},
			{QStringLiteral("youtube-bold"), QStringLiteral("YouTube Bold"),
			 QStringLiteral("Large bold white text with a thick black outline. Every setting stays editable."), youtubeBold},
		};
	}();
	return presets;
}

inline const Preset *find(const QString &id)
{
	for (const Preset &preset : builtins())
		if (preset.id == id)
			return &preset;
	return nullptr;
}

// Both built-in and saved looks change only the style. In particular a Random
// text keeps its alternatives and kind when the appearance changes.
inline void applyStyle(const TlText &style, TlText &target)
{
	const QString words = target.text;
	const QStringList variations = target.variations;
	const bool random = target.random;
	target = style;
	target.text = words;
	target.variations = variations;
	target.random = random;
}

} // namespace text_style_presets
} // namespace harpia
