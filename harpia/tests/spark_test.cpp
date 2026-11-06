#include "editor/timeline/Spark.hpp"
#include "editor/timeline/SoundRules.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why) { std::printf("%s %s\n", pass ? "PASS" : "FAIL", why); failures += !pass; }
static TlClip clip(int id, qint64 start, int group = 0, int link = 0)
{
	TlClip c; c.sourceId = id; c.srcStartMs = 100; c.srcEndMs = 1100; c.outStartMs = start; c.group = group; c.link = link;
	return c;
}
// One-second clips back to back on one lane, the first `favorites` starred.
static TimelineModel footageOf(int favorites, int ordinary)
{
	TimelineModel m;
	m.tracks = {TlTrack{}};
	for (int i = 0; i < favorites + ordinary; ++i) {
		TlClip c = clip(i + 1, i * 1000);
		c.favorite = i < favorites;
		m.tracks[0].clips.append(c);
	}
	return m;
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	TimelineModel m; TlTrack t; t.kind = TlTrack::Kind::Video;
	t.clips = {clip(1, 1000), clip(2, 6000), clip(3, 9000)}; t.clips[2].favorite = true; m.tracks = {t};
	const auto saved = clipToJson(t.clips[2]);
	ok(clipFromJson(saved).favorite, "stars round trip through project JSON");
	ok(!clipFromJson(QJsonObject{}).favorite && clipFromJson(QJsonObject{}).group == 0, "old projects load unstarred and ungrouped");
	TlClip changed = t.clips[2]; changed.favorite = false; ok(!(changed == t.clips[2]), "clip equality includes star");
	changed = t.clips[2]; changed.group = 5; ok(!(changed == t.clips[2]), "clip equality includes group");
	ok(clipFromJson(clipToJson(changed)).group == 5, "group membership survives JSON round trip");
	auto plan = spark::buildPlan(m);
	ok(plan.available() && plan.blocks.size() == 3 && plan.favoriteCount == 1 && plan.uniqueOrders == 2,
	   "one favorite and two ordinary clips yield only two orders");
	const auto orders = spark::generateOrders(plan, 100, 42);
	ok(orders.size() == 2 && orders[0] != orders[1], "over-request exports each unique order once");
	ok(orders[0][0] == 2 && orders[1][0] == 2, "every order opens with favorite blue");
	ok(orders == spark::generateOrders(plan, 100, 42), "fixed seed reproduces orders");
	ok(spark::generateOrders(plan, 0).isEmpty(), "zero requested orders returns immediately");
	for (const auto &order : orders) {
		const auto copy = spark::applyOrder(plan, order);
		ok(copy.tracks[0].clips[2].outStartMs == 0 && copy.contentDurationMs() == 3000, "Spark closes independent gaps and starts at zero");
		ok(copy.tracks[0].clips[0].srcStartMs == 100 && copy.tracks[0].clips[0].srcEndMs == 1100, "Spark retains source trims");
	}
	ok(m.tracks[0].clips[2].outStartMs == 9000 && m.tracks[0].clips[2].favorite, "planning and generation preserve original timeline");
	m.tracks[0].clips[2].favorite = false;
	ok(spark::buildPlan(m).uniqueOrders == 6, "without favorites all footage shuffles freely");
	m.tracks[0].clips[1] = clip(1, 6000); m.tracks[0].clips[2] = clip(1, 9000);
	ok(spark::buildPlan(m).uniqueOrders == 1 && spark::generateOrders(spark::buildPlan(m), 100).size() == 1,
	   "identical copies do not inflate unique count");
	m.tracks[0].clips[2].favorite = true;
	ok(spark::buildPlan(m).uniqueOrders == 1, "favorite and ordinary copies have separate permutation classes");
	m.tracks[0].clips = {clip(1, 1000, 7, 4), clip(2, 3500, 7), clip(3, 7000)};
	m.tracks[0].clips[1].favorite = true; m.tracks[0].clips[0].speed = 2;
	TlTrack audio; audio.kind = TlTrack::Kind::Audio;
	audio.clips = {clip(1, 1200, 9, 4), clip(2, 2000, 9), clip(4, 500)};
	m.tracks.append(audio);
	TlClip text = clip(0, 1500); text.type = TlClip::Type::Text; m.tracks[0].clips.append(text);
	plan = spark::buildPlan(m);
	ok(plan.blocks.size() == 2 && plan.blocks[0].members.size() == 4 && plan.blocks[0].favorite,
	   "groups and media links combine transitively into favorite blocks");
	const auto groupedCopy = spark::applyOrder(plan, {0, 1});
	ok(groupedCopy.tracks[0].clips[1].outStartMs == 2500 && groupedCopy.tracks[1].clips[0].outStartMs == 200,
	   "grouped blocks retain their internal gaps and offsets");
	ok(groupedCopy.tracks[0].clips[0].speed == 2 && groupedCopy.tracks[0].clips[0].srcStartMs == 100,
	   "block placement preserves speed and trims");
	ok(groupedCopy.tracks[1].clips[2] == m.tracks[1].clips[2] && groupedCopy.tracks[0].clips[3] == text,
	   "unrelated background audio and text stay at original times");
	m.tracks[1].locked = true;
	ok(!spark::buildPlan(m).available() && spark::buildPlan(m).error.contains(QStringLiteral("Unlock")), "locked linked member explains why Spark is disabled");
	m.tracks[1].clips = {clip(4, 500)};
	ok(spark::buildPlan(m).available(), "locked background tracks do not block Spark");
	m.tracks[0].hidden = true; ok(!spark::buildPlan(m).available(), "hidden footage is excluded");
	m.tracks[0].hidden = false; m.videoEndMs = 100;
	plan = spark::buildPlan(m);
	ok(plan.available() && plan.blocks.size() == 2 && plan.uniqueOrders == 1,
	   "footage after an early marker still takes part; with the star first, one video can be seen");

	// Every clip is shuffled, before the end marker and after it; each export
	// stops at the marker. Red and green before it, starred blue and yellow
	// after: blue opens every video, and the second second is red, green OR
	// yellow -- three different videos.
	TimelineModel pastMarker;
	TlTrack footage;
	footage.clips = {clip(1, 0), clip(2, 1000), clip(3, 2000), clip(4, 3000)};
	footage.clips[2].favorite = true;
	TlClip caption = clip(0, 2500);
	caption.type = TlClip::Type::Text;
	TlTrack captionLane;
	captionLane.clips = {caption};
	TlTrack music;
	music.kind = TlTrack::Kind::Audio;
	music.locked = true; // background on a locked lane does not block Spark
	music.clips = {clip(99, 0)};
	music.clips[0].srcEndMs = 5100;
	pastMarker.tracks = {footage, captionLane, music};
	pastMarker.videoEndMs = 2000;
	pastMarker.tags = {{1, QStringLiteral("Keep my sound rule")}};
	TlSoundRule rule;
	rule.id = 1; rule.sourceId = 99; rule.trigger = TlSoundRule::Trigger::VideoStarts;
	pastMarker.soundRules = {rule};
	pastMarker.markers = {500, 2600};
	const TimelineModel unchanged = pastMarker;
	plan = spark::buildPlan(pastMarker);
	ok(plan.available() && plan.blocks.size() == 4 && plan.favoriteCount == 1,
	   "every footage block takes part, after the marker as well, stars included");
	ok(plan.uniqueOrders == 3 && !plan.uniqueOrdersAtLeast,
	   "counted up to the marker: blue, then one of three -- three different videos");
	ok(plan.timeline == pastMarker, "the export snapshot is the whole timeline, marker, tags and sound rules included");
	const auto pastOrders = spark::generateOrders(plan, 100, 42);
	QSet<int> seconds;
	bool blueFirst = true;
	for (const auto &order : pastOrders) {
		blueFirst &= order.first() == 2;
		seconds.insert(order.value(1));
	}
	ok(pastOrders.size() == 3 && blueFirst, "three videos, each opening with starred blue");
	ok(seconds == QSet<int>({0, 1, 3}), "and yellow, from after the marker, is shuffled in like red and green");
	for (const auto &order : pastOrders) {
		auto copy = spark::applyOrder(plan, order);
		ok(copy.videoEndMs == 2000 && copy.durationMs() == 2000, "every export stops at the unchanged marker");
		ok(copy.tracks[1].clips[0].outStartMs == 2500 && copy.tracks[2].clips[0].outStartMs == 0,
		   "text and background sound keep their times");
		SoundRules::apply(copy, [](int) { return SoundInfo{3000, {}}; });
		ok(copy.durationMs() == 2000 && copy.contentDurationMs() > 2000,
		   "rebuilt event sounds cannot lengthen the export past the marker");
	}
	ok(pastMarker == unchanged, "planning and shuffling leave the editor timeline and marker unchanged");

	TimelineModel lockedPast = pastMarker;
	TlTrack lockedLane;
	lockedLane.locked = true;
	lockedLane.clips = {clip(5, 3500, 6)};
	lockedPast.tracks[0].clips[3].group = 6; // yellow, after the marker, grouped with a locked clip
	lockedPast.tracks.append(lockedLane);
	ok(!spark::buildPlan(lockedPast).available() && spark::buildPlan(lockedPast).error.contains(QStringLiteral("Unlock")),
	   "a locked member after the marker blocks Spark too: it is shuffled like the rest");

	TimelineModel crossing;
	TlTrack longAndShort;
	longAndShort.clips = {clip(1, 0), clip(2, 1000)};
	longAndShort.clips[1].srcEndMs = 2100; // two seconds
	crossing.tracks = {longAndShort};
	crossing.videoEndMs = 1500;
	ok(spark::buildPlan(crossing).uniqueOrders == 2, "a block reaching past the marker counts as what is seen of it");

	TimelineModel many30;
	many30.tracks = {TlTrack{}};
	for (int i = 0; i < 30; ++i) many30.tracks[0].clips.append(clip(i + 1, i * 1000));
	many30.videoEndMs = 3500; // four blocks start before it
	plan = spark::buildPlan(many30);
	std::printf("     30 one-second blocks, marker at 3.5 s: %lld videos\n", (long long)plan.uniqueOrders);
	ok(plan.uniqueOrders == 30LL * 29 * 28 * 27 && !plan.uniqueOrdersAtLeast,
	   "only the first four blocks are seen: 30 x 29 x 28 x 27 videos, counted exactly");
	ok(spark::generateOrders(plan, 1000, 7).size() == 1000, "and 1000 different ones are found");

	TimelineModel tiny;
	tiny.tracks = {TlTrack{}};
	for (int i = 0; i < 60; ++i) {
		TlClip c = clip(i + 1, i * 100);
		c.srcEndMs = 200; // 0.1 s each
		tiny.tracks[0].clips.append(c);
	}
	tiny.videoEndMs = 5000; // fifty of sixty seen: far too many to count
	plan = spark::buildPlan(tiny);
	std::printf("     60 tenth-second blocks, marker at 5 s: at least %lld videos\n", (long long)plan.uniqueOrders);
	ok(plan.uniqueOrdersAtLeast && plan.uniqueOrders >= 1, "too many to count: a lower bound, said to be one");
	ok(spark::generateOrders(plan, 50, 3).size() == 50, "and it does not cap the export: 50 asked, 50 made");
	TimelineModel equivalentCuts;
	TlTrack cutTrack;
	cutTrack.clips = {clip(1, 0), clip(1, 0)};
	cutTrack.clips[1].srcEndMs = 2100;
	equivalentCuts.tracks = {cutTrack}; equivalentCuts.videoEndMs = 500;
	ok(spark::buildPlan(equivalentCuts).uniqueOrders == 1,
	   "unique counts compare clips after cutting them at the marker");

	TimelineModel equivalent; TlTrack e; e.kind = TlTrack::Kind::Video;
	e.clips = {clip(1, 1000, 7, 4), clip(2, 2500, 7), clip(2, 6500, 8), clip(1, 5000, 8, 5)};
	equivalent.tracks = {e};
	ok(spark::buildPlan(equivalent).uniqueOrders == 1, "canonical fingerprint ignores group/link IDs and member vector ordering");
	equivalent.tracks[0].clips[2].rotation = 2;
	ok(spark::buildPlan(equivalent).uniqueOrders == 2, "fingerprint includes edited clip properties");
	TimelineModel many; many.tracks = {TlTrack{}};
	for (int i = 0; i < 30; ++i) many.tracks[0].clips.append(clip(i + 1, i * 1000));
	plan = spark::buildPlan(many);
	ok(plan.uniqueOrders == spark::kOrderCountCap, "large permutation counts cap without overflow");
	ok(spark::generateOrders(plan, 1200, 14).size() == 1000, "one request is capped at 1000 exports");
	TimelineModel withTexts;
	TlTrack pictures;
	pictures.clips = {clip(1, 0), clip(2, 1000), clip(3, 2000)};
	pictures.clips[0].clipOptions = {{1, 100}, {2, 200}};
	TlClip firstText;
	firstText.type = TlClip::Type::Text;
	firstText.srcEndMs = 3000;
	firstText.text.random = true;
	firstText.text.text = QStringLiteral("Opening A");
	firstText.text.variations = {QStringLiteral("Opening B"), QStringLiteral(""), QStringLiteral("   "),
		QStringLiteral("<b>Opening C</b>")};
	firstText.text.fontPx = 77;
	firstText.words = {{QStringLiteral("Opening"), 0, 1000}};
	TlClip secondText = firstText;
	secondText.text.text = QStringLiteral("Ending A");
	secondText.text.variations = {QStringLiteral("Ending B")};
	TlClip plainText = firstText;
	plainText.text.random = false; // retained alternatives must not vary plain Text
	TlClip singleText = firstText;
	singleText.text.variations = {QStringLiteral(""), QStringLiteral("   ")};
	TlTrack captions;
	captions.locked = true; // export choices can vary without editing this locked lane
	captions.clips = {firstText, secondText, plainText, singleText};
	withTexts.tracks = {pictures, captions};
	withTexts.videoEndMs = 3000;
	const auto textPlan = spark::buildPlan(withTexts);
	ok(textPlan.available() && textPlan.uniqueOrders == 12 && textPlan.uniqueFootageOrders == 6 &&
		   spark::generateOrders(textPlan, 100, 42).size() == 6,
	   "Random Text options do not multiply the count; the existing Random Clip doubles six orders to twelve videos");
	std::mt19937_64 textRng(42), repeatRng(42);
	ok(spark::pickRandomTexts(textPlan.timeline, textRng) == spark::pickRandomTexts(textPlan.timeline, repeatRng),
	   "Random Text export choices can be reproduced with a fixed seed");
	QSet<QString> firstChoices, secondChoices;
	QSet<QPair<QString, QString>> combinations;
	bool validChoices = true, preserved = true, timingsValid = true;
	for (int i = 0; i < 64; ++i) {
		const auto chosen = spark::pickRandomTexts(textPlan.timeline, textRng);
		const TlClip &a = chosen.tracks[1].clips[0], &b = chosen.tracks[1].clips[1];
		firstChoices.insert(a.text.text); secondChoices.insert(b.text.text);
		combinations.insert({a.text.text, b.text.text});
		validChoices &= text_variations::optionsOf(firstText.text).contains(a.text.text) &&
			text_variations::optionsOf(secondText.text).contains(b.text.text);
		TlClip restoredA = a, restoredB = b;
		restoredA.text.text = firstText.text.text; restoredA.words = firstText.words;
		restoredB.text.text = secondText.text.text; restoredB.words = secondText.words;
		preserved &= restoredA == firstText && restoredB == secondText &&
			chosen.tracks[0] == withTexts.tracks[0] && chosen.tracks[1].clips[2] == plainText &&
			chosen.tracks[1].clips[3] == singleText && chosen.videoEndMs == 3000;
		timingsValid &= (a.text.text == firstText.text.text ? a.words == firstText.words : a.words.isEmpty()) &&
			(b.text.text == secondText.text.text ? b.words == secondText.words : b.words.isEmpty());
	}
	ok(validChoices && firstChoices.size() == 3 && secondChoices.size() == 2,
	   "each Random Text samples its own current text and nonblank alternatives");
	ok(combinations.size() > 3, "different Random Text clips pick independently rather than sharing one option index");
	ok(preserved, "sampling retains text style, placement, alternatives, plain Text, Random Clips and end marker");
	ok(timingsValid, "changed text drops stale word timings while unchanged text retains them");
	ok(withTexts.tracks[1].clips[0] == firstText && textPlan.timeline.tracks[1].clips[0] == firstText,
	   "sampling export copies never changes the editor or the shared Spark plan");

	// ---- Spark settings: openers, a range of favourites, a placement ----
	std::printf("\n-- Spark settings --\n");
	{
		TlClip markedClip = clip(1, 0);
		markedClip.sparkMust = markedClip.sparkOpener = true;
		const TlClip back = clipFromJson(clipToJson(markedClip));
		ok(back.sparkMust && back.sparkOpener && !clipFromJson(clipToJson(clip(1, 0))).sparkMust &&
		   !clipToJson(clip(1, 0)).contains(QStringLiteral("sparkMust")),
		   "Must include and Opening clip round trip through project JSON; unmarked clips save no keys");
		TlClip other = markedClip;
		other.sparkOpener = false;
		ok(!(other == markedClip), "clip equality includes the marks");
		TimelineModel withSettings = footageOf(1, 1);
		withSettings.spark.favoritesMin = 2;
		withSettings.spark.favoritesMax = 4;
		withSettings.spark.placement = TlSparkSettings::Placement::Ends;
		QJsonObject root;
		timelineToProject(withSettings, root);
		const TimelineModel reopened = timelineFromProject(root);
		ok(reopened.spark == withSettings.spark, "the Spark settings are saved with the project");
		withSettings.spark.placement = TlSparkSettings::Placement::Random;
		QJsonObject randomRoot;
		timelineToProject(withSettings, randomRoot);
		ok(timelineFromProject(randomRoot).spark.placement == TlSparkSettings::Placement::Random,
		   "and Anywhere (random) is saved too");
		TimelineModel defaults = withSettings;
		defaults.spark = TlSparkSettings{};
		QJsonObject plain;
		timelineToProject(defaults, plain);
		ok(!plain.contains(QStringLiteral("spark")) && timelineFromProject(plain).spark.isDefault(),
		   "default settings save nothing, and older projects open with them");
		ok(!(reopened == defaults), "timeline equality includes the settings, so an undo step carries them");
		ok(!spark::detail::configured(spark::buildPlan(footageOf(2, 2))),
		   "with no settings and no opening clip, Spark takes its original path");
		TimelineModel same = footageOf(0, 2);
		same.tracks[0].clips[1] = clip(1, 1000);
		same.tracks[0].clips[1].sparkMust = true;
		same.tracks[0].clips[0].sparkOpener = true;
		const auto samePlan = spark::buildPlan(same);
		ok(samePlan.blocks[0].fingerprint == samePlan.blocks[1].fingerprint, "the marks are not part of how a block looks");
	}

	// A range: 1 to 3 of 6 favourites, always first, then 4 others.
	TimelineModel ranged = footageOf(6, 4);
	ranged.spark.favoritesMin = 1;
	ranged.spark.favoritesMax = 3;
	plan = spark::buildPlan(ranged);
	std::printf("     6 favourites, 1 to 3 a video, 4 others: %lld videos\n", (long long)plan.uniqueOrders);
	ok(plan.uniqueOrders == (6 + 30 + 120) * 24 && !plan.uniqueOrdersAtLeast,
	   "1, 2 or 3 of 6 favourites in order, then 4 others in any order: 3744 videos, counted exactly");
	const auto rangedOrders = spark::generateOrders(plan, 60, 5);
	{
		QSet<int> sizes;
		bool inRange = true, favoritesFirst = true, leftOut = true, shorter = true;
		for (const auto &order : rangedOrders) {
			int k = 0;
			for (int i : order)
				k += plan.blocks[i].favorite ? 1 : 0;
			sizes.insert(k);
			inRange &= k >= 1 && k <= 3 && order.size() == k + 4;
			for (int i = 0; i < order.size(); ++i)
				favoritesFirst &= plan.blocks[order[i]].favorite == (i < k);
			const auto copy = spark::applyOrder(plan, order);
			int starred = 0;
			for (const TlClip &c : copy.tracks[0].clips)
				starred += c.favorite ? 1 : 0;
			leftOut &= copy.tracks[0].clips.size() == k + 4 && starred == k;
			shorter &= copy.durationMs() == (k + 4) * 1000;
		}
		ok(rangedOrders.size() == 60, "60 different videos");
		ok(inRange && sizes == QSet<int>({1, 2, 3}), "each has 1 to 3 favourites, and every number in the range turns up");
		ok(favoritesFirst, "Always first: the favourites open each video");
		ok(leftOut, "the favourites a video does not pick are not in it at all");
		ok(shorter, "and the video ends with its footage, not on a blank picture");
		ok(rangedOrders == spark::generateOrders(plan, 60, 5), "a fixed seed gives the same videos");
	}
	TimelineModel few = footageOf(2, 1);
	few.spark.favoritesMin = few.spark.favoritesMax = 1;
	ok(spark::buildPlan(few).uniqueOrders == 2 && spark::generateOrders(spark::buildPlan(few), 100, 1).size() == 2,
	   "one of two favourites, then the other clip: exactly two videos, both found");

	// Must include: in every video, counted in the range.
	TimelineModel must = footageOf(6, 4);
	must.tracks[0].clips[0].sparkMust = true;
	must.spark.favoritesMin = 1;
	must.spark.favoritesMax = 2;
	plan = spark::buildPlan(must);
	ok(plan.mustCount == 1 && plan.favoriteCount == 6 && plan.uniqueOrders == 24 + 5 * 2 * 24,
	   "Must include counts in the range: alone, or with one of the other five -- 264 videos");
	{
		bool always = true;
		for (const auto &order : spark::generateOrders(plan, 100, 9))
			always &= order.contains(0);
		ok(always, "and it is in every video");
	}
	must.spark.favoritesMin = must.spark.favoritesMax = 0;
	plan = spark::buildPlan(must);
	{
		bool always = true;
		const auto orders0 = spark::generateOrders(plan, 100, 9);
		for (const auto &order : orders0)
			always &= order.size() == 5 && order.first() == 0;
		ok(plan.uniqueOrders == 24 && orders0.size() == 24 && always,
		   "a range of none still has it, first; the other favourites are left out");
	}
	TimelineModel unstarred = footageOf(0, 3);
	unstarred.tracks[0].clips[2].sparkMust = true;
	plan = spark::buildPlan(unstarred);
	{
		bool first = true;
		for (const auto &order : spark::generateOrders(plan, 100, 2))
			first &= order.first() == 2;
		ok(plan.favoriteCount == 1 && plan.uniqueOrders == 2 && first,
		   "a Must include clip without a star is a favourite too");
	}

	// Opening clips.
	TimelineModel opening = footageOf(2, 4);
	opening.tracks[0].clips[5].sparkOpener = true;
	plan = spark::buildPlan(opening);
	{
		const auto orders1 = spark::generateOrders(plan, 30, 3);
		bool first = true;
		for (const auto &order : orders1)
			first &= order.first() == 5 && order.size() == 6;
		ok(plan.openerCount == 1 && plan.favoriteCount == 2 && plan.uniqueOrders == 12 && orders1.size() == 12 && first,
		   "one opening clip: all 12 videos start with it, the favourites after it");
	}
	opening.tracks[0].clips[4].sparkOpener = true;
	plan = spark::buildPlan(opening);
	{
		const auto orders2 = spark::generateOrders(plan, 8, 3);
		int byFour = 0, byFive = 0;
		bool alone = true, atZero = true;
		for (const auto &order : orders2) {
			byFour += order.first() == 4 ? 1 : 0;
			byFive += order.first() == 5 ? 1 : 0;
			alone &= !order.mid(1).contains(4) && !order.mid(1).contains(5);
			const auto copy = spark::applyOrder(plan, order);
			atZero &= copy.tracks[0].clips.size() == 5;
			for (const TlClip &c : copy.tracks[0].clips)
				if (c.sparkOpener)
					atZero &= c.outStartMs == 0;
		}
		ok(plan.uniqueOrders == 8 && orders2.size() == 8 && byFour == 4 && byFive == 4,
		   "two opening clips take turns: each opens 4 of the 8 videos");
		ok(orders2.size() >= 2 && orders2[0].first() != orders2[1].first(), "one after the other");
		ok(alone && atZero, "and the other opening clip is not in that video");
	}

	// Placement.
	const auto favoriteSlots = [](const spark::Plan &p, const QVector<int> &order, int upTo) {
		QVector<int> at;
		for (int i = 0; i < std::min<int>(upTo, order.size()); ++i)
			if (p.blocks[order[i]].favorite)
				at.append(i);
		return at;
	};
	TimelineModel spread = footageOf(3, 6);
	spread.spark.placement = TlSparkSettings::Placement::Spread;
	plan = spark::buildPlan(spread);
	{
		bool even = true;
		const auto orders3 = spark::generateOrders(plan, 20, 4);
		for (const auto &order : orders3)
			even &= favoriteSlots(plan, order, 9) == QVector<int>({0, 3, 6});
		ok(plan.uniqueOrders == 6 * 720 && orders3.size() == 20 && even,
		   "Spread out: three favourites in nine seconds come at 0, 3 and 6 s");
	}
	spread.videoEndMs = 6000;
	plan = spark::buildPlan(spread);
	{
		bool even = true;
		for (const auto &order : spark::generateOrders(plan, 20, 4))
			even &= favoriteSlots(plan, order, 9) == QVector<int>({0, 2, 4}) && order.size() == 9;
		ok(even, "with an end marker at 6 s, they spread over what is seen: 0, 2 and 4 s");
	}
	TimelineModel ends = footageOf(4, 6);
	ends.spark.placement = TlSparkSettings::Placement::Ends;
	plan = spark::buildPlan(ends);
	{
		bool both = true;
		for (const auto &order : spark::generateOrders(plan, 20, 6))
			both &= favoriteSlots(plan, order, 10) == QVector<int>({0, 1, 8, 9});
		ok(both, "Beginning and end: two favourites open the video and two finish it");
	}
	ends.videoEndMs = 7000;
	plan = spark::buildPlan(ends);
	{
		bool both = true;
		for (const auto &order : spark::generateOrders(plan, 20, 6))
			both &= favoriteSlots(plan, order, 10) == QVector<int>({0, 1, 5, 6});
		ok(both, "with an end marker at 7 s, the last two finish exactly at the marker");
	}
	TimelineModel anywhere = footageOf(2, 3);
	anywhere.spark.placement = TlSparkSettings::Placement::Random;
	plan = spark::buildPlan(anywhere);
	{
		const auto orders4 = spark::generateOrders(plan, 200, 11);
		QSet<int> places;
		bool ordinaryFirst = false;
		for (const auto &order : orders4) {
			for (int at : favoriteSlots(plan, order, 5))
				places.insert(at);
			ordinaryFirst |= !plan.blocks[order.first()].favorite;
		}
		ok(plan.uniqueOrders == 120 && orders4.size() == 120,
		   "Anywhere: two favourites and three others in any order -- all 120 videos");
		ok(places == QSet<int>({0, 1, 2, 3, 4}) && ordinaryFirst,
		   "a favourite turns up in every place, and some videos open with an ordinary clip");
	}
	anywhere.spark.favoritesMin = anywhere.spark.favoritesMax = 1;
	plan = spark::buildPlan(anywhere);
	{
		bool one = true;
		const auto orders5 = spark::generateOrders(plan, 200, 12);
		for (const auto &order : orders5)
			one &= order.size() == 4 && favoriteSlots(plan, order, 4).size() == 1;
		ok(plan.uniqueOrders == 2 * 24 && orders5.size() == 48 && one,
		   "with one favourite a video, anywhere: either one, in any of four places -- 48 videos");
	}

	// What a video leaves out takes its linked media along; the rest stays.
	TimelineModel linked = footageOf(2, 2);
	linked.tracks[0].clips[0].link = 4;
	TlTrack voice;
	voice.kind = TlTrack::Kind::Audio;
	voice.clips = {clip(50, 0, 0, 4), clip(60, 0)};
	voice.clips[1].srcEndMs = 10100; // ten seconds of music
	linked.tracks.append(voice);
	linked.spark.favoritesMin = linked.spark.favoritesMax = 1;
	plan = spark::buildPlan(linked);
	{
		bool with = false, without = false, right = true, nineSeconds = true;
		for (const auto &order : spark::generateOrders(plan, 10, 2)) {
			const auto copy = spark::applyOrder(plan, order);
			if (order.contains(0)) {
				with = true;
				right &= copy.tracks[1].clips.size() == 2;
			} else {
				without = true;
				right &= copy.tracks[1].clips.size() == 1 && copy.tracks[1].clips[0].sourceId == 60;
			}
			nineSeconds &= copy.durationMs() == 9000;
		}
		ok(with && without && right, "a favourite left out takes its linked sound with it; the music stays");
		ok(nineSeconds, "the video is a second shorter, and the music still runs on after the footage as before");
	}
	TimelineModel marked = footageOf(2, 6);
	marked.videoEndMs = 4000;
	marked.spark.favoritesMin = marked.spark.favoritesMax = 0;
	plan = spark::buildPlan(marked);
	{
		bool atMarker = true;
		for (const auto &order : spark::generateOrders(plan, 10, 2))
			atMarker &= spark::applyOrder(plan, order).durationMs() == 4000;
		ok(atMarker, "with footage enough to reach the end marker, every video still stops there");
	}

	// Many favourites, a range, spread out, a marker: too many to count.
	TimelineModel big = footageOf(30, 20);
	big.spark.favoritesMin = 3;
	big.spark.favoritesMax = 5;
	big.spark.placement = TlSparkSettings::Placement::Spread;
	big.videoEndMs = 15000;
	{
		QElapsedTimer took;
		took.start();
		plan = spark::buildPlan(big);
		const qint64 countMs = took.elapsed();
		const auto bigOrders = spark::generateOrders(plan, 300, 8);
		std::printf("     30 favourites (3 to 5), 20 others, spread, marker at 15 s: at least %lld videos; "
			    "counted in %lld ms, 300 made in %lld ms\n",
			    (long long)plan.uniqueOrders, (long long)countMs, (long long)(took.elapsed() - countMs));
		ok(plan.uniqueOrdersAtLeast && bigOrders.size() == 300, "too many to count: a lower bound, and 300 are made");
		ok(took.elapsed() < 20000, "counting and making them stays quick");
	}


	QVector<QByteArray> repeated;
	for (int i = 0; i < 20; ++i) repeated.append("same");
	ok(spark::permutationCount(repeated) == 1, "multiset count divides out equivalent copies");
	repeated.append("different");
	ok(spark::permutationCount(repeated) == 21, "multiset count retains distinct placements");
	return failures ? 1 : 0;
}
