// Closing an editor that was opened blank (the main window's "open the editor"
// with no file) and never given a video.
//
// Such an editor has no active decoder: seeker_ stays null until a video is
// added. The close-time "anything unsaved?" check read seeker_->durationMs()
// regardless, so closing one crashed -- before the Unsaved Changes prompt could
// appear, taking whatever was on the timeline with it. Reported from v0.1.420
// with an audio clip and generated subtitles on the timeline.
//
//   * Blank and untouched: closes, no prompt.
//   * Blank with an audio clip and captions on it (no video at all): the
//     prompt appears, and Cancel keeps the editor open with its work.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/tts/TtsAudio.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cmath>
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

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir tmp;

	std::printf("\n-- blank and untouched --\n");
	{
		auto *w = new VideoEditorWindow(QString());
		// Opening maximised: nothing asked of the window system before the
		// window is on screen (a maximised state set that early is what Windows
		// drew at the restore size, Restore on its title bar), then maximised
		// once it is showing.
		ok(!(w->windowState() & Qt::WindowMaximized) && !w->testAttribute(Qt::WA_WState_Created),
		   "before it is shown: no native window yet, and not asked to be maximised");
		w->show();
		// Remembered monitor placement and native layout can adjust the size;
		// the regression here is maximizing before the window is first shown.
		ok(!w->isMaximized() && w->size().isValid(), "shown as a restored window before maximizing");
		settle(300);
		std::printf("     then %s, %d x %d\n", w->isMaximized() ? "maximised" : "NOT maximised", w->width(),
			    w->height());
		ok(w->isMaximized(), "and maximised once it is on screen");
		ok(w->isVisible(), "a blank editor opens");
		const bool closed = w->close();
		settle(100);
		ok(closed && !w->isVisible(), "and closes without a crash, and without asking anything");
		delete w;
	}

	std::printf("\n-- blank, with only audio and captions on it --\n");
	{
		const QString wav = tmp.filePath(QStringLiteral("speech.wav"));
		tts_audio::Pcm p;
		p.samples.resize(48000 * 2 * 2);
		for (int i = 0; i < 48000 * 2; ++i)
			p.samples[2 * i] = p.samples[2 * i + 1] = qint16(8000 * std::sin(i * 2 * M_PI * 220 / 48000.0));
		tts_audio::writeWav16(wav, p);
		QVector<ClipWordTime> words;
		for (int i = 0; i < 4; ++i)
			words.append({QStringLiteral("word%1").arg(i), 100 + i * 400, 400 + i * 400});

		auto *w = new VideoEditorWindow(QString());
		w->resize(1200, 800);
		w->show();
		settle(300);
		w->addSpeechClips(wav, 2000, words, GroupRule(), SubtitleLook());
		settle(200);
		TimelineView *tv = w->findChild<TimelineView *>();
		ok(tv && !tv->model().isEmpty(), "an audio clip and its captions, and no video anywhere");

		// The prompt is modal: answer it from inside its own event loop.
		bool prompted = false;
		QTimer::singleShot(400, [&prompted]() {
			if (QWidget *modal = QApplication::activeModalWidget())
				for (QPushButton *b : modal->findChildren<QPushButton *>())
					if (b->text() == QStringLiteral("Cancel")) {
						prompted = true;
						b->click();
						return;
					}
		});
		const bool closed = w->close();
		settle(200);
		ok(prompted, "closing asks about the unsaved work instead of crashing");
		ok(!closed && w->isVisible(), "Cancel keeps the editor open");
		ok(tv && !tv->model().isEmpty(), "with its work still there");
		w->setAttribute(Qt::WA_DeleteOnClose, false);
		delete w;
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
