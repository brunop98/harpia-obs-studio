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

	std::printf("\n-- choosing a source and moment for one option --\n");
	{
		TlClip c = slot();
		c.speed = 2.0;
		c.fadeInMs = 80;
		c.fadeOutMs = 120;
		c.keys.append(TlKeyframe{});
		ComponentInstance component;
		component.typeId = QStringLiteral("test.effect");
		component.instanceId = QStringLiteral("keep-effect");
		c.components.append(component);
		c.peaks = {0.25f, 0.75f};
		clip_variations::add(c, 2, total(2));
		// The shown clip has been slipped since its list entry was cached.
		c.srcStartMs = 6500;
		c.srcEndMs = 10500;
		TlClip expected = c;
		expected.clipOptions[0] = {1, 6500};
		expected.clipOptions[1] = {1, 17000};
		QString why = QStringLiteral("old error");
		ok(clip_variations::setOption(c, 1, 1, 17000, total(1), &why) && why.isEmpty(),
		   "a hidden option can choose another source and start time");
		ok(c == expected, "only that option changes; the shown slip is synchronized and every shared field stays intact");
		ok(c.clipShown == 0 && c.sourceId == 1 && c.srcStartMs == 6500 && c.outDurationMs() == 2000,
		   "editing a hidden option preserves current preview, timeline time and speed-adjusted duration");
		ok(clip_variations::setOption(c, 1, 2, 50000, total(2)) && c.clipOptions[1].srcStartMs == 2000,
		   "a requested moment near EOF clamps to the last complete shared window");
		ok(clip_variations::setOption(c, 1, 2, -1000, total(2)) && c.clipOptions[1].srcStartMs == 0,
		   "negative start times clamp to the source beginning");
		const TlClipOption other = c.clipOptions[1];
		ok(clip_variations::setOption(c, 0, 2, 1500, total(2)) &&
		   c.sourceId == 2 && c.srcStartMs == 1500 && c.srcEndMs == 5500 && c.clipShown == 0,
		   "editing the shown option updates its live source window without selecting another option");
		ok(c.clipOptions[0] == TlClipOption{2, 1500} && c.clipOptions[1] == other && c.peaks.isEmpty(),
		   "the shown cache matches its new source, clears the old waveform, and preserves the other option");
		c.peaks = {0.1f, 0.3f};
		ok(clip_variations::setOption(c, 0, 2, 1800, total(2)) && c.peaks == QVector<float>({0.1f, 0.3f}),
		   "slipping within one source preserves its whole-source waveform");
		const auto room = clip_variations::roomOfOthers(c, total);
		ok(room.headMs == 0 && room.tailMs == 2000,
		   "trim limits use a hidden option's newly chosen source window");
		TlClip shortSource = c;
		ok(!clip_variations::setOption(c, 1, 3, 0, total(3), &why) && c == shortSource && !why.isEmpty(),
		   "a source shorter than the shared slot is refused without changing any state");
		for (const auto &bad : QVector<QPair<int, qint64>>{{-1, 6000}, {2, 0}, {2, -1}}) {
			const TlClip before = c;
			ok(!clip_variations::setOption(c, 1, bad.first, 0, bad.second, &why) && c == before && !why.isEmpty(),
			   "an invalid source or unknown duration cannot change an option");
		}
		for (int badIndex : {-1, 2}) {
			const TlClip before = c;
			ok(!clip_variations::setOption(c, badIndex, 1, 0, total(1)) && c == before,
			   "an invalid option index leaves the clip untouched");
		}
		TlClip ordinary = slot(), before = ordinary;
		ok(!clip_variations::setOption(ordinary, 0, 1, 0, total(1)) && ordinary == before,
		   "editing an option never silently turns an ordinary clip into a random clip");
		ok(clip_variations::setOption(c, 1, 0, 0, 4000) && c.clipOptions[1] == TlClipOption{0, 0},
		   "source zero is valid and a source exactly the slot length has one usable window");
		ok(clip_variations::setOption(c, 1, 1, std::numeric_limits<qint64>::max(),
					     std::numeric_limits<qint64>::max()) &&
		   c.clipOptions[1].srcStartMs == std::numeric_limits<qint64>::max() - c.srcLenMs(),
		   "a maximum timestamp clamps safely without source-end overflow");
		TlClip invalid = c;
		invalid.srcStartMs = std::numeric_limits<qint64>::min();
		invalid.srcEndMs = std::numeric_limits<qint64>::max();
		before = invalid;
		ok(!clip_variations::setOption(invalid, 0, 1, 0, total(1)) && invalid == before,
		   "malformed source bounds are refused before duration arithmetic");

		clip_variations::setOption(c, 1, 1, 12345, total(1));
		TimelineModel timeline;
		TlTrack track;
		track.clips = {c};
		timeline.tracks = {track};
		const auto options = text_variations::slotsOf(timeline);
		const TimelineModel exported = text_variations::applyCombination(timeline, options, {1});
		const TlClip &result = exported.tracks[0].clips[0];
		ok(result.sourceId == 1 && result.srcStartMs == 12345 && result.srcEndMs == 16345 &&
		   result.outStartMs == c.outStartMs && result.outDurationMs() == c.outDurationMs() &&
		   result.components == c.components && result.keys == c.keys,
		   "export variations use the chosen moment with identical slot time, speed, look and animation");
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
