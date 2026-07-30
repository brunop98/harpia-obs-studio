// Dropping a music file onto the timeline.
//
// Before this, an .mp3 dragged onto the lanes fell through to the STILL-IMAGE
// branch: the still reader was handed an audio file, failed, and put up "could
// not be decoded", which was true of the reader and useless to the person
// holding the file. Audio was reachable only through a menu and a file dialog.
//
// The interesting failures are all about where it lands rather than whether it
// arrives, and none of them show up in a screenshot of a clip on a lane:
//
//   - an audio clip placed on a VIDEO lane, where the compositor will look for
//     a picture in it;
//   - an audio track inserted among the video tracks, which breaks the
//     video-then-audio ordering the whole model assumes;
//   - the drop indicator highlighting a video lane during the drag and then the
//     clip landing somewhere else entirely;
//   - a video and its music dropped together getting queued end to end instead
//     of stacked, so the music starts after the picture finishes;
//   - the clip arriving with no peaks, so the lane shows an empty box and there
//     is nothing to line up against.
#include "editor/MediaFiles.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
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

static void dropFiles(QWidget *w, const QStringList &paths, QPoint at)
{
	QMimeData *mime = new QMimeData;
	QList<QUrl> urls;
	for (const QString &p : paths)
		urls << QUrl::fromLocalFile(p);
	mime->setUrls(urls);
	QDragEnterEvent enter(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &enter);
	QDragMoveEvent move(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &move);
	QDropEvent drop(QPointF(at), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &drop);
	delete mime;
}

static bool ffmpeg(const QStringList &args)
{
	QProcess p;
	p.start(QStringLiteral("ffmpeg"), args);
	return p.waitForFinished(60000) && p.exitCode() == 0;
}

static int audioTracks(const TimelineModel &m)
{
	int n = 0;
	for (const TlTrack &t : m.tracks)
		if (t.kind == TlTrack::Kind::Audio)
			++n;
	return n;
}
static int clipsOnAudio(const TimelineModel &m)
{
	int n = 0;
	for (const TlTrack &t : m.tracks)
		if (t.kind == TlTrack::Kind::Audio)
			n += t.clips.size();
	return n;
}
static int clipsOnPicture(const TimelineModel &m)
{
	int n = 0;
	for (const TlTrack &t : m.tracks)
		if (t.kind != TlTrack::Kind::Audio)
			n += t.clips.size();
	return n;
}
// Video tracks must all come before audio tracks; the compositor and the
// exporter both walk the list assuming it.
static bool orderingIntact(const TimelineModel &m)
{
	bool seenAudio = false;
	for (const TlTrack &t : m.tracks) {
		if (t.kind == TlTrack::Kind::Audio)
			seenAudio = true;
		else if (seenAudio)
			return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- what counts as audio --\n");
	{
		ok(isAudioFile(QStringLiteral("song.mp3")) && isMediaFile(QStringLiteral("song.mp3")),
		   "an .mp3 is audio, and droppable at all");
		ok(isAudioFile(QStringLiteral("VOICE.WAV")), "the test is case-insensitive");
		// The distinction that decides which lane a drop lands on. An mp4 can
		// hold audio, but dropping one means you want the video.
		ok(!isAudioFile(QStringLiteral("clip.mp4")) && isVideoFile(QStringLiteral("clip.mp4")),
		   "an .mp4 is video, not audio, even though it carries sound");
		ok(!isAudioFile(QStringLiteral("shot.png")), "a .png is not audio");
		ok(!isMediaFile(QStringLiteral("notes.txt")), "and a .txt is nothing droppable");

		// The dialogs and the drop path read one list, after two hand-written
		// ones had already drifted (one offered *.wma, the other did not).
		const QString filter = audioOpenFilter();
		std::printf("     %s\n", qPrintable(filter.left(110)));
		for (const char *e : {"*.mp3", "*.wav", "*.flac", "*.wma", "*.opus"})
			ok(filter.contains(QLatin1String(e)),
			   QByteArray("the dialog offers ").append(e).constData());

		ok(TimelineView::allAudio({QStringLiteral("a.mp3"), QStringLiteral("b.wav")}),
		   "a drag of nothing but audio is an audio drag");
		ok(!TimelineView::allAudio({QStringLiteral("a.mp3"), QStringLiteral("b.mp4")}),
		   "a mixed drag is not -- its audio finds a lane on its own");
		ok(!TimelineView::allAudio({}), "and an empty drag is not an audio drag");
	}

	QTemporaryDir dir;
	const QString mp3 = dir.filePath(QStringLiteral("tone.mp3"));
	const QString wav = dir.filePath(QStringLiteral("beep.wav"));
	const QString mp4 = dir.filePath(QStringLiteral("movie.mp4"));
	const bool haveMp3 =
		ffmpeg({QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
			QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
			QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=3"),
			QStringLiteral("-y"), mp3});
	const bool haveWav =
		ffmpeg({QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
			QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
			QStringLiteral("-i"), QStringLiteral("sine=frequency=880:duration=2"),
			QStringLiteral("-y"), wav});
	const bool haveMp4 = ffmpeg({QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
				     QStringLiteral("error"), QStringLiteral("-f"),
				     QStringLiteral("lavfi"), QStringLiteral("-i"),
				     QStringLiteral("testsrc=size=160x120:duration=2:rate=15"),
				     QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
				     QStringLiteral("-y"), mp4});
	ok(haveMp3 && haveWav && haveMp4, "made an mp3, a wav and an mp4 to drop");
	if (!haveMp3 || !haveWav || !haveMp4)
		return 1;

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

	std::printf("\n-- an mp3 dropped on the lanes becomes an audio clip --\n");
	{
		const int before = clipsOnAudio(tv->model());
		// Against a baseline, not zero: entering Full editing seeds the source
		// video onto a picture lane, so "picture clips == 0" would be measuring
		// that rather than the drop.
		const int picBefore = clipsOnPicture(tv->model());
		// Dropped over the MIDDLE of the widget, which is a video lane on a
		// fresh timeline: an audio clip cannot live there, and the wrong answer
		// is to put it there anyway.
		dropFiles(tv, {mp3}, QPoint(tv->width() / 2, tv->height() / 2));
		settle(1500);

		std::printf("     audio clips %d -> %d, audio tracks %d, picture clips %d -> %d\n",
			    before, clipsOnAudio(tv->model()), audioTracks(tv->model()), picBefore,
			    clipsOnPicture(tv->model()));
		ok(clipsOnAudio(tv->model()) == before + 1, "one clip arrived on an audio lane");
		ok(clipsOnPicture(tv->model()) == picBefore,
		   "and nothing new was put on a picture lane");
		ok(orderingIntact(tv->model()),
		   "video tracks still all come before audio tracks");

		// Found by walking to it rather than assuming an index, because which
		// lane it chose is exactly what is under test.
		const TlClip *c = nullptr;
		for (const TlTrack &t : tv->model().tracks)
			if (t.kind == TlTrack::Kind::Audio && !t.clips.isEmpty())
				c = &t.clips.last();
		ok(c != nullptr, "the clip is findable on an audio track");
		if (c) {
			std::printf("     %lld ms long, %d peaks\n", (long long)c->outDurationMs(),
				    int(c->peaks.size()));
			// ~3s of sine. A zero-length clip would be invisible and
			// undraggable, which is the shape of "the decode quietly failed".
			ok(c->outDurationMs() > 2500 && c->outDurationMs() < 3500,
			   "as long as the file it came from");
			// Without peaks the lane shows an empty box and there is nothing
			// to line a cut up against, which is most of the point of having
			// the sound on the timeline at all.
			ok(!c->peaks.isEmpty(), "and it carries a waveform");
		}
	}

	std::printf("\n-- the drag is aimed at the audio lanes, not the video ones --\n");
	{
		// The indicator during the drag has to promise where the drop will
		// actually land. Over a video lane, an audio drag must not highlight it.
		QMimeData *mime = new QMimeData;
		mime->setUrls({QUrl::fromLocalFile(wav)});
		const QPoint over(tv->width() / 2, tv->height() / 4); // up among the pictures
		QDragEnterEvent enter(over, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &enter);
		QDragMoveEvent move(over, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &move);
		const int aimed = tv->dropTargetTrackForTest();
		std::printf("     drag over y=%d aims at track %d (kind %s)\n", over.y(), aimed,
			    aimed >= 0 && aimed < tv->model().tracks.size()
				    ? (tv->model().tracks[aimed].kind == TlTrack::Kind::Audio
					       ? "audio"
					       : "picture")
				    : "none/new");
		ok(aimed < 0 || aimed >= tv->model().tracks.size() ||
			   tv->model().tracks[aimed].kind == TlTrack::Kind::Audio,
		   "an audio drag never highlights a picture lane");
		QDragLeaveEvent leave;
		QApplication::sendEvent(tv, &leave);
		delete mime;
	}

	std::printf("\n-- a video and its music land stacked, not queued --\n");
	{
		const int picBefore = clipsOnPicture(tv->model());
		const int audBefore = clipsOnAudio(tv->model());
		dropFiles(tv, {mp4, wav}, QPoint(tv->width() / 2, tv->height() / 2));
		settle(2000);

		const TlClip *pic = nullptr;
		const TlClip *aud = nullptr;
		for (const TlTrack &t : tv->model().tracks) {
			if (t.clips.isEmpty())
				continue;
			if (t.kind == TlTrack::Kind::Audio)
				aud = &t.clips.last();
			else
				pic = &t.clips.last();
		}
		ok(clipsOnPicture(tv->model()) == picBefore + 1, "the video landed on a picture lane");
		ok(clipsOnAudio(tv->model()) == audBefore + 1, "the music landed on an audio lane");
		ok(orderingIntact(tv->model()), "and the track ordering survived a mixed drop");
		if (pic && aud) {
			std::printf("     video starts at %lld ms, music at %lld ms\n",
				    (long long)pic->outStartMs, (long long)aud->outStartMs);
			// One running position for both would have started the music
			// where the video ended -- the whole reason they are tracked
			// separately.
			ok(pic->outStartMs == aud->outStartMs,
			   "both start at the drop point, so the music plays UNDER the video");
		}
	}

	std::printf("\n-- two music files dropped together are a sequence on one lane --\n");
	{
		const int lanesBefore = audioTracks(tv->model());
		const int before = clipsOnAudio(tv->model());
		dropFiles(tv, {mp3, wav}, QPoint(tv->width() / 3, tv->height() / 2));
		settle(2500);
		std::printf("     audio clips %d -> %d, audio lanes %d -> %d\n", before,
			    clipsOnAudio(tv->model()), lanesBefore, audioTracks(tv->model()));
		ok(clipsOnAudio(tv->model()) == before + 2, "both arrived");
		// Two files must not mean two new lanes -- the same rule the picture
		// drop already follows.
		ok(audioTracks(tv->model()) <= lanesBefore + 1,
		   "and they did not each make a lane of their own");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
