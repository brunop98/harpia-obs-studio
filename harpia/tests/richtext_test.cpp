// Rich text in captions, Unity style: <b>, <i>, <u>, <s>, <color=…>, <size=…>,
// <alpha=…>, <mark=…>, <br>, <noparse>.
//
//   * The parser: runs and styles, tags that nest per kind, colours the way
//     Unity writes them (#rrggbbaa, alpha last), unknown or malformed tags
//     shown as typed, stray closing tags dropped, <noparse>.
//   * Drawn through the real compositor: coloured words land in their colour,
//     a caption with only empty tags draws the same pixels as one without,
//     bold is wider, <size=200%> taller, <alpha=#00> invisible, <mark> paints
//     behind, and a long tagged caption still wraps inside the picture.
//   * The typewriter never stops inside a tag and does not spend a step on one.
//   * Labels and file names use the words without the tags.
#include "editor/component/TextTyping.hpp"
#include "editor/timeline/RichText.hpp"
#include "editor/timeline/TextVariations.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QApplication>
#include <QPainter>

#include <cstdio>

using namespace harpia;
using namespace harpia::rich_text;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static const QSize kCanvas(640, 360);

static TlClip caption(const QString &text, int px = 160)
{
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcEndMs = 1000;
	c.text.text = text;
	c.text.fontPx = px;
	c.text.outlineWidth = 0.0;
	c.text.color = Qt::white;
	return c;
}

static QImage render(const TlClip &c)
{
	QImage img(kCanvas, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::black);
	QPainter p(&img);
	TimelineCompositor::drawClip(p, c, c.transformAt(0), kCanvas, QImage());
	p.end();
	return img;
}

// Pixels near a colour, in a column range.
static int count(const QImage &img, QRgb want, int x0 = 0, int x1 = -1)
{
	if (x1 < 0)
		x1 = img.width();
	int n = 0;
	for (int y = 0; y < img.height(); ++y)
		for (int x = x0; x < x1; ++x) {
			const QRgb v = img.pixel(x, y);
			if (std::abs(qRed(v) - qRed(want)) < 40 && std::abs(qGreen(v) - qGreen(want)) < 40 &&
			    std::abs(qBlue(v) - qBlue(want)) < 40)
				++n;
		}
	return n;
}

static int differing(const QImage &a, const QImage &b)
{
	int n = 0;
	for (int y = 0; y < a.height(); ++y)
		for (int x = 0; x < a.width(); ++x)
			if (a.pixel(x, y) != b.pixel(x, y))
				++n;
	return n;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the parser --\n");
	{
		const QVector<Line> l = parse(QStringLiteral("I like <b>dogs</b> a lot"), 64);
		ok(l.size() == 1 && l[0].size() == 3, "three runs");
		ok(l.size() == 1 && l[0].size() == 3 && l[0][1].text == QStringLiteral("dogs") && l[0][1].style.bold &&
			   !l[0][0].style.bold && !l[0][2].style.bold,
		   "the bold one is bold, the others not");

		const QVector<Line> n = parse(QStringLiteral("a<b>b<i>c</b>d</i>e"), 64);
		QString shape;
		for (const Run &r : n[0])
			shape += r.text + QLatin1Char(r.style.bold ? 'B' : '-') + QLatin1Char(r.style.italic ? 'I' : '-') +
				 QLatin1Char(' ');
		std::printf("     %s\n", qPrintable(shape));
		ok(shape == QStringLiteral("a-- bB- cBI d-I e-- "), "each kind nests on its own, as in Unity");

		const QVector<Line> c = parse(QStringLiteral("<color=#ff000080>x</color><color=red>y</color><color=#0f0>z"), 64);
		ok(c[0].size() == 3 && c[0][0].style.color == QColor(255, 0, 0, 128), "#rrggbbaa: alpha last");
		ok(c[0].size() == 3 && c[0][1].style.color == QColor(255, 0, 0), "a colour name");
		ok(c[0].size() == 3 && c[0][2].style.color == QColor(0, 255, 0), "#rgb, left open to the end");
		ok(parseColor(QStringLiteral("\"#00F\"")) == QColor(0, 0, 255), "quotes allowed");
		ok(parseColor(QStringLiteral("YELLOW")) == QColor(Qt::yellow), "names ignore case");

		const QVector<Line> s = parse(QStringLiteral("<size=100>a</size><size=150%>b</size><size=+20>c"), 64);
		ok(s[0].size() == 3 && s[0][0].style.sizePx == 100 && s[0][1].style.sizePx == 96 &&
			   s[0][2].style.sizePx == 84,
		   "sizes: absolute, percent, relative");

		ok(plainText(QStringLiteral("<b>I</b> <color=#f00>like</color> <foo>dogs</foo> 3<4 and 5>2")) ==
			   QStringLiteral("I like <foo>dogs</foo> 3<4 and 5>2"),
		   "unknown tags and stray angle brackets are shown as typed");
		ok(plainText(QStringLiteral("<color=banana>x</color>")) == QStringLiteral("<color=banana>x"),
		   "a colour that means nothing is not a tag (its stray closer is dropped)");
		ok(plainText(QStringLiteral("x</b></color>y")) == QStringLiteral("xy"), "stray closing tags dropped");
		ok(plainText(QStringLiteral("one<br>two")) == QStringLiteral("one\ntwo"), "<br> breaks the line");
		ok(plainText(QStringLiteral("<noparse><b>raw</b></noparse>")) == QStringLiteral("<b>raw</b>"),
		   "<noparse> shows tags as typed");
		ok(plainText(QStringLiteral("<B>UPPER</B> <COLOR=RED>CASE</COLOR>")) == QStringLiteral("UPPER CASE"),
		   "tags survive the UPPER CASE style");
		ok(!hasTags(QStringLiteral("3 < 4 > 2")) && hasTags(QStringLiteral("x<i>y")), "hasTags");
	}

	std::printf("\n-- drawn --\n");
	{
		const QImage rg = render(caption(QStringLiteral("<color=#ff0000>RR</color> <color=#00ff00>GG</color>")));
		const int redL = count(rg, qRgb(255, 0, 0), 0, 320), redR = count(rg, qRgb(255, 0, 0), 320);
		const int grnL = count(rg, qRgb(0, 255, 0), 0, 320), grnR = count(rg, qRgb(0, 255, 0), 320);
		std::printf("     red %d|%d  green %d|%d\n", redL, redR, grnL, grnR);
		ok(redL > 500 && redR < redL / 10, "red words on the left, in red");
		ok(grnR > 500 && grnL < grnR / 10, "green words on the right, in green");
		ok(count(rg, qRgb(255, 255, 255)) < 50, "none of it in the caption's own white");

		const QImage plain = render(caption(QStringLiteral("Hello there")));
		const QImage tagged = render(caption(QStringLiteral("<b></b>Hello <i></i>there")));
		const int d = differing(plain, tagged);
		std::printf("     empty tags vs none: %d pixels differ\n", d);
		ok(d < 40, "empty tags draw what no tags draw (same layout)");

		const QSize w0 = TimelineCompositor::textNaturalSize(caption(QStringLiteral("WWWW")).text, kCanvas);
		const QSize wb = TimelineCompositor::textNaturalSize(caption(QStringLiteral("<b>WWWW</b>")).text, kCanvas);
		const QSize wBig = TimelineCompositor::textNaturalSize(caption(QStringLiteral("<size=200%>W</size>")).text,
								      kCanvas);
		const QSize w1 = TimelineCompositor::textNaturalSize(caption(QStringLiteral("W")).text, kCanvas);
		std::printf("     WWWW %dx%d, bold %dx%d; W %dx%d, 200%% %dx%d\n", w0.width(), w0.height(), wb.width(),
			    wb.height(), w1.width(), w1.height(), wBig.width(), wBig.height());
		ok(wb.width() > w0.width(), "bold is wider");
		ok(wBig.height() > w1.height() * 17 / 10 && wBig.width() > w1.width() * 17 / 10, "200% is twice the size");

		ok(count(render(caption(QStringLiteral("<alpha=#00>hidden"))), qRgb(255, 255, 255)) == 0,
		   "<alpha=#00> draws nothing");
		const QImage mk = render(caption(QStringLiteral("<mark=#0000ff>hi</mark>")));
		ok(count(mk, qRgb(0, 0, 255)) > 1000 && count(mk, qRgb(255, 255, 255)) > 200,
		   "<mark> paints behind, the words on top");

		const QImage u = render(caption(QStringLiteral("<u>____</u>"), 80));
		const QImage uNo = render(caption(QStringLiteral("<i></i>____"), 80));
		ok(count(u, qRgb(255, 255, 255)) > count(uNo, qRgb(255, 255, 255)), "underline adds a line");

		TlClip longOne = caption(QStringLiteral("<b>This</b> is a <color=yellow>long caption</color> that "
							"<i>cannot</i> fit on one line of the picture"),
					 90);
		const QSize wrapped = TimelineCompositor::textNaturalSize(longOne.text, kCanvas);
		std::printf("     long caption %dx%d\n", wrapped.width(), wrapped.height());
		ok(wrapped.width() <= kCanvas.width() && wrapped.height() > w1.height(), "a long tagged caption wraps");

		const QSize br = TimelineCompositor::textNaturalSize(caption(QStringLiteral("a<br>b")).text, kCanvas);
		const QSize nl = TimelineCompositor::textNaturalSize(caption(QStringLiteral("a\nb")).text, kCanvas);
		ok(br == nl, "<br> is a line break");
	}

	std::printf("\n-- the typewriter --\n");
	{
		const QString s = QStringLiteral("<b>ab</b>c");
		const QVector<int> st = revealStops(s, RevealUnit::Characters);
		ok(st == QVector<int>({4, 5, 10}), "one step per drawn character");
		bool neverInside = true;
		for (int i = 1; i <= 20; ++i) {
			const QString shown = revealedText(s, i / 20.0, RevealUnit::Characters);
			if (shown.endsWith(QLatin1Char('<')) || shown.endsWith(QLatin1String("</")) ||
			    shown.endsWith(QLatin1String("<b")) || shown.endsWith(QLatin1String("</b")))
				neverInside = false;
		}
		ok(neverInside, "never cut inside a tag");
		ok(revealStops(QStringLiteral("ab</b>"), RevealUnit::Characters) == QVector<int>({1, 6}),
		   "a closing tag at the end is not a step of its own");
		const QString w = QStringLiteral("<color=red>hi</color> there");
		ok(revealStops(w, RevealUnit::Words) == QVector<int>({13, int(w.size())}), "words skip the tags");
		ok(revealStops(QStringLiteral("abc"), RevealUnit::Characters) == QVector<int>({1, 2, 3}),
		   "plain text as before");
	}

	std::printf("\n-- labels and names --\n");
	{
		TlText t;
		t.text = QStringLiteral("<b>I like</b> <color=#f80>cats</color>");
		ok(tlPlainText(t) == QStringLiteral("I like cats"), "the label shows the words");
		ok(text_variations::slug(t.text) == QStringLiteral("I-like-cats"), "and so does the file name");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
