// Social style presets remain complete editable styles, work with local fonts,
// and cannot replace a caption's words or its Random text alternatives.
#include "editor/timeline/TextStylePresets.hpp"
#include "editor/timeline/TextStyleJson.hpp"

#include <QGuiApplication>
#include <QJsonArray>
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
	      instagramBox->style.boxRadius > 0 && instagramBox->style.boxShape == TlTextBoxFitText,
	      "Instagram's rounded white sticker follows each text line");
	check(tiktok && tiktok->style.bold && !tiktok->style.boxEnabled &&
	      tiktok->style.outlineWidth > instagram->style.outlineWidth,
	      "TikTok Classic uses bolder text and a stronger outline");
	check(tiktokBox && tiktokBox->style.bold && tiktokBox->style.boxEnabled &&
	      tiktokBox->style.color == QColor(Qt::white) && tiktokBox->style.boxColor == QColor(Qt::black) &&
	      tiktokBox->style.boxOpacity == 1.0 && tiktokBox->style.boxShape == TlTextBoxFitText,
	      "TikTok's boxed variant follows each line while keeping strong contrast");
	check(youtube && youtube->style.boxEnabled && youtube->style.boxRadius == 0 &&
	      youtube->style.boxOpacity > 0.5 && youtube->style.boxOpacity < 1.0 &&
	      youtube->style.boxShape == TlTextBoxRectangle,
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

	TlText fitted;
	fitted.text = QStringLiteral("A longer <b>line</b>\nShort");
	fitted.variations = {QStringLiteral("Keep this alternative\nToo")};
	fitted.random = true;
	fitted.boxEnabled = true;
	fitted.boxShape = TlTextBoxFitText;
	fitted.boxOpacity = 0.42;
	const QJsonObject fittedJson = textStyleToJson(fitted);
	check(fittedJson.value(QStringLiteral("boxShape")).toInt(-1) == TlTextBoxFitText,
	      "saved styles explicitly store the fitted text background mode");
	TlText restored = fitted;
	restored.boxShape = TlTextBoxRectangle;
	applyTextStyleFromJson(fittedJson, restored);
	check(restored == fitted, "a fitted custom style round-trips with its caption content intact");
	TlText rectangular = fitted;
	rectangular.boxShape = TlTextBoxRectangle;
	check(!(rectangular == fitted), "a background-mode change is a real edit for Undo and project equality");

	QJsonObject legacy = fittedJson;
	legacy.remove(QStringLiteral("boxShape"));
	restored = fitted;
	applyTextStyleFromJson(legacy, restored);
	check(restored.boxShape == TlTextBoxRectangle && restored.boxEnabled &&
	      restored.text == fitted.text && restored.variations == fitted.variations && restored.random,
	      "an older style without a background mode keeps its original rectangle and caption content");
	legacy.insert(QStringLiteral("boxShape"), TlTextBoxRectangle);
	restored = fitted;
	applyTextStyleFromJson(legacy, restored);
	check(restored == rectangular, "an explicitly rectangular style never becomes fitted text");

	const QJsonValue invalidShapes[] = {QJsonValue(-1), QJsonValue(2), QJsonValue(999), QJsonValue(1.5),
		QJsonValue(QStringLiteral("1")), QJsonValue(true), QJsonValue(QJsonValue::Null),
		QJsonValue(QJsonArray{1}), QJsonValue(QJsonObject{{QStringLiteral("mode"), 1}})};
	for (const QJsonValue &invalid : invalidShapes) {
		QJsonObject json = fittedJson;
		json.insert(QStringLiteral("boxShape"), invalid);
		restored = fitted;
		applyTextStyleFromJson(json, restored);
		check(restored == rectangular,
		      "an unknown or malformed background mode safely falls back to the legacy rectangle");
	}

	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
