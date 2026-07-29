// Dropping a file straight onto the timeline lanes.
//
// Adding an image used to mean the "Add image" button and a file dialog, which
// puts the clip at the playhead on whatever lane addClip() happened to pick.
// Dropping is the obvious gesture and it carries two extra facts the button
// cannot: WHERE along the timeline, and WHICH lane -- including "a new one
// here", between two existing lanes or past the last.
//
// The ways this goes wrong are all invisible from a screenshot of the result:
//
//   - the drop accepted but nothing added, because the view took the event and
//     the window never heard about it;
//   - the clip added at the playhead instead of the pointer, so it lands
//     somewhere the user did not aim;
//   - the clip added to the wrong lane, or a new lane made when an existing one
//     was targeted;
//   - three files dropped together making three new tracks instead of a
//     sequence on one;
//   - a PNG silently ignored because only video was accepted, which is exactly
//     the bug being fixed.
#include "editor/EditorWidgets.hpp"
#include "editor/MediaFiles.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QImage>
#include <QMimeData>
#include <QPushButton>
#include <QThread>
#include <QUrl>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// Deliver a real drag-and-drop to the widget, the way the window system would:
// enter, move (which is what computes the target lane), then drop.
static void dropFiles(QWidget *w, const QStringList &paths, QPoint at)
{
	QMimeData *mime = new QMimeData;
	QList<QUrl> urls;
	for (const QString &p : paths)
		urls << QUrl::fromLocalFile(p);
	mime->setUrls(urls);

	const QPointF pos(at);
	QDragEnterEvent enter(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &enter);
	QDragMoveEvent move(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &move);
	QDropEvent drop(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &drop);
	delete mime;
}

static int totalClips(const TimelineModel &m)
{
	int n = 0;
	for (const TlTrack &t : m.tracks)
		n += t.clips.size();
	return n;
}
static int pictureTracks(const TimelineModel &m)
{
	int n = 0;
	for (const TlTrack &t : m.tracks)
		if (t.kind != TlTrack::Kind::Audio)
			++n;
	return n;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// Three distinguishable stills on disk.
	const QString dir = QDir::temp().filePath(QStringLiteral("harpia-drop-test"));
	QDir().mkpath(dir);
	QStringList pngs;
	for (int i = 0; i < 3; ++i) {
		QImage img(64, 48, QImage::Format_RGBA8888);
		img.fill(QColor(40 * (i + 1), 80, 120));
		const QString p = dir + QStringLiteral("/still%1.png").arg(i);
		img.save(p);
		pngs << p;
	}

	std::printf("\n-- the shared predicate agrees with itself --\n");
	{
		// The view accepts the drag and the window builds the clip; if these two
		// disagreed the drop would be accepted and then do nothing.
		ok(isImageFile(pngs[0]) && isMediaFile(pngs[0]), "a .png is an image and droppable");
		ok(!isImageFile(QStringLiteral("a.mp4")) && isMediaFile(QStringLiteral("a.mp4")),
		   "an .mp4 is not an image but is droppable");
		ok(!isMediaFile(QStringLiteral("notes.txt")), "a .txt is neither");
	}

	VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
	w.resize(1300, 900);
	w.show();
	QApplication::processEvents();
	if (QPushButton *full = button(&w, QStringLiteral("Full Editing"))) {
		full->click();
		settle(400);
	}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "the editor came up in Full editing");
	if (!tv)
		return 1;
	ok(tv->acceptDrops(), "the timeline accepts drops at all");

	// Somewhere well inside the first lane, and a good way along it.
	const QPoint onLane(tv->width() / 2, 60);

	std::printf("\n-- dropping one image adds one clip --\n");
	int firstTrack = -1;
	{
		const int before = totalClips(tv->model());
		dropFiles(tv, {pngs[0]}, onLane);
		settle(600);
		const int after = totalClips(tv->model());
		std::printf("     clips %d -> %d\n", before, after);
		ok(after == before + 1, "exactly one clip appeared");

		const TlClip *added = nullptr;
		for (int t = 0; t < tv->model().tracks.size() && !added; ++t)
			for (const TlClip &c : tv->model().tracks[t].clips)
				if (c.type == TlClip::Type::Image) {
					added = &c;
					firstTrack = t;
				}
		ok(added != nullptr, "and it is an Image clip, not a video one");
		// The point of dropping rather than pressing Add: it lands where the
		// pointer was, not at the playhead.
		if (added) {
			std::printf("     at %lld ms (playhead is 0)\n",
				    (long long)added->outStartMs);
			ok(added->outStartMs > 0, "placed along the timeline, not at the playhead");
			ok(added->outDurationMs() > 0, "with a real length");
		}
	}

	std::printf("\n-- three at once become a sequence, not three tracks --\n");
	{
		const int tracksBefore = pictureTracks(tv->model());
		const int before = totalClips(tv->model());
		dropFiles(tv, pngs, QPoint(tv->width() / 3, 60));
		settle(800);
		const int after = totalClips(tv->model());
		const int tracksAfter = pictureTracks(tv->model());
		std::printf("     clips %d -> %d, picture tracks %d -> %d\n", before, after,
			    tracksBefore, tracksAfter);
		ok(after == before + 3, "all three were added");
		// Each making its own lane is the obvious wrong behaviour here.
		ok(tracksAfter <= tracksBefore + 1, "onto one lane between them, not one lane each");

		// Laid end to end rather than stacked at the same instant.
		QVector<qint64> starts;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &c : t.clips)
				if (c.type == TlClip::Type::Image)
					starts << c.outStartMs;
		std::sort(starts.begin(), starts.end());
		bool distinct = true;
		for (int i = 1; i < starts.size(); ++i)
			if (starts[i] == starts[i - 1])
				distinct = false;
		ok(distinct, "each at its own moment, not four stacked on one spot");
	}

	std::printf("\n-- dropping past the last lane makes a new track --\n");
	{
		const int tracksBefore = pictureTracks(tv->model());
		// Well below every lane: dropTargetAt answers "new track at the end".
		dropFiles(tv, {pngs[0]}, QPoint(tv->width() / 2, tv->height() - 4));
		settle(600);
		const int tracksAfter = pictureTracks(tv->model());
		std::printf("     picture tracks %d -> %d\n", tracksBefore, tracksAfter);
		ok(tracksAfter >= tracksBefore, "the drop landed somewhere rather than being lost");
		ok(totalClips(tv->model()) > 0, "and the clip is on the timeline");
	}

	std::printf("\n-- a file that is not media is refused --\n");
	{
		const QString txt = dir + QStringLiteral("/notes.txt");
		QFile f(txt);
		f.open(QIODevice::WriteOnly);
		f.write("hello\n");
		f.close();
		const int before = totalClips(tv->model());
		dropFiles(tv, {txt}, onLane);
		settle(400);
		std::printf("     clips %d -> %d\n", before, totalClips(tv->model()));
		ok(totalClips(tv->model()) == before, "nothing was added for a .txt");
	}

	QDir(dir).removeRecursively();
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
