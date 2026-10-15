// Text to speech in the real editor window, without a network: a finished
// narration (a WAV and its words' times, as the Text to speech window hands
// it over) lands on the timeline as an audio clip and caption clips, LINKED.
//
//   * The narration at the playhead, on an audio lane; captions saying the
//     words, inside its span, on a Subtitles lane.
//   * All of them share one link id and are selected together.
//   * Clicking one caption selects the whole group (move one, move all).
//   * One Ctrl+Z takes all of it away.
//   * Add > Text to speech… opens the window; a .srt opens as its words.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/tts/TtsAudio.hpp"
#include "editor/tts/TtsDialog.hpp"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QTemporaryDir>
#include <QThread>

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

static void click(QWidget *w, QPoint p)
{
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(QPointF(p)), Qt::LeftButton,
			  Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(QPointF(p)), Qt::LeftButton,
			    Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &release);
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString video = media + QStringLiteral("/av_red.mp4");
	if (!QFile::exists(video)) {
		std::printf("missing %s\n", qPrintable(video));
		return 2;
	}
	QTemporaryDir tmp;

	// A 3 s "narration": a tone, 48 kHz stereo, and eight words over it.
	const QString wav = tmp.filePath(QStringLiteral("narration.wav"));
	{
		tts_audio::Pcm p;
		p.samples.resize(48000 * 3 * 2);
		for (int i = 0; i < 48000 * 3; ++i)
			p.samples[2 * i] = p.samples[2 * i + 1] = qint16(8000 * std::sin(i * 2 * M_PI * 220 / 48000.0));
		tts_audio::writeWav16(wav, p);
	}
	QVector<ClipWordTime> words;
	const QStringList said = {QStringLiteral("Olá"), QStringLiteral("pessoal,"), QStringLiteral("hoje"),
				  QStringLiteral("vamos"), QStringLiteral("lavar"), QStringLiteral("as"),
				  QStringLiteral("mãos"), QStringLiteral("direito.")};
	for (int i = 0; i < said.size(); ++i) {
		ClipWordTime w;
		w.text = said[i];
		w.startMs = 100 + i * 350;
		w.endMs = w.startMs + 300;
		words.append(w);
	}

	VideoEditorWindow w(video);
	w.resize(1200, 800);
	w.show();
	settle(300);
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Full Editing")) {
			b->click();
			settle(300);
			break;
		}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "in Full editing");
	if (!tv)
		return 1;
	tv->setPlayhead(500);
	const int lanesBefore = int(tv->model().tracks.size());

	std::printf("\n-- the narration lands, linked --\n");
	GroupRule rule;
	rule.maxWords = 3;
	SubtitleLook look;
	w.addSpeechClips(wav, 3000, words, rule, look);
	settle(300);
	const TimelineModel &m = tv->model();
	// (Right after adding: a selection change is an undo step of its own.)
	int link = 0;
	const TlClip *narration = nullptr;
	for (int t = 0; t < m.tracks.size(); ++t)
		for (const TlClip &c : m.tracks[t].clips)
			if (m.tracks[t].kind == TlTrack::Kind::Audio && c.link != 0) {
				narration = &c;
				link = c.link;
			}
	ok(narration != nullptr, "an audio clip");
	if (!narration)
		return 1;
	ok(narration->outStartMs == 500 && std::abs(narration->outDurationMs() - 3000) <= 25,
	   "at the playhead, 3 s long");
	QVector<QPair<int, int>> group = m.linkedClips(link);
	QStringList captionText;
	bool inside = true;
	for (const auto &p : group) {
		const TlClip &c = m.tracks[p.first].clips[p.second];
		if (c.type != TlClip::Type::Text)
			continue;
		captionText << c.text.text;
		inside = inside && c.outStartMs >= narration->outStartMs && c.outEndMs() <= narration->outEndMs();
	}
	std::printf("     %d lanes -> %d; captions: %s\n", lanesBefore, int(m.tracks.size()),
		    qPrintable(captionText.join(QStringLiteral(" | "))));
	ok(captionText.size() == 3, "three captions of at most three words");
	ok(captionText.join(QLatin1Char(' ')) == said.join(QLatin1Char(' ')), "saying the text as written");
	ok(inside, "each inside the narration's span");
	ok(group.size() == 4, "the narration and its captions share one link");
	ok(tv->selectedPairs().size() == 4, "and arrive selected together");

	std::printf("\n-- one undo --\n");
	{
		QShortcut *undo = nullptr;
		for (QShortcut *s : w.findChildren<QShortcut *>())
			if (s->key() == QKeySequence(Qt::CTRL | Qt::Key_Z) && s->isEnabled())
				undo = s;
		ok(undo != nullptr, "Ctrl+Z is there");
		if (undo) {
			emit undo->activated();
			settle(300);
			ok(tv->model().linkedClips(link).isEmpty(), "takes the narration and all its captions away");
		}
	}

	std::printf("\n-- click one, get the group --\n");
	{
		// Again (the undo above took it away), then nothing selected.
		w.addSpeechClips(wav, 3000, words, rule, look);
		settle(300);
		link = 0;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &c : t.clips)
				if (t.kind == TlTrack::Kind::Audio && c.link != 0)
					link = c.link;
		group = tv->model().linkedClips(link);
		tv->setSelectionState(TlSelection());
		int capTrack = -1, capClip = -1;
		for (const auto &p : group)
			if (m.tracks[p.first].clips[p.second].type == TlClip::Type::Text) {
				capTrack = p.first;
				capClip = p.second;
			}
		click(tv, tv->clipRectForTest(capTrack, capClip).center());
		settle(100);
		ok(tv->selectedPairs().size() == 4, "clicking a caption selects the narration and every caption");
	}

	std::printf("\n-- the window --\n");
	{
		QAction *act = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text() == QStringLiteral("Text to speech…"))
				act = a;
		ok(act != nullptr, "Add > Text to speech…");
		if (act) {
			act->trigger();
			settle(200);
			TtsDialog *d = w.findChild<TtsDialog *>();
			ok(d && d->isVisible(), "opens the Text to speech window");
			if (d)
				d->close();
		}
		const QString srt = tmp.filePath(QStringLiteral("a.srt"));
		QFile f(srt);
		f.open(QIODevice::WriteOnly);
		f.write("1\n00:00:01,000 --> 00:00:02,000\nOl\xc3\xa1 pessoal\n\n2\n00:00:02,000 --> 00:00:03,000\ntudo bem?\n");
		f.close();
		QString err;
		ok(TtsDialog::readTextFile(srt, &err) == QStringLiteral("Olá pessoal tudo bem?"),
		   "an .srt opens as its words, without numbers and times");
		const QString name = TtsDialog::fileNameFor(QStringLiteral("Olá, pessoal! Hoje vamos lavar as mãos."),
							    QDateTime(QDate(2026, 10, 15), QTime(14, 3, 22)));
		std::printf("     %s\n", qPrintable(name));
		ok(name == QStringLiteral("2026-10-15 14-03-22 Olá pessoal Hoje vamos lavar.wav"),
		   "a narration is named by its time and first words");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
