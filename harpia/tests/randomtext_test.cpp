// The two kinds of text, in the real editor window.
//
//   * Add → Random text puts a Random text on the timeline with two texts;
//     the Inspector calls it "Random text" and shows its other texts, each in
//     its own box. A plain Text shows none of that.
//   * "+ Add text" adds a box; a text can run over several lines; × removes
//     one; each is one undo step.
//   * The quick-tag buttons style the box being typed in, not just the first.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TextVariations.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QTextCursor>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QThread>

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

// The visible text boxes of the Random text section, in order (Text 2, 3…).
static QVector<QPlainTextEdit *> variantBoxes(QWidget &w)
{
	QVector<QPlainTextEdit *> out;
	for (QPlainTextEdit *e : w.findChildren<QPlainTextEdit *>())
		if (e->isVisible() && e->placeholderText().startsWith(QStringLiteral("Another text")))
			out.push_back(e);
	std::sort(out.begin(), out.end(), [](QPlainTextEdit *a, QPlainTextEdit *b) {
		return a->mapToGlobal(QPoint()).y() < b->mapToGlobal(QPoint()).y();
	});
	return out;
}

static QPushButton *button(QWidget &w, const QString &text)
{
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->isVisible() && b->text() == text)
			return b;
	return nullptr;
}

// The Text section's title: a section header (a button you click to fold it).
static bool headerSays(QWidget &w, const QString &text)
{
	for (QLabel *l : w.findChildren<QLabel *>())
		if (l->isVisible() && l->text() == text)
			return true;
	for (QAbstractButton *b : w.findChildren<QAbstractButton *>())
		if (b->isVisible() && b->text() == text)
			return true;
	return false;
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
	VideoEditorWindow w(video);
	w.resize(1400, 1000);
	w.show();
	w.activateWindow();
	settle(300);
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Full Editing")) {
			b->click();
			settle(300);
			break;
		}
	// The Inspector starts collapsed: open it, as the user would.
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->isCheckable() && b->text() == QStringLiteral("Inspector") && !b->isChecked()) {
			b->click();
			settle(300);
			break;
		}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "in Full editing");
	if (!tv)
		return 1;
	QShortcut *undo = nullptr;
	for (QShortcut *s : w.findChildren<QShortcut *>())
		if (s->key() == QKeySequence(Qt::CTRL | Qt::Key_Z) && s->isEnabled())
			undo = s;
	const auto selected = [&]() -> const TlClip * { return tv->selectedClipPtr(); };

	std::printf("\n-- Add → Random text --\n");
	QAction *addRandom = nullptr, *addText = nullptr;
	for (QAction *a : w.findChildren<QAction *>()) {
		if (a->text() == QStringLiteral("Random text"))
			addRandom = a;
		if (a->text() == QStringLiteral("Text"))
			addText = a;
	}
	ok(addRandom && addText, "the Add menu has both Text and Random text");
	if (!addRandom || !addText)
		return 1;
	addRandom->trigger();
	settle(400);
	const TlClip *c = selected();
	ok(c && c->type == TlClip::Type::Text && c->text.random, "a Random text is selected");
	ok(c && text_variations::optionsOf(c->text).size() == 2, "with two texts to begin with");
	ok(headerSays(w, QStringLiteral("Random text")), "the Inspector calls it Random text");
	ok(variantBoxes(w).size() == 1, "one box for Text 2");
	ok(button(w, QStringLiteral("+  Add text")) != nullptr, "and a + Add text button");

	std::printf("\n-- adding, several lines, removing --\n");
	if (QPushButton *add = button(w, QStringLiteral("+  Add text")))
		add->click();
	settle(300);
	QVector<QPlainTextEdit *> boxes = variantBoxes(w);
	ok(boxes.size() == 2 && selected()->text.variations.size() == 2, "+ adds a box");
	if (boxes.size() == 2) {
		boxes[1]->setPlainText(QStringLiteral("Line one\nline two"));
		settle(500);
	}
	ok(selected()->text.variations.value(1) == QStringLiteral("Line one\nline two"), "a text over two lines");
	ok(text_variations::optionsOf(selected()->text).size() == 3, "three versions now");
	ok(tv->model().tracks.size() > 0, "still on the timeline");

	std::printf("\n-- tags in the box being typed in --\n");
	boxes = variantBoxes(w);
	if (boxes.size() == 2) {
		boxes[0]->setFocus();
		settle(100);
		boxes[0]->selectAll();
		QPushButton *bold = button(w, QStringLiteral("B"));
		ok(bold != nullptr, "the B button");
		if (bold) {
			bold->click();
			settle(500);
		}
		std::printf("     Text 2 is now: %s\n", qPrintable(selected()->text.variations.value(0)));
		ok(selected()->text.variations.value(0) == QStringLiteral("<b>Your other text</b>"),
		   "B wrapped Text 2, not Text 1");
		ok(selected()->text.text == QStringLiteral("Your text"), "Text 1 untouched");
	}

	std::printf("\n-- typing into a box that is not the one previewed --\n");
	boxes = variantBoxes(w);
	if (boxes.size() == 2) {
		boxes[1]->setFocus(); // previews Text 3
		settle(200);
		boxes[0]->setFocus();
		QTextCursor cur = boxes[0]->textCursor();
		cur.movePosition(QTextCursor::End);
		boxes[0]->setTextCursor(cur);
		QKeyEvent k(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
		QApplication::sendEvent(boxes[0], &k);
		settle(300);
		std::printf("     Text 2 is now: %s\n", qPrintable(selected()->text.variations.value(0)));
		ok(selected()->text.variations.value(0).endsWith(QLatin1Char('x')), "the keystroke is kept");
	}

	std::printf("\n-- × removes one, Ctrl+Z puts it back --\n");
	{
		QPushButton *del = nullptr;
		for (QPushButton *b : w.findChildren<QPushButton *>())
			if (b->isVisible() && b->toolTip() == QStringLiteral("Remove this text"))
				del = b; // the last one: Text 3
		ok(del != nullptr, "each box has a ×");
		if (del) {
			del->click();
			settle(500);
		}
		ok(selected()->text.variations.size() == 1 && variantBoxes(w).size() == 1, "Text 3 removed");
		if (undo) {
			emit undo->activated();
			settle(500);
			ok(tv->selectedClipPtr() && tv->selectedClipPtr()->text.variations.size() == 2,
			   "Ctrl+Z brings it back");
		}
	}

	std::printf("\n-- a plain Text has none of it --\n");
	addText->trigger();
	settle(400);
	c = selected();
	ok(c && c->type == TlClip::Type::Text && !c->text.random, "a plain Text is selected");
	ok(headerSays(w, QStringLiteral("Text")) && !headerSays(w, QStringLiteral("Random text")),
	   "the Inspector calls it Text");
	ok(variantBoxes(w).isEmpty() && !button(w, QStringLiteral("+  Add text")), "and shows no other texts");
	ok(text_variations::slotsOf(tv->model()).size() == 1, "only the Random text makes versions");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
