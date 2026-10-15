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
//   * The narration keeps how it was made (TlClip::tts) and lands where it
//     is asked to (Fit to selection: at the selection's start).
//   * Re-speaking one sentence: the sentence as its captions say it now
//     (an edited caption included), the new take spliced into a new file,
//     its captions rebuilt, the later ones moved by the difference, all
//     still linked -- and one Ctrl+Z brings the old take back.
//   * Fit to selection follows the timeline's selection; ▶ says the text's
//     first sentence.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/tts/TtsAudio.hpp"
#include "editor/tts/TtsDialog.hpp"
#include "editor/tts/TtsProviders.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QToolButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
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

	// A narration of two sentences, 4 s, at 10 s, made "with OpenAI's marin".
	const QString wav2 = tmp.filePath(QStringLiteral("two sentences.wav"));
	{
		tts_audio::Pcm p;
		p.samples.resize(48000 * 4 * 2);
		for (int i = 0; i < 48000 * 4; ++i)
			p.samples[2 * i] = p.samples[2 * i + 1] = qint16(6000 * std::sin(i * 2 * M_PI * 330 / 48000.0));
		tts_audio::writeWav16(wav2, p);
	}
	QVector<ClipWordTime> words2;
	const QStringList said2 = {QStringLiteral("Olá"),   QStringLiteral("pessoal,"), QStringLiteral("hoje"),
				   QStringLiteral("vamos"), QStringLiteral("lavar."),   QStringLiteral("As"),
				   QStringLiteral("mãos"),  QStringLiteral("direito.")};
	for (int i = 0; i < said2.size(); ++i) {
		ClipWordTime wt;
		wt.text = said2[i];
		wt.startMs = (i < 5 ? 100 : 700) + i * 320;
		wt.endMs = wt.startMs + 280;
		words2.append(wt);
	}
	TtsSpec spec;
	spec.voice = QStringLiteral("marin");
	spec.language = QStringLiteral("pt");

	std::printf("\n-- how it was made, and where --\n");
	int link2 = 0;
	{
		w.addSpeechClips(wav2, 4000, words2, rule, look, spec.toJson(), 10000);
		settle(300);
		const TlClip *a = nullptr;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &c : t.clips)
				if (!c.tts.isEmpty() && c.outStartMs == 10000) {
					a = &c;
					link2 = c.link;
				}
		ok(a != nullptr, "placed where asked (Fit to selection: the selection's start), not at the playhead");
		bool good = false;
		ok(a && TtsSpec::fromJson(a->tts, &good).voice == QLatin1String("marin") && good,
		   "and keeps how it was made, for re-speaking");
	}

	// The captions of that narration, in time order: (track, clip).
	const auto captions = [&]() {
		QVector<QPair<int, int>> out;
		for (const auto &p : tv->model().linkedClips(link2))
			if (tv->model().tracks[p.first].clips[p.second].type == TlClip::Type::Text)
				out.append(p);
		std::sort(out.begin(), out.end(), [&](const QPair<int, int> &x, const QPair<int, int> &y) {
			return tv->model().tracks[x.first].clips[x.second].outStartMs <
			       tv->model().tracks[y.first].clips[y.second].outStartMs;
		});
		return out;
	};
	const auto capText = [&](const QPair<int, int> &p) { return tv->model().tracks[p.first].clips[p.second].text.text; };
	const auto capAt = [&](const QPair<int, int> &p) { return tv->model().tracks[p.first].clips[p.second].outStartMs; };

	std::printf("\n-- re-speak one sentence --\n");
	{
		QVector<QPair<int, int>> caps = captions();
		QStringList texts;
		for (const auto &p : caps)
			texts << capText(p);
		std::printf("     captions: %s\n", qPrintable(texts.join(QStringLiteral(" | "))));
		ok(caps.size() == 3, "three captions");
		int link = 0;
		QString why;
		ok(w.narrationSentenceAt(caps[2].first, caps[2].second, 0, &link, &why) == QStringLiteral("As mãos direito.") &&
			   link == link2,
		   "right-clicked on the last caption: its sentence");
		// The user edits a caption's words first.
		{
			TimelineModel m = tv->model();
			m.tracks[caps[1].first].clips[caps[1].second].text.text = QStringLiteral("vamos limpar.");
			tv->setModelAndCommit(m);
			settle(100);
		}
		const QString sentence = w.narrationSentenceAt(caps[0].first, caps[0].second, 0, &link, &why);
		ok(sentence == QStringLiteral("Olá pessoal, hoje vamos limpar."), "the first sentence, with the edited words");

		// The new take, 2.5 s, as the service would give it.
		const QString take = tmp.filePath(QStringLiteral("take.wav"));
		tts_audio::Pcm tp;
		tp.samples.resize(48000 * 5 / 2 * 2);
		for (qsizetype i = 0; i < tp.samples.size(); ++i)
			tp.samples[i] = 4000;
		tts_audio::writeWav16(take, tp);
		QVector<ClipWordTime> tw;
		const QStringList newWords = {QStringLiteral("Olá"), QStringLiteral("pessoal,"), QStringLiteral("hoje"),
					      QStringLiteral("vamos"), QStringLiteral("limpar.")};
		for (int i = 0; i < newWords.size(); ++i) {
			ClipWordTime wt;
			wt.text = newWords[i];
			wt.startMs = 150 + i * 440;
			wt.endMs = wt.startMs + 400;
			tw.append(wt);
		}
		const qint64 laterBefore = capAt(caps[2]);
		int srcBefore = 0;
		qint64 lenBefore = 0;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &c : t.clips)
				if (c.link == link2 && !c.tts.isEmpty()) {
					srcBefore = c.sourceId;
					lenBefore = c.outDurationMs();
				}
		ok(w.applyRespoken(link2, sentence, take, tw, &why), "the new take goes in");
		if (!why.isEmpty())
			std::printf("     %s\n", qPrintable(why));
		settle(200);
		caps = captions();
		texts.clear();
		for (const auto &p : caps)
			texts << capText(p);
		std::printf("     captions now: %s\n", qPrintable(texts.join(QStringLiteral(" | "))));
		ok(texts.join(QLatin1Char(' ')) == QStringLiteral("Olá pessoal, hoje vamos limpar. As mãos direito."),
		   "its captions say the new words; the other sentence's are kept");
		int srcAfter = 0;
		qint64 lenAfter = 0;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &c : t.clips)
				if (c.link == link2 && !c.tts.isEmpty()) {
					srcAfter = c.sourceId;
					lenAfter = c.outDurationMs();
				}
		const qint64 grew = lenAfter - lenBefore;
		std::printf("     narration %lld -> %lld ms; the later caption %lld -> %lld\n", (long long)lenBefore,
			    (long long)lenAfter, (long long)laterBefore, (long long)capAt(caps.last()));
		ok(srcAfter != srcBefore && grew > 300, "a new, longer narration file");
		ok(QFileInfo::exists(tmp.filePath(QStringLiteral("two sentences (re-spoken 1).wav"))),
		   "saved beside the old one, which stays");
		ok(std::llabs(capAt(caps.last()) - laterBefore - grew) <= 2, "the later sentence moved by the difference");
		ok(tv->model().linkedClips(link2).size() == 1 + caps.size(), "everything still linked");

		QShortcut *undo = nullptr;
		for (QShortcut *sc : w.findChildren<QShortcut *>())
			if (sc->key() == QKeySequence(Qt::CTRL | Qt::Key_Z) && sc->isEnabled())
				undo = sc;
		if (undo) {
			emit undo->activated();
			settle(300);
			int srcUndone = 0;
			for (const TlTrack &t : tv->model().tracks)
				for (const TlClip &c : t.clips)
					if (c.link == link2 && !c.tts.isEmpty())
						srcUndone = c.sourceId;
			texts.clear();
			for (const auto &p : captions())
				texts << capText(p);
			ok(srcUndone == srcBefore && texts.join(QLatin1Char(' ')) ==
							     QStringLiteral("Olá pessoal, hoje vamos limpar. As mãos direito."),
			   "one Ctrl+Z: the old take back (with the edited caption, the step before)");
		}
	}

	std::printf("\n-- fit to selection, and the sample line --\n");
	{
		int vt = -1;
		for (int t = 0; t < tv->model().tracks.size(); ++t)
			for (const TlClip &c : tv->model().tracks[t].clips)
				if (c.type == TlClip::Type::Video && c.tts.isEmpty() && tv->model().tracks[t].kind == TlTrack::Kind::Video)
					vt = t;
		TlSelection sel;
		sel.track = vt;
		sel.clip = 0;
		tv->setSelectionState(sel);
		settle(100);
		TtsDialog *d = w.findChild<TtsDialog *>();
		QCheckBox *fit = nullptr;
		if (d)
			for (QCheckBox *c : d->findChildren<QCheckBox *>())
				if (c->text().startsWith(QLatin1String("Fit to the selected clips")))
					fit = c;
		ok(fit && fit->isEnabled() && fit->text().contains(QLatin1String("0:04.0")),
		   "Fit to selection knows the selected clip: 4 s");
		if (fit)
			std::printf("     %s\n", qPrintable(fit->text()));
		tv->setSelectionState(TlSelection());
		settle(100);
		ok(fit && !fit->isEnabled(), "and is off with nothing selected");
		ok(TtsDialog::previewSentence(QStringLiteral("Olá pessoal! Tudo bem? [pause 1s] Mais."), QStringLiteral("pt")) ==
			   QStringLiteral("Olá pessoal!"),
		   "▶ says the text's first sentence");
		ok(TtsDialog::previewSentence(QString(), QStringLiteral("pt")).startsWith(QStringLiteral("Olá!")),
		   "or, with no text yet, a line in the language");
	}

	std::printf("\n-- the marks toolbar --\n");
	{
		TtsDialog *d = w.findChild<TtsDialog *>();
		ok(d != nullptr, "the Text to speech window");
		if (d) {
			d->show();
			settle(100);
			auto *edit = d->findChild<QPlainTextEdit *>();
			const auto action = [&](const QString &label) -> QAction * {
				for (QAction *a : d->findChildren<QAction *>())
					if (a->text().section(QStringLiteral("  "), 0, 0) == label)
						return a;
				return nullptr;
			};
			QComboBox *service = nullptr, *model = nullptr;
			for (QComboBox *c : d->findChildren<QComboBox *>()) {
				if (c->findData(QStringLiteral("deepgram")) >= 0)
					service = c;
				if (c->toolTip().startsWith(QLatin1String("Multilingual v2 is steady")))
					model = c; // filled while ElevenLabs is the service
			}
			ok(edit && service && model && action(QStringLiteral("Slower")) && action(QStringLiteral("Whisper")),
			   "Pace, Mood, ... over the text; a model picker for ElevenLabs");
			if (edit && service && model) {
				service->setCurrentIndex(service->findData(QStringLiteral("openai")));
				settle(50);
				d->setText(QStringLiteral("Hoje vamos lavar as mãos."));
				QTextCursor c = edit->textCursor();
				c.setPosition(5);
				c.setPosition(16, QTextCursor::KeepAnchor); // "vamos lavar"
				edit->setTextCursor(c);
				action(QStringLiteral("Slower"))->trigger();
				ok(d->text() == QStringLiteral("Hoje [slow]vamos lavar[/slow] as mãos."), "Slower wraps the selected words");
				c = edit->textCursor();
				c.setPosition(int(d->text().size()));
				edit->setTextCursor(c);
				for (QToolButton *b : d->findChildren<QToolButton *>())
					if (b->text().startsWith(QLatin1String("Pause")) && b->menu())
						for (QAction *a : b->menu()->actions())
							if (a->text() == QLatin1String("1s"))
								a->trigger();
				ok(d->text().endsWith(QStringLiteral("mãos. [pause 1s] ")), "Pause puts [pause 1s] where the cursor is");

				ok(action(QStringLiteral("Whisper"))->isEnabled() && action(QStringLiteral("Whisper"))->text().contains(QChar(0x2248)),
				   "OpenAI: Whisper, done another way (≈)");
				service->setCurrentIndex(service->findData(QStringLiteral("deepgram")));
				settle(50);
				ok(!action(QStringLiteral("Whisper"))->isEnabled() && action(QStringLiteral("Slower"))->isEnabled(),
				   "Deepgram: Whisper greyed, pace still there");
				ok(action(QStringLiteral("Whisper"))->toolTip().contains(QLatin1String("Deepgram")), "and it says why");
				service->setCurrentIndex(service->findData(QStringLiteral("elevenlabs")));
				settle(50);
				ok(model->isVisible(), "ElevenLabs: the model picker shows");
				model->setCurrentIndex(model->findData(QStringLiteral("eleven_multilingual_v2")));
				settle(50);
				const bool v2 = !action(QStringLiteral("Laugh"))->isEnabled();
				model->setCurrentIndex(model->findData(QStringLiteral("eleven_v4")));
				settle(50);
				ok(v2 && action(QStringLiteral("Laugh"))->isEnabled(), "Laugh: not on v2, yes on v4");
				ok(d->spec().model == QLatin1String("eleven_v4"), "and the narration is made with that model");

				for (QToolButton *b : d->findChildren<QToolButton *>())
					if (b->text() == QLatin1String("Clear")) {
						edit->moveCursor(QTextCursor::Start);
						b->click();
					}
				ok(d->text() == QStringLiteral("Hoje vamos lavar as mãos.  "), "Clear marks takes them all out");
			}
			d->close();
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
