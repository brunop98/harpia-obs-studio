// Saved lists of text variations.
//
//   * The file: readable JSON, blank lines dropped, junk entries skipped, a
//     save then load gives the same lists (accents, quotes, rich-text tags).
//   * In the real editor window: a list saved in the file shows up in the
//     Variations box's "Saved lists…" combo; picking it puts those lines on
//     the selected caption (its own text untouched), and one Ctrl+Z takes
//     them off again.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/timeline/VariationPresets.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QStandardPaths>
#include <QTemporaryDir>
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

	std::printf("\n-- the file --\n");
	{
		QTemporaryDir dir;
		const QString path = dir.filePath(QStringLiteral("sub/variation-presets.json"));
		QMap<QString, QStringList> p;
		p.insert(QStringLiteral("Pets"), {QStringLiteral("I like cats"), QStringLiteral(""),
						  QStringLiteral("I like vultures"), QStringLiteral("  ")});
		p.insert(QStringLiteral("Ação"), {QStringLiteral("Era uma \"vez\""), QStringLiteral("<b>ção</b>")});
		ok(variation_presets::save(path, p), "saved (the folder is made)");
		const auto back = variation_presets::load(path);
		ok(back.value(QStringLiteral("Pets")) ==
			   QStringList({QStringLiteral("I like cats"), QStringLiteral("I like vultures")}),
		   "blank lines are not kept");
		ok(back.value(QStringLiteral("Ação")) == p.value(QStringLiteral("Ação")),
		   "accents, quotes and tags survive");
		ok(variation_presets::fromJson(R"({"a": "not a list", "": ["x"], "b": [1, "y"], "c": []})") ==
			   QMap<QString, QStringList>({{QStringLiteral("b"), {QStringLiteral("y")}}}),
		   "junk entries are skipped");
		ok(variation_presets::load(dir.filePath(QStringLiteral("missing.json"))).isEmpty(), "no file: no lists");
	}

	// The window reads the same place it writes.
	const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
			     QStringLiteral("/harpia/variation-presets.json");
	QMap<QString, QStringList> saved;
	saved.insert(QStringLiteral("Pets"),
		     {QStringLiteral("I like cats"), QStringLiteral("I like vultures"), QStringLiteral("I like parrots")});
	ok(variation_presets::save(path, saved), "a list saved where the editor keeps them");

	std::printf("\n-- in the editor --\n");
	VideoEditorWindow w(video);
	w.resize(1400, 900);
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

	TimelineModel m;
	TlTrack t;
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcEndMs = 2000;
	c.text.text = QStringLiteral("I like dogs");
	t.clips = {c};
	m.tracks = {t};
	tv->setModelAndCommit(m);
	settle(300);
	tv->zoomToFit();
	settle(50);
	{
		const QPoint at = tv->clipRectForTest(0, 0).center();
		QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), tv->mapToGlobal(QPointF(at)), Qt::LeftButton,
				  Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &press);
		QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), tv->mapToGlobal(QPointF(at)),
				    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &release);
		settle(200);
	}
	ok(tv->selectedClip() == 0, "the caption is selected");

	QComboBox *combo = nullptr;
	for (QComboBox *cb : w.findChildren<QComboBox *>())
		if (cb->count() > 0 && cb->itemText(0) == QStringLiteral("Saved lists…"))
			combo = cb;
	ok(combo != nullptr, "the Variations box has a saved-lists combo");
	if (!combo)
		return 1;
	const int idx = combo->findText(QStringLiteral("Pets"));
	ok(idx > 0, "the saved list is in it");
	combo->setCurrentIndex(idx);
	emit combo->activated(idx);
	settle(500);
	const TlClip &after = tv->model().tracks[0].clips[0];
	ok(after.text.variations == saved.value(QStringLiteral("Pets")), "picking it puts its lines on the caption");
	ok(after.text.text == QStringLiteral("I like dogs"), "the caption keeps its own text");

	QShortcut *undo = nullptr;
	for (QShortcut *s : w.findChildren<QShortcut *>())
		if (s->key() == QKeySequence(Qt::CTRL | Qt::Key_Z) && s->isEnabled())
			undo = s;
	ok(undo != nullptr, "Ctrl+Z is bound");
	if (undo) {
		emit undo->activated();
		settle(200);
		ok(tv->model().tracks[0].clips[0].text.variations.isEmpty(), "one Ctrl+Z takes the list off again");
	}

	QFile::remove(path);
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
