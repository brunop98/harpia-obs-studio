#include "editor/timeline/Spark.hpp"
#include "editor/timeline/SoundRules.hpp"
#include <QCoreApplication>
#include <cstdio>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why) { std::printf("%s %s\n", pass ? "PASS" : "FAIL", why); failures += !pass; }
static TlClip clip(int id, qint64 start, int group = 0, int link = 0)
{
	TlClip c; c.sourceId = id; c.srcStartMs = 100; c.srcEndMs = 1100; c.outStartMs = start; c.group = group; c.link = link;
	return c;
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
	ok(!plan.available() && plan.blocks.isEmpty() && plan.uniqueOrders == 0,
	   "no footage before the marker disables Spark instead of expanding the end");
	TimelineModel bounded;
	TlTrack footage;
	footage.clips = {clip(1, 0, 5, 9), clip(2, 1200, 5), clip(3, 1500, 5), clip(4, 500)};
	footage.clips[1].speed = 2;
	footage.clips[2].favorite = true; // starts exactly at the marker: excluded
	footage.clips[3].type = TlClip::Type::Image;
	footage.clips[3].speed = 0.5;
	TlTrack linked;
	linked.kind = TlTrack::Kind::Audio;
	linked.clips = {clip(1, 300, 0, 9)};
	TlTrack past;
	past.locked = true;
	past.clips = {clip(3, 1700, 5)};
	TlTrack music;
	music.kind = TlTrack::Kind::Audio;
	music.locked = true;
	music.clips = {clip(99, 0)};
	music.clips[0].srcEndMs = 5100;
	bounded.tracks = {footage, linked, past, music};
	bounded.videoEndMs = 1500;
	bounded.tags = {{1, QStringLiteral("Keep my sound rule")}};
	TlSoundRule rule;
	rule.id = 1; rule.sourceId = 99; rule.trigger = TlSoundRule::Trigger::VideoStarts;
	bounded.soundRules = {rule};
	bounded.markers = {500, 1600};
	const TimelineModel unchanged = bounded;
	plan = spark::buildPlan(bounded);
	ok(plan.available() && plan.blocks.size() == 2 && plan.favoriteCount == 0 && plan.uniqueOrders == 2,
	   "marker excludes later footage, stars and locked members before counting orders");
	ok(plan.timeline.tracks[0].clips.size() == 3 && plan.timeline.tracks[2].clips.isEmpty(),
	   "clips at or beyond the marker are removed from the export snapshot");
	ok(plan.timeline.tracks[0].clips[1].srcEndMs == 700 &&
	   plan.timeline.tracks[0].clips[1].outDurationMs() == 300,
	   "group member crossing the marker is cut in source time at its playback speed");
	ok(plan.timeline.tracks[0].clips[2].outDurationMs() == 1000,
	   "image crossing the marker is cut rather than excluded");
	ok(plan.blocks[0].members.size() == 3 && plan.blocks[0].durationMs == 1500,
	   "cut group retains surviving linked partners and internal timing");
	ok(plan.timeline.tags == bounded.tags && plan.timeline.soundRules == bounded.soundRules,
	   "clipped snapshot retains tags and event-sound rules");
	ok(plan.timeline.tracks[3].clips[0].outStartMs == 0 &&
	   plan.timeline.tracks[3].clips[0].outDurationMs() == 1500,
	   "background music keeps its position and is cut at the same boundary");
	ok(plan.timeline.markers == QVector<qint64>{500}, "markers beyond the export boundary are excluded");
	for (const auto &order : spark::generateOrders(plan, 10, 42)) {
		auto copy = spark::applyOrder(plan, order);
		ok(copy.videoEndMs == 1500 && copy.durationMs() == 1500,
		   "overlapping blocks sequenced end to end never extend the end marker");
		SoundRules::apply(copy, [](int) { return SoundInfo{3000, {}}; });
		ok(copy.durationMs() == 1500 && copy.contentDurationMs() > 1500,
		   "rebuilt event sounds cannot lengthen the export past the marker");
	}
	ok(bounded == unchanged, "cutting and shuffling leave the editor timeline and marker unchanged");
	TimelineModel bridges;
	TlTrack bridgeTrack;
	bridgeTrack.clips = {clip(1, 0, 11), clip(2, 500, 12), clip(3, 2000, 11, 99), clip(4, 3000, 12, 99)};
	bridges.tracks = {bridgeTrack}; bridges.videoEndMs = 1000;
	const auto bridgePlan = spark::buildPlan(bridges);
	ok(bridgePlan.blocks.size() == 1 && bridgePlan.blocks[0].members.size() == 2,
	   "cutting off link bridges does not split surviving members of the original block");
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
	QVector<QByteArray> repeated;
	for (int i = 0; i < 20; ++i) repeated.append("same");
	ok(spark::permutationCount(repeated) == 1, "multiset count divides out equivalent copies");
	repeated.append("different");
	ok(spark::permutationCount(repeated) == 21, "multiset count retains distinct placements");
	return failures ? 1 : 0;
}
