// Social style presets remain complete editable styles, work with local fonts,
// and cannot replace a caption's words or its Random text alternatives.
#include "editor/timeline/TextStylePresets.hpp"
#include "editor/timeline/TextStyleJson.hpp"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QSet>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	const auto &presets = text_style_presets::builtins();
	check(presets.size() == 6, "the six built-in looks cover Instagram, TikTok and YouTube");
	check(&presets == &text_style_presets::builtins(), "the catalog and lookup pointers remain stable");
	QSet<QString> ids, names;
	const QStringList installed = QFontDatabase::families();
	for (const auto &preset : presets) {
		ids.insert(preset.id);
		names.insert(preset.name);
		check(text_style_presets::find(preset.id) == &preset, "a built-in is found by its stable ID");
		check(!preset.description.isEmpty() && installed.contains(preset.style.fontFamily, Qt::CaseInsensitive),
		      "the look has a description and a font installed on this system");
		check(preset.style.fontPx >= 56 && preset.style.fontPx <= 72 &&
		      preset.style.align == 1 && preset.style.textCase == TlCaseAsTyped,
		      "social captions start centered, readable and with the author's original case");

		TlText caption;
		caption.text = QStringLiteral("<b>Keep</b> my Words!\nWith a second line");
		caption.variations = {QStringLiteral("A different version"), QStringLiteral("Another\nversion")};
		caption.random = true;
		const TlText original = caption;
		text_style_presets::applyStyle(preset.style, caption);
		check(caption.text == original.text && caption.variations == original.variations && caption.random,
		      "applying the look preserves rich text, line breaks and Random text content");
		check(textStyleToJson(caption) == textStyleToJson(preset.style), "all style fields are applied");

		const QJsonObject json = QJsonDocument::fromJson(QJsonDocument(textStyleToJson(caption)).toJson()).object();
		check(!json.contains(QStringLiteral("text")) && !json.contains(QStringLiteral("variations")) &&
		      !json.contains(QStringLiteral("random")), "saved style JSON contains no caption content");
		TlText restored = original;
		applyTextStyleFromJson(json, restored);
		check(restored == caption, "a styled caption survives the existing project/style JSON round trip");
	}
	check(ids.size() == presets.size() && names.size() == presets.size(), "built-in IDs and names are unique");
	check(!text_style_presets::find(QStringLiteral("missing")), "an unknown built-in does not select another look");

	const auto *instagram = text_style_presets::find(QStringLiteral("instagram-classic"));
	const auto *instagramBox = text_style_presets::find(QStringLiteral("instagram-classic-box"));
	const auto *tiktok = text_style_presets::find(QStringLiteral("tiktok-classic"));
	const auto *tiktokBox = text_style_presets::find(QStringLiteral("tiktok-classic-box"));
	const auto *youtube = text_style_presets::find(QStringLiteral("youtube-classic"));
	const auto *youtubeBold = text_style_presets::find(QStringLiteral("youtube-bold"));
	check(instagram && !instagram->style.boxEnabled && !instagram->style.bold &&
	      instagram->style.color == QColor(Qt::white) && instagram->style.outlineWidth > 0,
	      "Instagram Classic uses simple white lettering with a fine dark edge");
	check(instagramBox && instagramBox->style.boxEnabled && instagramBox->style.color == QColor(Qt::black) &&
	      instagramBox->style.boxColor == QColor(Qt::white) && instagramBox->style.boxOpacity == 1.0 &&
	      instagramBox->style.boxRadius > 0, "Instagram's rounded white sticker has readable black text");
	check(tiktok && tiktok->style.bold && !tiktok->style.boxEnabled &&
	      tiktok->style.outlineWidth > instagram->style.outlineWidth,
	      "TikTok Classic uses bolder text and a stronger outline");
	check(tiktokBox && tiktokBox->style.bold && tiktokBox->style.boxEnabled &&
	      tiktokBox->style.color == QColor(Qt::white) && tiktokBox->style.boxColor == QColor(Qt::black) &&
	      tiktokBox->style.boxOpacity == 1.0, "TikTok's boxed variant keeps strong contrast");
	check(youtube && youtube->style.boxEnabled && youtube->style.boxRadius == 0 &&
	      youtube->style.boxOpacity > 0.5 && youtube->style.boxOpacity < 1.0,
	      "YouTube Classic uses the familiar translucent caption background");
	check(youtubeBold && youtubeBold->style.bold && !youtubeBold->style.boxEnabled &&
	      youtubeBold->style.fontPx > youtube->style.fontPx && youtubeBold->style.outlineWidth >= 3.0,
	      "YouTube Bold provides large outlined social-video captions");

	check(text_style_presets::detail::chooseFont({QStringLiteral("Missing"), QStringLiteral("ROBOTO")},
	      {QStringLiteral("Arial"), QStringLiteral("Roboto")}, QStringLiteral("Fallback")) == QStringLiteral("Roboto"),
	      "font lookup skips absent families and matches installed names without case sensitivity");
	check(text_style_presets::detail::chooseFont({QStringLiteral("Missing")},
	      {QStringLiteral("Arial")}, QStringLiteral("Fallback")) == QStringLiteral("Fallback"),
	      "a missing platform family uses the supplied system fallback");

	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
