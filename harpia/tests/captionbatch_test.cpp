// Styling a whole caption lane at once must never touch what the captions say.
//
// With a lane's header selected, the Inspector edits every caption on it --
// font, size, colour. Its text box is hidden then, but it holds the FIRST
// caption's words, and the quick-tag buttons (B, I, U, ...) stayed on screen
// and typed into it: one click and every caption said what the first one did.
// Generated subtitles came out as "Engine start, no" six times over.
//
//   * Lane selected: the quick tags are hidden (there is no one caption's text
//     to wrap words in).
//   * Even a tag pressed anyway leaves every caption its own words.
//   * Lane styling still reaches every caption.
//   * One caption selected: the quick tags are back and edit that one only.
//   * Several captions selected (Ctrl+click): the Inspector shows the clicked
//     one, and its style controls -- size, bold, colour -- reach every
//     selected caption, as one change; their words stay their own, and the
//     words box edits the clicked one only. A note says which is which.
//   * And they move, scale and turn together: a drag in the preview, a scale
//     grip, or the Transform row gives each the clicked one's change -- the
//     same offset, the same factor -- so the group keeps its layout.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/ComponentPanel.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/tts/TtsAudio.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
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

static void click(QWidget *w, QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(QPointF(p)), Qt::LeftButton,
			  Qt::LeftButton, mods);
	QApplication::sendEvent(w, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(QPointF(p)), Qt::LeftButton,
			    Qt::NoButton, mods);
	QApplication::sendEvent(w, &release);
}

// The captions on `lane`, in time order.
static QStringList captionsOn(const TimelineView *tv, int lane)
{
	QStringList out;
	for (const TlClip &c : tv->model().tracks[lane].clips)
		if (c.type == TlClip::Type::Text)
			out << c.text.text;
	return out;
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

	// Something to caption: 3 s of tone and nine words, three per caption.
	const QString wav = tmp.filePath(QStringLiteral("speech.wav"));
	{
		tts_audio::Pcm p;
		p.samples.resize(48000 * 3 * 2);
		for (int i = 0; i < 48000 * 3; ++i)
			p.samples[2 * i] = p.samples[2 * i + 1] = qint16(8000 * std::sin(i * 2 * M_PI * 220 / 48000.0));
		tts_audio::writeWav16(wav, p);
	}
	const QStringList said = {QStringLiteral("Engine"), QStringLiteral("start,"), QStringLiteral("no"),
				  QStringLiteral("wait"),   QStringLiteral("for"),    QStringLiteral("the"),
				  QStringLiteral("green"),  QStringLiteral("light"),  QStringLiteral("now.")};
	QVector<ClipWordTime> words;
	for (int i = 0; i < said.size(); ++i) {
		ClipWordTime w;
		w.text = said[i];
		w.startMs = 100 + i * 300;
		w.endMs = w.startMs + 250;
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
	tv->setPlayhead(0);
	GroupRule rule;
	rule.maxWords = 3;
	w.addSpeechClips(wav, 3000, words, rule, SubtitleLook());
	settle(300);
	{
		// Generated subtitles are not linked to anything: unlink these, so a
		// click picks one caption rather than the narration's whole group.
		TimelineModel m = tv->model();
		for (TlTrack &t : m.tracks)
			for (TlClip &c : t.clips)
				c.link = 0;
		tv->setModel(m);
		settle(100);
	}

	int lane = -1;
	for (int t = 0; t < tv->model().tracks.size() && lane < 0; ++t)
		for (const TlClip &c : tv->model().tracks[t].clips)
			if (c.type == TlClip::Type::Text)
				lane = t;
	ok(lane >= 0, "a lane of captions");
	if (lane < 0)
		return 1;
	const QStringList before = captionsOn(tv, lane);
	std::printf("     captions: %s\n", qPrintable(before.join(QStringLiteral(" | "))));
	ok(before.size() == 3 && before.first() == QStringLiteral("Engine start, no"), "three different captions");

	QPushButton *bold = nullptr;
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("B") && b->toolTip().startsWith(QStringLiteral("Bold the selection")))
			bold = b;
	QSpinBox *size = nullptr;
	for (QSpinBox *s : w.findChildren<QSpinBox *>())
		if (s->minimum() == 6 && s->maximum() == 400)
			size = s;
	ok(bold && size, "the Inspector's quick tags and text size");
	if (!bold || !size)
		return 1;

	std::printf("\n-- the whole lane selected --\n");
	{
		const QRect toggle = tv->headerToggleRectForTest(lane, 0);
		click(tv, QPoint(tv->contentRectForTest().x() - 6, toggle.top() - 12));
		settle(200);
		ok(tv->selectedHeaderTrack() == lane && tv->selectedPairs().isEmpty(), "its header is selected, no clip");
		// Their own row, not the button: the Inspector tab around them may
		// not be on screen in a test window at all.
		ok(bold->parentWidget()->isHidden(), "the quick tags are hidden: there is no one caption's text to tag");

		// What the user did: a tag pressed while the lane was selected.
		bold->click();
		settle(100);
		const QStringList after = captionsOn(tv, lane);
		std::printf("     after B: %s\n", qPrintable(after.join(QStringLiteral(" | "))));
		ok(after == before, "even pressed anyway, every caption keeps its own words");

		size->setValue(77);
		settle(100);
		bool all = true;
		for (const TlClip &c : tv->model().tracks[lane].clips)
			if (c.type == TlClip::Type::Text)
				all = all && c.text.fontPx == 77;
		ok(all, "styling the lane still reaches every caption");
		ok(captionsOn(tv, lane) == before, "and still leaves their words alone");
	}

	std::printf("\n-- one caption selected --\n");
	{
		const QRect second = tv->clipRectForTest(lane, 1);
		click(tv, second.center());
		settle(200);
		ok(tv->selectedTrack() == lane && tv->selectedHeaderTrack() < 0, "the second caption is selected");
		ok(!bold->parentWidget()->isHidden(), "the quick tags are back");
		QPlainTextEdit *box = nullptr;
		for (QPlainTextEdit *e : w.findChildren<QPlainTextEdit *>())
			if (!e->isHidden() && e->toPlainText() == before[1])
				box = e;
		ok(box != nullptr, "the text box holds that caption's words");
		if (box) {
			QTextCursor cur = box->textCursor();
			cur.select(QTextCursor::Document);
			box->setTextCursor(cur);
			box->setFocus();
			bold->click();
			settle(100);
		}
		const QStringList after = captionsOn(tv, lane);
		std::printf("     after B: %s\n", qPrintable(after.join(QStringLiteral(" | "))));
		ok(after[1] == QStringLiteral("<b>%1</b>").arg(before[1]), "B bolds that caption");
		ok(after[0] == before[0] && after[2] == before[2], "and only that one");
	}

	std::printf("\n-- several captions selected --\n");
	{
		const QStringList words = captionsOn(tv, lane);
		click(tv, tv->clipRectForTest(lane, 0).center());
		settle(150);
		click(tv, tv->clipRectForTest(lane, 2).center(), Qt::ControlModifier);
		settle(200);
		ok(tv->selectedPairs().size() == 2 && tv->selectedTrack() == lane && tv->selectedClip() == 2,
		   "the first and third captions are selected, the third clicked last");
		QLabel *note = w.findChild<QLabel *>(QStringLiteral("textBatchNote"));
		ok(note && !note->isHidden() && note->text().startsWith(QStringLiteral("2 texts selected")),
		   "the Text section says its style reaches both");

		const int sizeOfSecond = tv->model().tracks[lane].clips[1].text.fontPx;
		size->setValue(55);
		settle(100);
		const auto &cs = tv->model().tracks[lane].clips;
		ok(cs[0].text.fontPx == 55 && cs[2].text.fontPx == 55, "a new size: both selected captions take it");
		ok(cs[1].text.fontPx == sizeOfSecond, "the one not selected keeps its own");
		QCheckBox *boldBox = nullptr;
		for (QCheckBox *b : w.findChildren<QCheckBox *>())
			if (b->text() == QStringLiteral("Bold"))
				boldBox = b;
		if (boldBox)
			boldBox->click();
		settle(100);
		const auto &cb = tv->model().tracks[lane].clips;
		ok(boldBox && cb[0].text.bold == cb[2].text.bold && cb[0].text.bold != cb[1].text.bold,
		   "Bold: both selected, not the other");
		ok(captionsOn(tv, lane) == words, "and every caption still says what it said");

		QPlainTextEdit *box = nullptr;
		for (QPlainTextEdit *e : w.findChildren<QPlainTextEdit *>())
			if (!e->isHidden() && e->toPlainText() == words[2])
				box = e;
		ok(box != nullptr, "the words box holds the clicked caption's words");
		if (box)
			box->setPlainText(QStringLiteral("Changed words"));
		settle(100);
		const QStringList now = captionsOn(tv, lane);
		ok(now[2] == QStringLiteral("Changed words") && now[0] == words[0] && now[1] == words[1],
		   "and edits that caption's words only");

		std::printf("\n-- several captions, moved and scaled together --\n");
		{
			// Apart first: the first high, the third low.
			TimelineModel m = tv->model();
			m.tracks[lane].clips[0].posY = 0.2;
			m.tracks[lane].clips[2].posY = 0.8;
			tv->setModel(m);
			settle(100);
			click(tv, tv->clipRectForTest(lane, 0).center());
			settle(150);
			click(tv, tv->clipRectForTest(lane, 2).center(), Qt::ControlModifier);
			settle(200);
			tv->setPlayhead(tv->model().tracks[lane].clips[2].outStartMs + 50);
			settle(100);
		}
		PreviewCanvas *canvas = w.findChild<PreviewCanvas *>();
		ok(canvas != nullptr, "the preview");
		if (canvas) {
			const double x1 = tv->model().tracks[lane].clips[1].posX;
			const double y1 = tv->model().tracks[lane].clips[1].posY;
			emit canvas->transformDragged(0.0, -0.05); // the clicked one, dragged up
			emit canvas->transformEditFinished();
			settle(150);
			const auto &mc = tv->model().tracks[lane].clips;
			std::printf("     after the drag: %.3f  (%.3f)  %.3f\n", mc[0].posY, mc[1].posY, mc[2].posY);
			ok(std::abs(mc[2].posY - 0.75) < 1e-6 && std::abs(mc[0].posY - 0.15) < 1e-6,
			   "a drag in the preview moves both by the same amount: still apart");
			ok(mc[1].posY == y1 && mc[1].posX == x1, "the one not selected stays put");

			const double s0 = mc[0].scale, s2 = mc[2].scale;
			emit canvas->transformScaled(1.5, 0.5, 0.5);
			emit canvas->transformEditFinished();
			settle(150);
			const auto &ms = tv->model().tracks[lane].clips;
			ok(std::abs(ms[2].scale - s2 * 1.5) < 1e-6 && std::abs(ms[0].scale - s0 * 1.5) < 1e-6,
			   "a scale grip: both grow by the same factor");
		}
		ComponentPanel *panel = w.findChild<ComponentPanel *>();
		ok(panel != nullptr, "the component list");
		if (panel) {
			const double y0 = tv->model().tracks[lane].clips[0].posY;
			const double y2 = tv->model().tracks[lane].clips[2].posY;
			emit panel->pinnedEdited(QStringLiteral("harpia.transform"), QStringLiteral("posY"), y2 + 0.1);
			settle(150);
			const auto &mp = tv->model().tracks[lane].clips;
			ok(std::abs(mp[2].posY - (y2 + 0.1)) < 1e-6 && std::abs(mp[0].posY - (y0 + 0.1)) < 1e-6,
			   "Transform > Position: the clicked one takes the value, the other the same change");
			const double sc0 = mp[0].scale, sc2 = mp[2].scale;
			emit panel->pinnedEdited(QStringLiteral("harpia.transform"), QStringLiteral("scale"), sc2 * 2.0);
			settle(150);
			const auto &mz = tv->model().tracks[lane].clips;
			ok(std::abs(mz[2].scale - sc2 * 2.0) < 1e-6 && std::abs(mz[0].scale - sc0 * 2.0) < 1e-6,
			   "Transform > Scale: twice as big, both");
		}

		click(tv, tv->clipRectForTest(lane, 1).center());
		settle(200);
		ok(note && note->isHidden(), "one caption clicked on its own: the note goes");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
