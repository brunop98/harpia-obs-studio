// Simple Trim and Multi-Cut must show the requested frame immediately, even
// when a late in-point lies more than 240 frames beyond the previous keyframe.
// Numbered video frames expose visible decode catch-up and timing mismatches.
#include "editor/EditorWidgets.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/TrackEditor.hpp"
#include "editor/VideoEditorWindow.hpp"

#include <QApplication>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

static int counterOf(const QImage &image)
{
	if (image.isNull() || image.width() != 64 || image.height() != 64)
		return -1;
	const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
	int count = 0;
	for (int bit = 0; bit < 8; ++bit)
		if (qGray(rgb.pixel(bit * 8 + 4, 32)) > 128)
			count |= 1 << bit;
	return count;
}

static bool invoke(VideoEditorWindow &editor, const char *method)
{
	return QMetaObject::invokeMethod(&editor, method, Qt::DirectConnection);
}

static void stopPlaybackTimer(VideoEditorWindow &editor)
{
	for (QTimer *timer : editor.findChildren<QTimer *>())
		if (timer->isActive() && timer->interval() == 33)
			timer->stop();
}

static bool matches(PreviewCanvas &canvas, FrameSeeker &reference, qint64 sourceMs)
{
	const int actual = counterOf(canvas.currentFrame());
	const int expected = counterOf(reference.frameAt(sourceMs, 64, 64));
	if (actual != expected || actual < 0) {
		std::printf("     source time %lld ms: frame %d, wanted %d\n",
			    (long long)sourceMs, actual, expected);
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	const QString media = scratch.filePath(QStringLiteral("long-gop-counter.mp4"));
	QProcess ff;
	ff.start(QStringLiteral("ffmpeg"), {
		QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		QStringLiteral("nullsrc=s=64x64:r=30:d=12,format=yuv420p,"
				 "geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128"),
		QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"),
		QStringLiteral("-g"), QStringLiteral("600"), QStringLiteral("-keyint_min"), QStringLiteral("600"),
		QStringLiteral("-sc_threshold"), QStringLiteral("0"), QStringLiteral("-crf"), QStringLiteral("8"),
		QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), media});
	if (!ff.waitForFinished(30000) || ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0) {
		std::printf("could not generate the long-GOP numbered video\n");
		return 2;
	}
	VideoEditorWindow editor(media);
	auto *trim = editor.findChild<Timeline *>();
	auto *cuts = editor.findChild<TrackEditor *>();
	auto *canvas = editor.findChild<PreviewCanvas *>();
	FrameSeeker reference;
	ok(editor.isValid() && trim && cuts && canvas && reference.open(media),
	   "the real editor and independent reference decoder open the numbered source");
	if (!trim || !cuts || !canvas || !reference.isOpen())
		return 1;
	constexpr qint64 start = 11000;
	constexpr qint64 end = 11500;

	std::printf("\n-- Simple Trim, a late in-point in a long GOP --\n");
	trim->setEnd(end);
	trim->setStart(start);
	trim->setPlayhead(start);
	ok(invoke(editor, "onPlayPause"), "Simple Trim starts through the production Play action");
	stopPlaybackTimer(editor);
	ok(matches(*canvas, reference, start), "Simple Trim immediately shows its actual in-point");
	bool accurate = true;
	for (int sample = 0; sample < 6; ++sample) {
		QThread::msleep(20);
		invoke(editor, "onPlayTick");
		const qint64 at = trim->playhead();
		accurate = at >= start && at < end && matches(*canvas, reference, at) && accurate;
	}
	ok(accurate && trim->playhead() >= start + 60,
	   "Simple Trim advances from its in-point without showing earlier catch-up footage");
	invoke(editor, "onPlayPause");
	ok(trim->start() == start && trim->end() == end, "Simple Trim playback preserves the trim bounds");

	std::printf("\n-- Multi-Cut, the same late source range --\n");
	const QVector<CutSegment> segments = {{start, end, 1.0, editor.activeSourceIdForTest()}};
	cuts->setSegments(segments);
	cuts->setPlayhead(0);
	bool switched = false;
	for (QPushButton *button : editor.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Multi-Cut")) {
			button->click();
			switched = true;
			break;
		}
	ok(switched, "Multi-Cut is selected through the real mode control");
	ok(invoke(editor, "onPlayPause"), "Multi-Cut starts through the production Play action");
	stopPlaybackTimer(editor);
	ok(matches(*canvas, reference, start), "Multi-Cut immediately shows the cut's actual source in-point");
	accurate = true;
	for (int sample = 0; sample < 6; ++sample) {
		QThread::msleep(20);
		invoke(editor, "onPlayTick");
		const qint64 outMs = cuts->playhead();
		accurate = outMs >= 0 && outMs < end - start &&
			   matches(*canvas, reference, start + outMs) && accurate;
	}
	ok(accurate && cuts->playhead() >= 60,
	   "Multi-Cut advances at the timeline cadence without showing earlier catch-up footage");
	invoke(editor, "onPlayPause");
	ok(cuts->segments() == segments, "Multi-Cut preview leaves the source ranges and speeds unchanged");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
