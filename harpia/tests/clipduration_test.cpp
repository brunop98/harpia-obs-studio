// Set a common duration through the real clip menu and its dialog. Durations
// are in output time: every video keeps its source in-point and playback rate.
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static TlClip video(qint64 start, qint64 from, qint64 to, double speed = 1.0)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 1;
	c.outStartMs = start;
	c.srcStartMs = from;
	c.srcEndMs = to;
	c.speed = speed;
	return c;
}

static TimelineModel fixture()
{
	TimelineModel m;
	TlTrack videos;
	videos.kind = TlTrack::Kind::Video;
	videos.clips = {video(0, 1000, 7000), video(6000, 2000, 6000, 2.0),
			video(8000, 500, 3500, 0.5), video(15000, 0, 1000)};
	videos.clips[0].fadeInMs = videos.clips[0].fadeOutMs = 5000;
	videos.clips[0].setKeyframeAt(3000, TlTransform{});
	m.tracks.append(videos);
	TlTrack mixed;
	mixed.kind = TlTrack::Kind::Video;
	for (const auto type : {TlClip::Type::Text, TlClip::Type::Image, TlClip::Type::Effect}) {
		TlClip c = video(16000, 0, 50);
		c.type = type;
		mixed.clips.append(c);
	}
	m.tracks.append(mixed);
	TlTrack audio;
	audio.kind = TlTrack::Kind::Audio;
	audio.clips = {video(0, 0, 50)};
	m.tracks.append(audio);
	TlTrack locked = videos;
	locked.locked = true;
	locked.clips = {video(0, 0, 50)};
	m.tracks.append(locked);
	return m;
}

static void selectVideos(TimelineView &v)
{
	v.selectClip(0, 0);
	v.addToSelection(0, 1);
	v.addToSelection(0, 2);
}

struct DialogResult {
	bool action = false, dialog = false, timedOut = false;
	double minimum = 0, maximum = 0, initial = 0;
};

static DialogResult chooseDuration(TimelineView &v, int clip, double seconds, bool accept,
				   bool activate = true)
{
	DialogResult result;
	QTimer driver, watchdog;
	driver.setInterval(10);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
			for (QAction *a : menu->actions()) {
				if (!a->text().startsWith(QStringLiteral("Set duration…")))
					continue;
				result.action = a->isEnabled();
				if (activate && a->isEnabled()) {
					menu->setActiveAction(a);
					QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
					QApplication::sendEvent(menu, &enter);
					return;
				}
			}
			menu->close();
			return;
		}
		for (QWidget *widget : QApplication::topLevelWidgets()) {
			auto *dialog = qobject_cast<QInputDialog *>(widget);
			if (!dialog || !dialog->isVisible() ||
			    dialog->objectName() != QStringLiteral("setClipDurationDialog"))
				continue;
			result.dialog = true;
			result.minimum = dialog->doubleMinimum();
			result.maximum = dialog->doubleMaximum();
			result.initial = dialog->doubleValue();
			dialog->setDoubleValue(seconds);
			if (accept)
				dialog->accept();
			else
				dialog->reject();
			return;
		}
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		result.timedOut = true;
		for (QWidget *widget : QApplication::topLevelWidgets()) {
			if (auto *dialog = qobject_cast<QInputDialog *>(widget))
				dialog->reject();
			if (auto *menu = qobject_cast<QMenu *>(widget))
				menu->close();
		}
	});
	driver.start();
	watchdog.start(3000);
	const QPoint at = v.clipRectForTest(0, clip).center();
	QMouseEvent right(QEvent::MouseButtonPress, QPointF(at), v.mapToGlobal(at), Qt::RightButton,
			  Qt::RightButton, Qt::NoModifier);
	QApplication::sendEvent(&v, &right);
	driver.stop();
	watchdog.stop();
	return result;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	TimelineView v;
	v.resize(1400, 400);
	v.show();
	QApplication::processEvents();
	int commits = 0, updates = 0;
	QObject::connect(&v, &TimelineView::editCommitted, [&]() { ++commits; });
	QObject::connect(&v, &TimelineView::clipsChanged, [&]() { ++updates; });
	const TimelineModel original = fixture();

	std::printf("\n-- the shortest selected VIDEO sets the maximum --\n");
	v.setModel(original);
	selectVideos(v);
	for (int i = 0; i < 3; ++i)
		v.addToSelection(1, i);
	v.addToSelection(2, 0);
	v.addToSelection(3, 0);
	ok(v.selectedVideoDurationLimitMs() == 2000,
	   "2 seconds in output time; unselected, non-video and locked clips do not limit it");
	const auto selection = v.selectedPairs();
	commits = updates = 0;
	ok(v.trimSelectedVideosToDuration(1000) == 3, "all three selected videos were trimmed");
	for (int i = 0; i < 3; ++i) {
		const TlClip &c = v.model().tracks[0].clips[i];
		const TlClip &was = original.tracks[0].clips[i];
		ok(c.outDurationMs() == 1000, "the video lasts exactly one second on the timeline");
		ok(c.srcStartMs == was.srcStartMs && c.outStartMs == was.outStartMs && c.speed == was.speed,
		   "its source in-point, timeline position and speed are preserved");
		ok(c.srcEndMs < was.srcEndMs, "the end was trimmed, without extending footage");
	}
	ok(v.model().tracks[0].clips[0].srcEndMs == 2000 &&
		   v.model().tracks[0].clips[1].srcEndMs == 4000 &&
		   v.model().tracks[0].clips[2].srcEndMs == 1000,
	   "normal, double-speed and half-speed clips use their own source-time lengths");
	ok(v.model().tracks[0].clips[0].fadeInMs == 1000 && v.model().tracks[0].clips[0].fadeOutMs == 1000,
	   "fades cannot outlast the shortened clip");
	ok(v.model().tracks[0].clips[0].keys == original.tracks[0].clips[0].keys,
	   "animation keys are preserved, as with an ordinary right-edge trim");
	ok(v.model().tracks[0].clips[3] == original.tracks[0].clips[3], "the unselected video is untouched");
	for (int i = 1; i < original.tracks.size(); ++i)
		ok(v.model().tracks[i] == original.tracks[i], "non-video and locked tracks are untouched");
	ok(v.selectedPairs() == selection, "the complete selection is preserved");
	ok(commits == 1 && updates == 1, "one completed edit and one preview update for the whole batch");
	commits = updates = 0;
	ok(v.trimSelectedVideosToDuration(1000) == 0 && commits == 0 && updates == 0,
	   "repeating the current duration does not create an undo entry");

	std::printf("\n-- bounds are enforced before changing any clip --\n");
	v.setModel(original);
	selectVideos(v);
	commits = 0;
	for (qint64 invalid : {-1000, 0, 99, 2001, 6000})
		ok(v.trimSelectedVideosToDuration(invalid) == 0, "an out-of-range duration is rejected");
	ok(v.model() == original && commits == 0, "invalid requests do not partly trim the selection");
	ok(v.trimSelectedVideosToDuration(2000) == 2 && commits == 1,
	   "the shortest clip's exact duration is accepted; only the longer clips change");

	std::printf("\n-- the real context menu preserves the selection and opens a bounded dialog --\n");
	v.setModel(original);
	selectVideos(v);
	const auto videos = v.selectedPairs();
	commits = 0;
	DialogResult result = chooseDuration(v, 1, 1.0, true);
	ok(result.action && result.dialog && !result.timedOut, "right-click a second selected video: Set duration opens");
	ok(result.minimum == 0.1 && result.maximum == 2.0 && result.initial == 2.0,
	   "the dialog defaults to the shortest clip and allows 0.1 to 2 seconds");
	ok(v.selectedPairs() == videos, "right-click keeps the previously primary clip selected too");
	for (int i = 0; i < 3; ++i)
		ok(v.model().tracks[0].clips[i].outDurationMs() == 1000, "OK trims every selected video to one second");
	ok(commits == 1, "the menu action is one undo step");

	v.setModel(original);
	selectVideos(v);
	commits = 0;
	result = chooseDuration(v, 0, 1.0, false);
	ok(result.dialog && v.model() == original && commits == 0, "Cancel leaves every clip unchanged");
	result = chooseDuration(v, 0, 99.0, true);
	ok(result.dialog && v.model().tracks[0].clips[0].outDurationMs() == 2000 &&
		   v.model().tracks[0].clips[1].outDurationMs() == 2000,
	   "the dialog clamps an oversized value to the shortest selected video");
	v.setModel(original);
	selectVideos(v);
	result = chooseDuration(v, 3, 1.0, false, false);
	ok(!result.action && !result.dialog && v.selectedPairs().size() == 1 && v.isSelected(0, 3),
	   "right-clicking an unselected video starts a single selection, not a batch edit");
	ok(v.model() == original, "dismissing that menu changes no durations");
	v.selectClip(1, 0);
	ok(v.selectedVideoDurationLimitMs() == 0 && v.trimSelectedVideosToDuration(1000) == 0,
	   "a selection without an editable video has no duration action");
	v.selectClip(-1, -1);
	ok(v.selectedVideoDurationLimitMs() == 0 && v.trimSelectedVideosToDuration(1000) == 0,
	   "no selection is a no-op");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
