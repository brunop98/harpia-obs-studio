// Text variations: counting, ordering, substitution and file names.
//
//   * A clip's options are its own text then every non-blank alternative.
//   * Varying clips are found top track first, then by start time.
//   * The count is the product and saturates instead of overflowing.
//   * Version 1 is the project as edited; the last varying clip changes fastest.
//   * applyCombination changes only the text (and drops word timings that
//     belonged to the old text).
//   * File names: prefix, zero-padded number, shortened texts, never over the
//     limit, never cutting the number, nothing Windows refuses.
#include "editor/timeline/TextVariations.hpp"

#include <QCoreApplication>

#include <cstdio>

using namespace harpia;
using namespace harpia::text_variations;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static TlClip textClip(const QString &text, const QStringList &vars, qint64 at)
{
	TlClip c;
	c.type = TlClip::Type::Text;
	c.text.text = text;
	c.text.variations = vars;
	c.text.random = true;
	c.outStartMs = at;
	c.srcStartMs = 0;
	c.srcEndMs = 1000;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- options --\n");
	{
		TlText t;
		t.text = QStringLiteral("I like dogs");
		t.variations = {QStringLiteral("I like cats"), QStringLiteral(""), QStringLiteral("   "),
				QStringLiteral("I like vultures")};
		const QStringList o = optionsOf(t);
		ok(o.size() == 3 && o[0] == QStringLiteral("I like dogs") && o[2] == QStringLiteral("I like vultures"),
		   "own text first, blank lines dropped");
		TlText plain;
		plain.text = QStringLiteral("x");
		ok(optionsOf(plain).size() == 1, "no alternatives: one option");
	}

	std::printf("\n-- finding the varying clips --\n");
	TimelineModel m;
	{
		TlTrack top, bottom, video;
		top.kind = TlTrack::Kind::Video;
		top.clips = {textClip(QStringLiteral("B"), {QStringLiteral("B2")}, 5000),
			     textClip(QStringLiteral("plain"), {}, 0),
			     textClip(QStringLiteral("A"), {QStringLiteral("A2"), QStringLiteral("A3")}, 1000)};
		bottom.kind = TlTrack::Kind::Video;
		bottom.clips = {textClip(QStringLiteral("C"), {QStringLiteral("C2")}, 0)};
		TlClip v;
		v.type = TlClip::Type::Video;
		v.srcEndMs = 1000;
		video.clips = {v};
		m.tracks = {top, bottom, video};
		const QVector<Slot> vs = slotsOf(m);
		ok(vs.size() == 3, "three varying clips (the plain one and the video are not)");
		ok(vs.size() == 3 && vs[0].track == 0 && vs[0].clip == 2 && vs[1].track == 0 && vs[1].clip == 0 &&
			   vs[2].track == 1,
		   "top track first, then by start time");
		ok(combinationCount(vs) == 3 * 2 * 2, "3 x 2 x 2 = 12 videos");
		ok(combinationCount({}) == 0, "no varying clips: no batch");
		TimelineModel plainKind = m;
		plainKind.tracks[0].clips[0].text.random = false;
		ok(slotsOf(plainKind).size() == 2, "a plain Text clip never varies, whatever it carries");

		const QVector<int> first = combinationAt(vs, 0);
		ok(first == QVector<int>({0, 0, 0}), "version 1 is every clip on its own text");
		ok(combinationAt(vs, 1) == QVector<int>({0, 0, 1}), "the last clip changes fastest");
		ok(combinationAt(vs, 2) == QVector<int>({0, 1, 0}), "then the one before it");
		ok(combinationAt(vs, 4) == QVector<int>({1, 0, 0}), "the first clip changes slowest");
		ok(combinationAt(vs, 11) == QVector<int>({2, 1, 1}), "the last version: every last option");
		QStringList all;
		for (qint64 i = 0; i < combinationCount(vs); ++i)
			all << textsAt(vs, combinationAt(vs, i)).join(QLatin1Char('|'));
		all.removeDuplicates();
		ok(all.size() == 12, "every version is different");
	}

	std::printf("\n-- overflow --\n");
	{
		QVector<Slot> big;
		for (int i = 0; i < 80; ++i)
			big.push_back({0, i, {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}});
		ok(combinationCount(big) == std::numeric_limits<qint64>::max(), "3^80 saturates");
	}

	std::printf("\n-- one version's timeline --\n");
	{
		TimelineModel w = m;
		ClipWordTime word;
		word.text = QStringLiteral("B");
		w.tracks[0].clips[0].words = {word};
		w.tracks[0].clips[0].text.fontPx = 77;
		const QVector<Slot> vs = slotsOf(w);
		const TimelineModel v1 = applyCombination(w, vs, combinationAt(vs, 0));
		ok(v1 == w, "version 1 is the project unchanged");
		const TimelineModel v = applyCombination(w, vs, {2, 1, 0});
		ok(v.tracks[0].clips[2].text.text == QStringLiteral("A3"), "the first slot took its third option");
		ok(v.tracks[0].clips[0].text.text == QStringLiteral("B2"), "the second slot its second");
		ok(v.tracks[1].clips[0].text.text == QStringLiteral("C"), "the third kept its own text");
		ok(v.tracks[0].clips[0].words.isEmpty(), "word timings for the old text dropped");
		ok(v.tracks[0].clips[0].text.fontPx == 77 && v.tracks[0].clips[0].outStartMs == 5000,
		   "style and timing untouched");
		ok(v.tracks[0].clips[0].text.variations == w.tracks[0].clips[0].text.variations,
		   "the alternatives list itself untouched");
		ok(v.tracks[0].clips[1].text.text == QStringLiteral("plain"), "a clip without alternatives untouched");
		ok(textsAt(vs, {2, 1, 0}) == QStringList({QStringLiteral("A3"), QStringLiteral("B2"), QStringLiteral("C")}),
		   "textsAt lists them in slot order");
	}

	std::printf("\n-- file names --\n");
	{
		ok(slug(QStringLiteral("I like cats")) == QStringLiteral("I-like-cats"), "spaces become dashes");
		ok(slug(QStringLiteral("a/b\\c:d*e?f\"g<h>i|j")) == QStringLiteral("abcdefghij"),
		   "characters Windows refuses are dropped");
		ok(slug(QStringLiteral("two\nlines  here")) == QStringLiteral("two-lines-here"), "one line, one dash");
		ok(slug(QStringLiteral("...end.")) == QStringLiteral("end"), "no leading or trailing dots");
		const QString longOne = slug(QStringLiteral("the quick brown fox jumps over the lazy dog"));
		std::printf("     long text -> %s\n", qPrintable(longOne));
		ok(longOne.size() <= 24 && !longOne.endsWith(QLatin1Char('-')), "cut to 24");
		ok(longOne == QStringLiteral("the-quick-brown-fox"), "at a whole word");
		ok(slug(QStringLiteral("Tamanho ção 🐶")) .contains(QStringLiteral("ção")), "accents kept");

		ok(fileName(QStringLiteral("Promo"), 3, 4, {QStringLiteral("I like cats")}) ==
			   QStringLiteral("Promo_03_I-like-cats"),
		   "Promo_03_I-like-cats");
		ok(fileName(QStringLiteral("Promo"), 7, 120, {QStringLiteral("x")}) == QStringLiteral("Promo_007_x"),
		   "the number is as wide as the batch");
		ok(fileName(QStringLiteral("P"), 2, 4, {QStringLiteral("A b"), QStringLiteral("C d")}) ==
			   QStringLiteral("P_02_A-b_C-d"),
		   "several clips, in order");
		QStringList many;
		for (int i = 0; i < 12; ++i)
			many << QStringLiteral("some rather long caption number %1").arg(i);
		const QString n = fileName(QStringLiteral("Campaign"), 12, 99, many);
		std::printf("     many texts -> %s (%d)\n", qPrintable(n), int(n.size()));
		ok(n.size() <= 120 && n.startsWith(QStringLiteral("Campaign_12_")), "never over 120, number kept");
		ok(fileName(QStringLiteral("P"), 1, 2, {QStringLiteral("???")}) == QStringLiteral("P_01"),
		   "a text with nothing usable adds nothing");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
