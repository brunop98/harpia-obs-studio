// Random clips: one slot, several clips, one version of the video each.
//
//   * The shown clip and the list stay in step: showing another keeps the
//     slot's place and length, and remembers where the last one had been
//     slipped to.
//   * A file shorter than the slot is refused, and says why.
//   * The last clip cannot be removed; removing the shown one shows another.
//   * A trim of the slot's head trims every clip the same way, and the slot
//     can only grow as far as its shortest clip allows.
//   * Export: Random clips count in with Random texts (every combination),
//     each version plays its own clip, named after the file and its moment.
#include "editor/timeline/ClipVariations.hpp"
#include "editor/timeline/TextVariations.hpp"

#include <QCoreApplication>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static TlClip slot()
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 1; // beach.mp4, 30 s
	c.srcStartMs = 5000;
	c.srcEndMs = 9000; // a 4 s slot
	c.outStartMs = 2000;
	c.posX = 0.3;
	c.scale = 1.4;
	return c;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const auto total = [](int id) -> qint64 { return id == 1 ? 30000 : id == 2 ? 6000 : id == 3 ? 3000 : 0; };

	std::printf("\n-- making one, adding to it --\n");
	{
		TlClip c = slot();
		ok(!c.isRandomClip(), "an ordinary clip is not a Random clip");
		clip_variations::makeRandom(c);
		ok(c.isRandomClip() && c.clipOptions.size() == 1 && c.clipOptions[0].sourceId == 1 &&
			   c.clipOptions[0].srcStartMs == 5000,
		   "made random, it is its own first clip");
		QString why;
		ok(clip_variations::add(c, 3, total(3), &why) == -1 && why.contains(QStringLiteral("3.0 s long")),
		   "a 3 s file cannot fill a 4 s slot, and it says so");
		ok(clip_variations::add(c, 2, total(2)) == 1 && c.clipOptions.size() == 2, "a 6 s file can");
		ok(c.clipOptions[1].srcStartMs == 0, "starting at its beginning");
	}

	std::printf("\n-- showing one --\n");
	{
		TlClip c = slot();
		clip_variations::add(c, 2, total(2));
		c.srcStartMs = 6000; // slipped while clip 1 was shown
		c.srcEndMs = 10000;
		ok(clip_variations::show(c, 1), "show clip 2");
		ok(c.sourceId == 2 && c.srcStartMs == 0 && c.srcLenMs() == 4000, "it plays, for the slot's length");
		ok(c.outStartMs == 2000 && c.posX == 0.3 && c.scale == 1.4, "in the same place, framed the same");
		ok(c.clipOptions[0].srcStartMs == 6000, "clip 1 remembers where it had been slipped to");
		c.srcStartMs = 1500; // slip clip 2
		c.srcEndMs = 5500;
		ok(c.clipOptionAt(1).srcStartMs == 1500, "the shown clip reads from the clip itself");
		clip_variations::show(c, 0);
		ok(c.sourceId == 1 && c.srcStartMs == 6000 && c.clipOptions[1].srcStartMs == 1500,
		   "back to clip 1, and clip 2 kept its slip");
	}

	std::printf("\n-- removing one --\n");
	{
		TlClip c = slot();
		clip_variations::add(c, 2, total(2));
		clip_variations::add(c, 1, total(1));
		clip_variations::show(c, 1);
		ok(clip_variations::remove(c, 1) && c.clipOptions.size() == 2, "the shown clip can go");
		ok(c.clipShown == 1 && c.sourceId == 1 && c.srcStartMs == 0, "and the next one shows in its place");
		ok(clip_variations::remove(c, 0) && c.clipShown == 0 && c.clipOptions.size() == 1,
		   "one before it: the shown one keeps showing");
		ok(!clip_variations::remove(c, 0), "the last clip stays");
		clip_variations::makeOrdinary(c);
		ok(!c.isRandomClip() && c.sourceId == 1, "made ordinary: it keeps the one it shows");
	}

	std::printf("\n-- trims --\n");
	{
		TlClip c = slot();
		clip_variations::add(c, 2, total(2));
		c.clipOptions[1].srcStartMs = 1000;
		clip_variations::shiftOthers(c, 500);
		ok(c.clipOptions[1].srcStartMs == 1500 && c.clipOptions[0].srcStartMs == 5000,
		   "trimming the head trims the others the same way; the shown one moves with the clip");
		clip_variations::shiftOthers(c, -4000);
		ok(c.clipOptions[1].srcStartMs == 0, "never before a file's first frame");
		c.clipOptions[1].srcStartMs = 1000;
		const clip_variations::Room r = clip_variations::roomOfOthers(c, total);
		ok(r.headMs == 1000, "it can grow back 1 s at the head: clip 2 has 1 s before it");
		ok(r.tailMs == 1000, "and 1 s at the tail: clip 2 is 6 s, from 1 s, for 4 s");
	}

	std::printf("\n-- versions --\n");
	{
		TimelineModel m;
		TlTrack v;
		v.kind = TlTrack::Kind::Video;
		TlClip c = slot();
		clip_variations::add(c, 2, total(2));
		c.clipOptions[1].srcStartMs = 1500;
		v.clips = {c};
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip txt;
		txt.type = TlClip::Type::Text;
		txt.srcEndMs = 4000;
		txt.text.text = QStringLiteral("cats");
		txt.text.random = true;
		txt.text.variations = {QStringLiteral("dogs"), QStringLiteral("birds")};
		t.clips = {txt};
		m.tracks = {t, v};
		const auto names = [](int id) {
			return id == 1 ? QStringLiteral("beach") : id == 2 ? QStringLiteral("forest") : QString();
		};
		const QVector<text_variations::Slot> vs = text_variations::slotsOf(m, names);
		ok(vs.size() == 2 && !vs[0].isClip() && vs[1].isClip(), "a Random text and a Random clip, top to bottom");
		ok(text_variations::combinationCount(vs) == 6, "3 texts x 2 clips = 6 videos");
		ok(vs[1].options == QStringList({QStringLiteral("beach @ 5.0s"), QStringLiteral("forest @ 1.5s")}),
		   "the clips are named by file and moment");
		const QVector<int> pick = text_variations::combinationAt(vs, 3); // dogs, forest
		const TimelineModel out = text_variations::applyCombination(m, vs, pick);
		const TlClip &oc = out.tracks[1].clips[0];
		ok(out.tracks[0].clips[0].text.text == QStringLiteral("dogs"), "version 4: the second text");
		ok(oc.sourceId == 2 && oc.srcStartMs == 1500 && oc.srcLenMs() == 4000 && oc.outStartMs == 2000,
		   "and the second clip, from its moment, for the slot's length, in the slot's place");
		ok(oc.scale == 1.4 && oc.posX == 0.3, "framed as the slot is");
		ok(text_variations::fileName(QStringLiteral("Promo"), 4, 6, text_variations::textsAt(vs, pick)) ==
			   QStringLiteral("Promo_04_dogs_forest-@-1.5s"),
		   "its file is named after both");
		const TimelineModel first = text_variations::applyCombination(m, vs, text_variations::combinationAt(vs, 0));
		ok(first.tracks[1].clips[0].sourceId == 1 && first.tracks[1].clips[0].srcStartMs == 5000,
		   "version 1 plays the first clip");
		TimelineModel one = m;
		one.tracks[1].clips[0].clipOptions.resize(1);
		ok(text_variations::slotsOf(one).size() == 1, "a Random clip with one clip does not vary");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
