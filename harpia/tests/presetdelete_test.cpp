// Deleting a preset (ui/PresetDelete.hpp): the question Harpia asks.
//
//   * The same folder is recognised however it is written (trailing slash,
//     "./" and ".." parts, backslashes); an empty folder matches nothing.
//   * The other presets saving to the same folder are found, in order.
//   * The question names the preset; the details say the recordings stay on
//     disk, and either that they stop being listed (no other preset saves
//     there) or that they stay listed (naming who else saves there); a preset
//     with no folder changes nothing on disk. Names with quotes and %1 come
//     through as they are.
//   * The box: only "Delete preset" says yes. Cancel is the default, so Enter
//     keeps the preset, and so do Escape and Cancel.
#include "ui/PresetDelete.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include <cstdio>
#include <functional>

using namespace harpia::preset_delete;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// Does `act` to the next message box, reading its texts first; closes it
// whatever happens, so a missing box fails a check instead of hanging.
static void onNextBox(std::function<void(QMessageBox *)> act, QString *text = nullptr, QString *details = nullptr)
{
	QTimer::singleShot(100, [act, text, details]() {
		for (QWidget *w : QApplication::topLevelWidgets())
			if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
				if (text)
					*text = box->text();
				if (details)
					*details = box->informativeText();
				act(box);
				if (box->isVisible())
					box->reject();
				return;
			}
	});
}

static QAbstractButton *buttonNamed(QMessageBox *box, const QString &label)
{
	for (QAbstractButton *b : box->buttons())
		if (b->text() == label)
			return b;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the same folder --\n");
	ok(sameFolder(QStringLiteral("C:/Videos/Harpia"), QStringLiteral("C:/Videos/Harpia/")), "a trailing slash");
	ok(sameFolder(QStringLiteral("C:/Videos/./Harpia"), QStringLiteral("C:/Videos/x/../Harpia")),
	   "\"./\" and \"..\" parts");
	ok(sameFolder(QStringLiteral("C:\\Videos\\Harpia"), QStringLiteral("C:/Videos/Harpia")), "backslashes");
	ok(!sameFolder(QStringLiteral("C:/Videos/Harpia"), QStringLiteral("C:/Videos/Harpia 2")), "a different folder");
	ok(!sameFolder(QString(), QString()) && !sameFolder(QStringLiteral(" "), QStringLiteral("C:/x")),
	   "an empty folder matches nothing");
#ifdef Q_OS_WIN
	ok(sameFolder(QStringLiteral("C:/videos"), QStringLiteral("c:/Videos")), "case does not matter on Windows");
#else
	ok(!sameFolder(QStringLiteral("/home/a/videos"), QStringLiteral("/home/a/Videos")),
	   "case matters where the file system cares");
#endif

	std::printf("\n-- who else saves there --\n");
	const QVector<OtherPreset> others{{QStringLiteral("Tutorials"), QStringLiteral("C:/Videos/Harpia/")},
					  {QStringLiteral("Games"), QStringLiteral("D:/Games")},
					  {QStringLiteral("Shorts"), QStringLiteral("C:\\Videos\\Harpia")}};
	ok(sharing(QStringLiteral("C:/Videos/Harpia"), others) ==
		   (QStringList{QStringLiteral("Tutorials"), QStringLiteral("Shorts")}),
	   "the presets with the same folder, in list order");
	ok(sharing(QStringLiteral("E:/Elsewhere"), others).isEmpty(), "nobody else: none");
	ok(sharing(QString(), others).isEmpty(), "no folder: none");

	std::printf("\n-- the question --\n");
	Question q = question(QStringLiteral("Unity"), QStringLiteral("D:/Recordings/Unity"), {});
	ok(q.text == QStringLiteral("Delete the preset \"Unity\"?"), "names the preset");
	ok(q.details.contains(QDir::toNativeSeparators(QStringLiteral("D:/Recordings/Unity"))) &&
		   q.details.contains(QStringLiteral("not deleted")),
	   "the recordings stay, and where");
	ok(q.details.contains(QStringLiteral("no longer list them in Recent recordings or the Clip Library")),
	   "alone in its folder: they stop being listed");
	ok(q.details.contains(QStringLiteral("cannot be brought back")), "and the preset cannot come back");

	q = question(QStringLiteral("Unity"), QStringLiteral("C:/Videos/Harpia"), {QStringLiteral("Tutorials")});
	ok(q.details.contains(QStringLiteral("stay listed, because the preset \"Tutorials\" saves there too")),
	   "shared with one: they stay listed, and who saves there");
	q = question(QStringLiteral("Unity"), QStringLiteral("C:/Videos/Harpia"),
		     {QStringLiteral("Tutorials"), QStringLiteral("Shorts")});
	ok(q.details.contains(QStringLiteral("because the presets \"Tutorials\", \"Shorts\" save there too")),
	   "shared with two: both named");
	ok(!q.details.contains(QStringLiteral("no longer list")), "and nothing about them disappearing");

	q = question(QStringLiteral("Scratch"), QString(), {});
	ok(q.details.contains(QStringLiteral("nothing on disk changes")), "no folder: nothing on disk changes");

	q = question(QStringLiteral("50%1 \"best\""), QStringLiteral("C:/a %2 b"), {QStringLiteral("x%3")});
	ok(q.text == QStringLiteral("Delete the preset \"50%1 \"best\"\"?"), "a name with %1 and quotes, as it is");
	ok(q.details.contains(QDir::toNativeSeparators(QStringLiteral("C:/a %2 b"))) &&
		   q.details.contains(QStringLiteral("\"x%3\"")),
	   "and a folder and another name with %N in them");

	std::printf("\n-- the box --\n");
	const Question asked = question(QStringLiteral("Unity"), QStringLiteral("D:/Recordings/Unity"), {});
	QString text, details;
	onNextBox([](QMessageBox *box) {
		if (QAbstractButton *b = buttonNamed(box, QStringLiteral("Delete preset")))
			b->click();
	}, &text, &details);
	ok(confirm(nullptr, asked), "\"Delete preset\": yes");
	ok(text == asked.text && details == asked.details, "the box shows the question and the details");

	QString defaultLabel;
	onNextBox([&defaultLabel](QMessageBox *box) {
		if (QPushButton *d = box->defaultButton())
			defaultLabel = d->text();
		QTest::keyClick(box, Qt::Key_Return);
	});
	ok(!confirm(nullptr, asked), "Enter: kept");
	ok(!defaultLabel.contains(QStringLiteral("Delete")), "the default button is not Delete");

	onNextBox([](QMessageBox *box) { QTest::keyClick(box, Qt::Key_Escape); });
	ok(!confirm(nullptr, asked), "Escape: kept");
	onNextBox([](QMessageBox *box) {
		if (QAbstractButton *b = box->button(QMessageBox::Cancel))
			b->click();
	});
	ok(!confirm(nullptr, asked), "Cancel: kept");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
