// "Set image time": one length for every selected image and text clip, from
// quick picks or a typed value, through the real clip menu (Timing).
//
//   * Which clips: the selected images and texts on unlocked lanes when the
//     right-clicked clip is selected, else that clip alone; never a video.
//   * Later clips on the same lane move by the change, so a slideshow stays
//     back to back and a gap after it stays the gap it was; a clip linked to
//     a moved one, on another lane, moves with it.
//   * Exact at any speed, fades pulled in, keys kept, out-of-range and
//     no-change requests touch nothing, one undo step for the whole batch.
#include "support/ContextMenu.hpp"
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

static TlClip still(TlClip::Type type, qint64 start, qint64 len, double speed = 1.0)
{
	TlClip c;
	c.type = type;
	c.sourceId = 1;
	c.outStartMs = start;
	c.srcStartMs = 0;
	c.srcEndMs = qint64(len * speed);
	c.speed = speed;
	return c;
}

// Lane 0: three images back to back, a 1 s gap, a video, a text.
// Lane 1: audio linked to that video. Lane 2: locked, one image.
// Lane 3: an image at double speed.
static TimelineModel fixture()
{
	TimelineModel m;
	TlTrack pics;
	pics.kind = TlTrack::Kind::Video;
	pics.clips = {still(TlClip::Type::Image, 0, 2000), still(TlClip::Type::Image, 2000, 2000),
		      still(TlClip::Type::Image, 4000, 2000), still(TlClip::Type::Video, 7000, 2000),
		      still(TlClip::Type::Text, 9000, 2000)};
	pics.clips[0].fadeInMs = 1500;
	pics.clips[0].setKeyframeAt(300, TlTransform{});
	pics.clips[3].link = 7;
	m.tracks.append(pics);
	TlTrack audio;
	audio.kind = TlTrack::Kind::Audio;
	audio.clips = {still(TlClip::Type::Video, 7000, 2000)};
	audio.clips[0].link = 7;
	m.tracks.append(audio);
	TlTrack locked;
	locked.kind = TlTrack::Kind::Video;
	locked.locked = true;
	locked.clips = {still(TlClip::Type::Image, 0, 2000)};
	m.tracks.append(locked);
	TlTrack fast;
	fast.kind = TlTrack::Kind::Video;
	fast.clips = {still(TlClip::Type::Image, 0, 3000, 2.0)};
	m.tracks.append(fast);
	return m;
}

static QVector<QPair<qint64, qint64>> spans(const TimelineView &v, int track)
{
	QVector<QPair<qint64, qint64>> out;
	for (const TlClip &c : v.model().tracks[track].clips)
		out.append({c.outStartMs, c.outEndMs()});
	return out;
}

static void printSpans(const char *what, const TimelineView &v, int track)
{
	std::printf("     %s:", what);
	for (const auto &s : spans(v, track))
		std::printf(" [%.1f-%.1f]", s.first / 1000.0, s.second / 1000.0);
	std::printf("\n");
}

struct MenuResult {
	bool found = false, enabled = false, checked = false, dialog = false, timedOut = false;
	QStringList labels; // every "Set image time" entry in Timing
	double initial = 0;
};

// Right-click (track, clip), open Timing, and choose the entry starting with
// `pick` (empty: choose nothing). A Custom dialog gets `seconds`, accepted or not.
static MenuResult viaMenu(TimelineView &v, int track, int clip, const QString &pick, double seconds = 0,
			  bool accept = true)
{
	MenuResult r;
	QTimer driver, watchdog;
	driver.setInterval(10);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
			if (menu->objectName() == QLatin1String("timelineClipMenu"))
				menu = test_menu::openSubmenu(menu, QStringLiteral("Timing"));
			QAction *chosen = nullptr;
			for (QAction *a : menu->actions()) {
				if (a->text().startsWith(QStringLiteral("Set image time")))
					r.labels << a->text();
				if (!pick.isEmpty() && a->text().startsWith(pick) && !chosen) {
					chosen = a;
					r.found = true;
					r.enabled = a->isEnabled();
					r.checked = a->isChecked();
				}
			}
			if (chosen && chosen->isEnabled()) {
				menu->setActiveAction(chosen);
				QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
				QApplication::sendEvent(menu, &enter);
				return;
			}
			test_menu::dismiss(menu);
			return;
		}
		for (QWidget *w : QApplication::topLevelWidgets()) {
			auto *dialog = qobject_cast<QInputDialog *>(w);
			if (!dialog || !dialog->isVisible() || dialog->objectName() != QLatin1String("setImageTimeDialog"))
				continue;
			r.dialog = true;
			r.initial = dialog->doubleValue();
			dialog->setDoubleValue(seconds);
			if (accept)
				dialog->accept();
			else
				dialog->reject();
			return;
		}
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		r.timedOut = true;
		for (QWidget *w : QApplication::topLevelWidgets()) {
			if (auto *dialog = qobject_cast<QInputDialog *>(w))
				dialog->reject();
			if (auto *menu = qobject_cast<QMenu *>(w))
				test_menu::dismiss(menu);
		}
	});
	driver.start();
	watchdog.start(3000);
	const QPoint at = v.clipRectForTest(track, clip).center();
	QMouseEvent right(QEvent::MouseButtonPress, QPointF(at), v.mapToGlobal(at), Qt::RightButton, Qt::RightButton,
			  Qt::NoModifier);
	QApplication::sendEvent(&v, &right);
	driver.stop();
	watchdog.stop();
	return r;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	TimelineView v;
	v.resize(1600, 500);
	v.show();
	QApplication::processEvents();
	int commits = 0;
	QObject::connect(&v, &TimelineView::editCommitted, [&]() { ++commits; });
	const TimelineModel original = fixture();

	std::printf("\n-- which clips --\n");
	v.setModel(original);
	ok(v.imageTimeTargets(0, 1) == QVector<QPair<int, int>>({{0, 1}}),
	   "an unselected image: just that one");
	v.selectClip(0, 0);
	for (const auto &p : QVector<QPair<int, int>>{{0, 1}, {0, 3}, {0, 4}, {2, 0}, {3, 0}})
		v.addToSelection(p.first, p.second);
	const auto targets = v.imageTimeTargets(0, 0);
	ok(targets == QVector<QPair<int, int>>({{0, 0}, {0, 1}, {0, 4}, {3, 0}}),
	   "selected: the images and the text; not the video, not the locked lane");
	ok(v.imageTimeTargets(0, 3).isEmpty(), "a video has no image time");
	ok(v.imageTimeTargets(2, 0).isEmpty(), "nor does a clip on a locked lane");

	std::printf("\n-- shorter: what follows closes up --\n");
	commits = 0;
	printSpans("lane 0 before", v, 0);
	ok(v.setImageTime(targets, 1000) == 4, "four clips changed");
	printSpans("lane 0 after ", v, 0);
	ok(spans(v, 0) == QVector<QPair<qint64, qint64>>(
				  {{0, 1000}, {1000, 2000}, {2000, 4000}, {5000, 7000}, {7000, 8000}}),
	   "1 s each, back to back; the unselected image follows; the gap before the video is kept");
	ok(v.model().tracks[1].clips[0].outStartMs == 5000, "the video's linked audio moved with it");
	ok(v.model().tracks[2] == original.tracks[2], "the locked lane is untouched");
	ok(v.model().tracks[3].clips[0].outDurationMs() == 1000 && v.model().tracks[3].clips[0].srcLenMs() == 2000,
	   "at double speed: exactly 1 s on screen");
	ok(v.model().tracks[0].clips[0].fadeInMs == 1000, "a fade longer than the clip is pulled in");
	ok(v.model().tracks[0].clips[0].keys == original.tracks[0].clips[0].keys, "its keyframes are kept");
	ok(v.model().tracks[0].clips[3].srcLenMs() == 2000, "the video's own length is not touched");
	ok(commits == 1, "one undo step for the whole batch");

	std::printf("\n-- longer: what follows makes room --\n");
	v.setModel(original);
	commits = 0;
	ok(v.setImageTime({{0, 0}}, 2500) == 1, "one image to 2.5 s");
	ok(spans(v, 0).mid(0, 4) ==
		   QVector<QPair<qint64, qint64>>({{0, 2500}, {2500, 4500}, {4500, 6500}, {7500, 9500}}),
	   "everything after it moves 0.5 s later, nothing overlaps");
	ok(v.model().tracks[1].clips[0].outStartMs == 7500, "linked audio too");

	std::printf("\n-- requests that change nothing --\n");
	v.setModel(original);
	commits = 0;
	ok(v.setImageTime({{0, 0}, {0, 1}}, 2000) == 0 && commits == 0, "the length they already have: no undo step");
	ok(v.setImageTime({{0, 0}}, 50) == 0 && v.setImageTime({{0, 0}}, TimelineView::kMaxImageTimeMs + 1) == 0,
	   "below 0.1 s or above an hour: refused");
	ok(v.setImageTime({{0, 3}}, 1000) == 0 && v.setImageTime({{2, 0}}, 1000) == 0,
	   "a video, or a locked lane: refused");
	ok(v.model() == original && commits == 0, "and nothing was touched by any of them");

	std::printf("\n-- through the real menu --\n");
	v.setModel(original);
	v.selectClip(0, 0);
	v.addToSelection(0, 1);
	QApplication::processEvents();
	MenuResult r = viaMenu(v, 0, 0, QStringLiteral("Set image time: 1 s"));
	for (const QString &l : r.labels)
		std::printf("     Timing > %s\n", qPrintable(l));
	ok(!r.timedOut && r.found && r.enabled, "Timing has Set image time: 1 s, live");
	ok(r.labels.size() == 6 && r.labels.last().startsWith(QStringLiteral("Set image time: Custom")),
	   "0.5, 1, 2, 3, 5 s and Custom, flat in Timing (no submenu inside it)");
	ok(r.labels.first().endsWith(QStringLiteral("(2 clips)")), "saying how many clips it is for");
	ok(spans(v, 0).mid(0, 3) == QVector<QPair<qint64, qint64>>({{0, 1000}, {1000, 2000}, {2000, 4000}}),
	   "a quick pick: both selected images are 1 s, the next one closes up");
	r = viaMenu(v, 0, 0, QStringLiteral("Set image time: 1 s"));
	ok(r.checked, "the length they now share is ticked");

	r = viaMenu(v, 0, 0, QStringLiteral("Set image time: Custom"), 0.5, true);
	std::printf("     the box opened at %.2f s\n", r.initial);
	ok(!r.timedOut && r.dialog && r.initial == 1.0, "Custom opens a box at the clip's current time");
	ok(spans(v, 0).mid(0, 3) == QVector<QPair<qint64, qint64>>({{0, 500}, {500, 1000}, {1000, 3000}}),
	   "typing 0.5: both are half a second");
	const TimelineModel beforeCancel = v.model();
	r = viaMenu(v, 0, 0, QStringLiteral("Set image time: Custom"), 3.0, false);
	ok(r.dialog && v.model() == beforeCancel, "Cancel changes nothing");

	r = viaMenu(v, 2, 0, QStringLiteral("Set image time: 1 s"));
	ok(r.found && !r.enabled, "on a locked lane: shown, greyed out");
	r = viaMenu(v, 0, 3, QStringLiteral("Set image time"));
	ok(!r.timedOut && r.labels.isEmpty(), "on a video: not offered");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
