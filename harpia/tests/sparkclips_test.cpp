#include "editor/timeline/Spark.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>

using namespace harpia;
namespace {
int failures;
void check(bool pass, const char *why)
{
	std::printf("%s %s\n", pass ? "PASS" : "FAIL", why);
	failures += !pass;
}
TlClip clip(int id, qint64 at = 0)
{
	TlClip c;
	c.sourceId = id;
	c.srcEndMs = 1000;
	c.outStartMs = at;
	return c;
}
TimelineModel timeline(const QVector<TlClip> &clips)
{
	TimelineModel m;
	TlTrack t;
	t.clips = clips;
	m.tracks = {t};
	return m;
}
QSet<QByteArray> keys(const spark::Plan &plan, const QVector<spark::Video> &videos)
{
	QSet<QByteArray> out;
	for (const auto &video : videos) {
		out.insert(spark::detail::videoKey(spark::applyVideo(plan, video)));
	}
	return out;
}
} // namespace
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	TlClip random = clip(1);
	random.clipOptions = {{1, 0, -0.2, 0.1}, {2, 100, 0.1, -0.2}, {3, 200, 0.3, 0.15}};
	random.speed = 2;
	random.crop = QRect(2, 4, 100, 80);
	random.fadeInMs = 40;
	random.fadeOutMs = 60;
	TlKeyframe key;
	key.tMs = 100;
	key.tf.posX = 0.4;
	random.keys = {key};
	ComponentInstance effect;
	effect.typeId = QStringLiteral("harpia.alwaysRotate");
	effect.instanceId = QStringLiteral("keep");
	random.components = {effect};
	const auto original = timeline({random, clip(99, 2000)});
	auto plan = spark::buildPlan(original);
	check(plan.available() && plan.uniqueOrders == 6 && !plan.uniqueOrdersAtLeast && plan.uniqueFootageOrders == 2,
	      "two block orders times three Random Clip options count as six unique videos");
	const auto videos = spark::generateVideos(plan, 100, 42);
	check(videos.size() == 6 && keys(plan, videos).size() == 6,
	      "over-requesting exports each distinct order/choice once");
	check(videos == spark::generateVideos(plan, 100, 42), "a fixed seed reproduces order and footage choices");
	QSet<int> shown;
	bool preserved = true;
	for (const auto &video : videos) {
		const auto output = spark::applyVideo(plan, video);
		const auto &c = output.tracks[0].clips[0];
		const auto option = random.clipOptionAt(c.clipShown);
		shown.insert(c.clipShown);
		preserved &= c.sourceId == option.sourceId && c.srcStartMs == option.srcStartMs &&
			     c.srcEndMs == option.srcStartMs + 1000 && c.outDurationMs() == 500 && c.speed == 2 &&
			     c.optionPositionOffset() == QPointF(option.offsetX, option.offsetY) &&
			     c.crop == random.crop && c.components == random.components && c.keys == random.keys &&
			     c.fadeInMs == 40 && c.fadeOutMs == 60;
	}
	check(shown.size() == 3 && preserved,
	      "each option retains its source window, position, crop, speed, effects, keys and fades");
	check(plan.timeline == original, "planning and sampling never change the editor's shared timeline");
	check(spark::generateVideos(plan, 0).isEmpty(), "zero requested videos does no generation work");

	TlClip a = clip(1), b = clip(3, 1000);
	a.clipOptions = {{1, 0}, {2, 0}};
	b.clipOptions = {{3, 0}, {4, 0}};
	a.group = b.group = 7;
	plan = spark::buildPlan(timeline({a, b}));
	check(plan.uniqueOrders == 4 && keys(plan, spark::generateVideos(plan, 100, 7)).size() == 4,
	      "two grouped Random Clips choose independently, giving four combinations in one order");
	a = clip(1);
	a.clipOptions = {{1, 0}, {1, 0}, {1, 0}};
	plan = spark::buildPlan(timeline({a, clip(2, 1000)}));
	check(plan.uniqueOrders == 2 && spark::generateVideos(plan, 100, 8).size() == 2,
	      "duplicate entries in the option list never inflate the count or repeat exports");
	a.clipOptions = {{1, 0}, {2, 0}};
	plan = spark::buildPlan(timeline({a, clip(2, 1000)}));
	check(plan.uniqueOrders == 3 && keys(plan, spark::generateVideos(plan, 100, 8)).size() == 3,
	      "overlapping choices and interchangeable footage deduplicate across different orders");
	const auto before = keys(plan, spark::generateVideos(plan, 100, 8));
	clip_variations::show(a, 1);
	plan = spark::buildPlan(timeline({a, clip(2, 1000)}));
	check(plan.uniqueOrders == 3 && keys(plan, spark::generateVideos(plan, 100, 8)) == before,
	      "the current preview choice cannot alter the available unique exports");

	a = clip(1);
	b = clip(2, 1000);
	b.clipOptions = {{2, 0}, {3, 0}, {4, 0}};
	a.group = b.group = 9;
	auto cut = timeline({a, b});
	cut.videoEndMs = 500;
	plan = spark::buildPlan(cut);
	check(plan.uniqueOrders == 1 && spark::generateVideos(plan, 100, 3).size() == 1,
	      "a Random Clip entirely beyond the end marker adds no invisible combinations");
	cut.videoEndMs = 1500;
	plan = spark::buildPlan(cut);
	check(plan.uniqueOrders == 3 && keys(plan, spark::generateVideos(plan, 100, 3)).size() == 3,
	      "a partially visible Random Clip contributes its distinct choices before the marker");

	a = clip(1);
	a.favorite = true;
	a.clipOptions = {{1, 0}, {2, 0}};
	auto favorites = timeline({a, clip(3, 1000)});
	favorites.spark.favoritesMin = 0;
	favorites.spark.favoritesMax = 1;
	plan = spark::buildPlan(favorites);
	const auto filtered = spark::generateVideos(plan, 100, 9);
	int omitted = 0;
	for (const auto &video : filtered) {
		omitted += spark::applyVideo(plan, video).tracks[0].clips.size() == 1;
	}
	check(plan.uniqueOrders == 3 && filtered.size() == 3 && omitted == 1,
	      "an omitted favorite adds no choices, while each included option remains distinct");

	a = clip(1);
	b = clip(2);
	a.sparkOpener = b.sparkOpener = true;
	b.clipOptions.clear();
	for (int i = 2; i < 10; ++i) {
		b.clipOptions.append({i, 0});
	}
	plan = spark::buildPlan(timeline({a, b, clip(99, 2000)}));
	check(plan.uniqueOrders == 9 && keys(plan, spark::generateVideos(plan, 100, 3)).size() == 9,
	      "an exhausted opener hands over so other opening clips can export all their options");

	a = clip(1);
	a.clipOptions = {{1, 0}, {2, 0}};
	auto withAudio = timeline({clip(99)});
	TlTrack sound;
	sound.kind = TlTrack::Kind::Audio;
	sound.clips = {a};
	withAudio.tracks.append(sound);
	plan = spark::buildPlan(withAudio);
	check(plan.uniqueOrders == 2 && spark::generateVideos(plan, 100, 3).size() == 2,
	      "an audible background Random Clip also chooses its own source per video");
	withAudio.tracks[1].muted = true;
	check(spark::buildPlan(withAudio).uniqueOrders == 1, "muted alternatives do not inflate the video count");
	withAudio.tracks[1].muted = false;
	withAudio.tracks[0].solo = true;
	check(spark::buildPlan(withAudio).uniqueOrders == 1, "soloing another track excludes inaudible alternatives");
	withAudio.tracks[0].solo = false;
	withAudio.tracks[1].kind = TlTrack::Kind::Video;
	withAudio.tracks[1].hidden = true;
	check(spark::buildPlan(withAudio).uniqueOrders == 2,
	      "hidden video options still count when their sound is audible");

	TimelineModel large = timeline({});
	for (int i = 0; i < 20; ++i) {
		TlClip c = clip(100 + i * 2, i * 1000);
		c.group = 1;
		c.clipOptions = {{c.sourceId, 0}, {c.sourceId + 1, 0}};
		large.tracks[0].clips.append(c);
	}
	QElapsedTimer clock;
	clock.start();
	plan = spark::buildPlan(large);
	check(plan.uniqueOrdersAtLeast && plan.uniqueOrders >= 1 && clock.elapsed() < 2000,
	      "large choice spaces use a bounded lower-bound count instead of blocking the editor");
	const auto many = spark::generateVideos(plan, 100, 5);
	check(many.size() == 100 && keys(plan, many).size() == 100,
	      "a lower-bound count still permits all 100 requested unique videos");
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
