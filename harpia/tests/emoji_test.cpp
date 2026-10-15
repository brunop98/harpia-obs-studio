// Colour emoji in text clips (editor/timeline/Emoji): Google's Noto pictures
// drawn where the emoji are, instead of the one-colour outline a glyph becomes
// in a text path (a white skull for 💀).
//
//   * Keys: code points in hex, no U+FE0F, as the pack files them.
//   * Which characters are emoji: 😀, ⭐, sequences (skin tone, family, flags,
//     keycaps, U+FE0F) yes; "1", "©", "™", "↔" on their own no.
//   * The pictures decode (FFmpeg's WebP decoder), in colour, and scaled down.
//   * A caption with 😀 in it: yellow pixels where the emoji is, the words
//     still drawn, the emoji takes room in the line; plain and rich text both.
//   * Without the pack nothing changes: everything is text.
#include "editor/timeline/Emoji.hpp"
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool cond, const char *what)
{
	std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		++failures;
}

static QString u(const char *utf8)
{
	return QString::fromUtf8(utf8);
}

// The emoji pieces of `text`, keys joined by " ".
static QString emojiKeys(const QString &text)
{
	QStringList keys;
	for (const emoji::Piece &p : emoji::split(text))
		if (p.isEmoji())
			keys << p.key;
	return keys.join(QLatin1Char(' '));
}

// Pixels a yellow face could be (and white text, black outline, a dark
// background cannot).
static int yellowPixels(const QImage &img, int x0, int x1)
{
	int n = 0;
	for (int y = 0; y < img.height(); ++y)
		for (int x = std::max(0, x0); x < std::min(img.width(), x1); ++x) {
			const QColor c = img.pixelColor(x, y);
			if (c.red() > 200 && c.green() > 150 && c.blue() < 90)
				++n;
		}
	return n;
}

static int whitePixels(const QImage &img)
{
	int n = 0;
	for (int y = 0; y < img.height(); ++y)
		for (int x = 0; x < img.width(); ++x) {
			const QColor c = img.pixelColor(x, y);
			if (c.red() > 240 && c.green() > 240 && c.blue() > 240)
				++n;
		}
	return n;
}

static QImage render(const QString &text, QSize canvas)
{
	QImage img(canvas, QImage::Format_ARGB32_Premultiplied);
	img.fill(QColor(30, 30, 60));
	TlClip c;
	c.type = TlClip::Type::Text;
	c.text.text = text;
	c.text.fontPx = 120;
	QPainter p(&img);
	TimelineCompositor::drawClip(p, c, TlTransform{}, canvas, QImage());
	p.end();
	return img;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	if (argc < 2) {
		std::printf("usage: emoji_test <noto-emoji.pack>\n");
		return 2;
	}

	std::printf("\n-- keys --\n");
	ok(emoji::keyFor(u("💀")) == QLatin1String("1f480"), "💀 -> 1f480");
	ok(emoji::keyFor(u("❤️")) == QLatin1String("2764"), "❤️ -> 2764 (no fe0f)");
	ok(emoji::keyFor(u("1️⃣")) == QLatin1String("0031_20e3"), "keycap 1 -> 0031_20e3");
	ok(emoji::keyFor(u("👨‍👩‍👧")) == QLatin1String("1f468_200d_1f469_200d_1f467"), "family: joined by 200d");

	std::printf("\n-- without the pack --\n");
	emoji::setPackPath(QStringLiteral("/nonexistent/noto-emoji.pack"));
	// Also not next to the test, nor in $HARPIA_EMOJI_PACK.
	qunsetenv("HARPIA_EMOJI_PACK");
	ok(!emoji::available(), "no pack: not available");
	{
		const auto pieces = emoji::split(u("hi 💀 there"));
		ok(pieces.size() == 1 && !pieces[0].isEmoji(), "everything is text, as before");
	}

	std::printf("\n-- which characters are emoji --\n");
	emoji::setPackPath(QString::fromLocal8Bit(argv[1]));
	ok(emoji::count() > 4000, "the pack loads (over 4000 emoji)");
	std::printf("     %d emoji\n", emoji::count());
	ok(emojiKeys(u("I'm dead 💀")) == QLatin1String("1f480"), "💀 in a sentence");
	{
		const auto pieces = emoji::split(u("I'm dead 💀!"));
		ok(pieces.size() == 3 && pieces[0].text == u("I'm dead ") && pieces[1].isEmoji() &&
			   pieces[2].text == QLatin1String("!"),
		   "split as text, emoji, text -- in order, nothing lost");
	}
	ok(emojiKeys(u("⭐ ❤️ 👍🏽")) == QLatin1String("2b50 2764 1f44d_1f3fd"), "⭐, ❤️ and a skin tone");
	ok(emojiKeys(u("👨‍👩‍👧")) == QLatin1String("1f468_200d_1f469_200d_1f467"), "a family is ONE emoji");
	ok(emojiKeys(u("🇧🇷🇺🇸")) == QLatin1String("1f1e7_1f1f7 1f1fa_1f1f8"), "flags: two of them");
	ok(emojiKeys(u("🏴󠁧󠁢󠁳󠁣󠁴󠁿")) == QLatin1String("1f3f4_e0067_e0062_e0073_e0063_e0074_e007f"), "Scotland (tags)");
	ok(emojiKeys(u("1️⃣ ©️")) == QLatin1String("0031_20e3 00a9"), "keycap and ©️ asked for");
	ok(emojiKeys(u("1 © ™ ↔ ♥ abc")).isEmpty(), "1, ©, ™, ↔, ♥ on their own stay text");
	ok(emojiKeys(u("☺️ ☺")) == QLatin1String("263a"), "☺ only with U+FE0F");

	std::printf("\n-- pictures --\n");
	const QImage face = emoji::image(QStringLiteral("1f600"));
	ok(face.width() == 128 && face.height() == 128, "😀 decodes at 128 px");
	ok(yellowPixels(face, 0, 128) > 3000, "in colour (yellow)");
	ok(qAlpha(face.pixel(0, 0)) == 0, "with a transparent corner");
	const QImage flag = emoji::image(QStringLiteral("1f1e7_1f1f7"));
	ok(!flag.isNull() && qGreen(flag.pixel(10, 30)) > 120 && qRed(flag.pixel(10, 30)) < 80, "🇧🇷 is green");
	const QImage small = emoji::image(QStringLiteral("1f600"), 40);
	ok(small.width() == 40 && small.height() == 40, "scaled down to 40 px");
	ok(emoji::image(QStringLiteral("1f600"), 40).constBits() == small.constBits(), "and cached");
	ok(emoji::image(QStringLiteral("nope")).isNull(), "an unknown key: null");

	std::printf("\n-- a caption --\n");
	const QSize canvas(1920, 1080); // fontPx is against a 1080-tall canvas
	const QImage plain = render(u("so funny"), canvas);
	const QImage withFace = render(u("so funny 😀"), canvas);
	ok(yellowPixels(plain, 0, canvas.width()) == 0, "plain text: no yellow");
	ok(yellowPixels(withFace, 0, canvas.width()) > 2000, "with 😀: a yellow face, not a white shape");
	ok(whitePixels(withFace) > whitePixels(plain) * 8 / 10, "the words are still drawn");
	{
		TlText a, b;
		a.fontPx = b.fontPx = 120;
		a.text = u("so funny");
		b.text = u("so funny 😀");
		const int wa = TimelineCompositor::textNaturalSize(a, canvas).width();
		const int wb = TimelineCompositor::textNaturalSize(b, canvas).width();
		std::printf("     %d -> %d px\n", wa, wb);
		ok(wb > wa + 100, "the emoji takes room in the line (the box and outline fit it)");
	}
	{
		// The emoji sits on the line: inside the band of the text's height.
		int top = canvas.height(), bottom = 0;
		for (int y = 0; y < withFace.height(); ++y)
			for (int x = 0; x < withFace.width(); ++x) {
				const QColor c = withFace.pixelColor(x, y);
				if (c.red() > 200 && c.green() > 150 && c.blue() < 90) {
					top = std::min(top, y);
					bottom = std::max(bottom, y);
				}
			}
		std::printf("     face rows %d..%d\n", top, bottom);
		ok(top > 540 - 90 && bottom < 540 + 90 && bottom - top > 100, "about a line high, on the line");
	}
	if (qEnvironmentVariableIsSet("EMOJI_TEST_SAVE"))
		withFace.save(qEnvironmentVariable("EMOJI_TEST_SAVE"));
	const QImage rich = render(u("<color=#f00>red</color> 😀"), canvas);
	ok(yellowPixels(rich, 0, canvas.width()) > 2000, "rich text: the face too");
	const QImage only = render(u("💀"), canvas);
	{
		int lightGrey = 0;
		for (int y = 0; y < only.height(); ++y)
			for (int x = 0; x < only.width(); ++x) {
				const QColor c = only.pixelColor(x, y);
				if (c.red() > 170 && c.red() < 250 && std::abs(c.red() - c.blue()) < 25)
					++lightGrey;
			}
		ok(lightGrey > 1000, "💀 on its own: the shaded skull, not a flat white shape");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
