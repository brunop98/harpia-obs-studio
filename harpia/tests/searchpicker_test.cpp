// The searchable picker behind Add Component and Add effect.
//
// The ranking is the part people feel: "bl" has to put Blur first, "alwz"
// has to find Always Zoom, "zoom" has to prefer Zoom over Always Zoom, and a
// query with no match has to say so rather than show everything. Then the
// widget: folders until you type, a flat ranked list once you do, Enter picks
// the highlighted row, Up/Down move it, Escape closes with nothing, and a
// click away also closes with nothing -- which a blocking pick() depends on.
#include "ui/FuzzyMatch.hpp"
#include "ui/SearchPicker.hpp"

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QTest>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eqs(const QString &got, const char *want, const char *w)
{
	const bool good = got == QString::fromUtf8(want);
	std::printf("  %s %s (got \"%s\")\n", good ? "PASS" : "FAIL", w, qPrintable(got));
	if (!good)
		++failures;
}

static QVector<PickItem> catalogue()
{
	auto it = [](const char *id, const char *name, const char *cat, bool pinned = false) {
		PickItem p;
		p.id = QString::fromLatin1(id);
		p.name = QString::fromUtf8(name);
		p.category = QString::fromLatin1(cat);
		p.pinned = pinned;
		return p;
	};
	return {it("blur", "Blur", "Pixel"),          it("mask", "Mask", "Pixel"),
		it("rot", "Always Rotate", "Motion"), it("grow", "Always Grow", "Motion"),
		it("azoom", "Always Zoom", "Motion"), it("zoom", "Zoom", "Motion"),
		it("type", "Typing", "Text"),         it("bul", "Bullets", "Text"),
		it("sub", "Subtitle", "Text"),        it("sh.glitch", "glitch", "Shader"),
		it("sc.shake", "shake", "Script")};
}

static QStringList names(const QVector<PickItem> &v)
{
	QStringList out;
	for (const PickItem &p : v)
		out << p.name;
	return out;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QVector<PickItem> all = catalogue();

	std::printf("\n-- the ranking --\n");
	{
		eqs(names(rankItems(all, "bl")).value(0), "Blur", "'bl' puts Blur first");
		ok(names(rankItems(all, "bl")).contains(QStringLiteral("Bullets")),
		   "and still finds Bullets further down (b…l, in order)");
		eqs(names(rankItems(all, "alwz")).join(","), "Always Zoom", "'alwz' finds Always Zoom and nothing else");
		eqs(names(rankItems(all, "zoom")).value(0), "Zoom", "'zoom' prefers the exact name over Always Zoom");
		eqs(names(rankItems(all, "zoom")).value(1), "Always Zoom", "which comes second");
		eqs(names(rankItems(all, "az")).value(0), "Always Zoom", "'az' matches the word starts");
		ok(rankItems(all, "qqq").isEmpty(), "a query nothing matches gives an empty list, not everything");
		ok(rankItems(all, "").size() == all.size() && names(rankItems(all, "")) == names(all),
		   "an empty query is every item in the order given");
		ok(fuzzyScore("BLUR", "blur") > 0 && fuzzyScore("always zoom", "Always Zoom") > fuzzyScore("alz", "Always Zoom"),
		   "case does not matter, and a fuller query scores higher than a sparser one");
		ok(fuzzyScore("ur", "Blur") > 0 && fuzzyScore("ur", "Blur") < fuzzyScore("bl", "Blur"),
		   "a substring in the middle matches, below a prefix");
		ok(names(rankItems(all, "sh")).contains(QStringLiteral("shake")) && names(rankItems(all, "gl")).value(0) == QStringLiteral("glitch"),
		   "scripts and shaders are in the same list and found the same way");
	}

	std::printf("\n-- the widget: folders, then a flat list --\n");
	{
		auto *p = new SearchPicker(all);
		QString picked = QStringLiteral("(none)");
		p->setOnPicked([&](const QString &id) { picked = id; });
		p->showAt(QPoint(100, 100));
		QApplication::processEvents();
		const QStringList folders = p->visibleNames();
		std::printf("     folders: %s\n", qPrintable(folders.join(" | ")));
		ok(folders.size() == 5 && folders[0].startsWith(QStringLiteral("Pixel")) && folders[1].startsWith(QStringLiteral("Motion")),
		   "before typing: one folder per category, in first-seen order");
		ok(p->highlightedRow() == 0, "the first row is highlighted so Enter does something");

		// Enter on a folder opens it; the first row is the way back.
		QTest::keyClick(p->searchField(), Qt::Key_Return);
		QApplication::processEvents();
		const QStringList inPixel = p->visibleNames();
		ok(inPixel.size() == 3 && inPixel[0].contains(QStringLiteral("Pixel")) && inPixel[1] == QStringLiteral("Blur") &&
			   inPixel[2] == QStringLiteral("Mask"),
		   "Enter on Pixel shows its two items under a back row");
		QTest::keyClick(p->searchField(), Qt::Key_Backspace);
		QApplication::processEvents();
		ok(p->visibleNames() == folders, "Backspace in an empty field steps back out to the folders");

		// Typing replaces the folders with the ranked list.
		QTest::keyClicks(p->searchField(), QStringLiteral("alwz"));
		QApplication::processEvents();
		eqs(p->visibleNames().join(","), "Always Zoom", "typing 'alwz' shows only Always Zoom");
		p->searchField()->clear();
		QTest::keyClicks(p->searchField(), QStringLiteral("zo"));
		QApplication::processEvents();
		ok(p->visibleNames().value(0) == QStringLiteral("Zoom") && p->highlightedRow() == 0, "'zo': Zoom first, highlighted");
		QTest::keyClick(p->searchField(), Qt::Key_Down);
		ok(p->highlightedRow() == 1, "Down moves the highlight");
		QTest::keyClick(p->searchField(), Qt::Key_Up);
		ok(p->highlightedRow() == 0, "and Up moves it back");
		QTest::keyClick(p->searchField(), Qt::Key_Down);
		QTest::keyClick(p->searchField(), Qt::Key_Return);
		QApplication::processEvents();
		eqs(picked, "azoom", "Enter picks the highlighted row (Always Zoom)");
		ok(!p->isVisible(), "and the popup is gone");
	}

	std::printf("\n-- nothing picked --\n");
	{
		auto *p = new SearchPicker(all);
		int calls = 0;
		QString picked = QStringLiteral("(none)");
		p->setOnPicked([&](const QString &id) {
			picked = id;
			++calls;
		});
		p->showAt(QPoint(100, 100));
		QApplication::processEvents();
		QTest::keyClicks(p->searchField(), QStringLiteral("qqq"));
		QApplication::processEvents();
		ok(p->visibleNames().size() == 1 && p->visibleNames()[0].contains(QStringLiteral("Nothing")),
		   "a query with no match says so");
		QTest::keyClick(p->searchField(), Qt::Key_Return);
		ok(calls == 0, "and Enter on it does nothing");
		QTest::keyClick(p->searchField(), Qt::Key_Escape);
		QApplication::processEvents();
		ok(calls == 1 && picked.isEmpty(), "Escape reports an empty pick, exactly once");

		auto *q = new SearchPicker(all);
		int qc = 0;
		q->setOnPicked([&](const QString &) { ++qc; });
		q->showAt(QPoint(100, 100));
		QApplication::processEvents();
		q->hide(); // what clicking away does to a Qt::Popup
		QApplication::processEvents();
		ok(qc == 1, "closing by any other route also reports once, so a blocking pick() returns");

		// A pinned item sits above the folders and is found by search.
		QVector<PickItem> withPaste = all;
		PickItem paste;
		paste.id = QStringLiteral("paste:");
		paste.name = QStringLiteral("Paste Blur");
		paste.pinned = true;
		withPaste.prepend(paste);
		auto *r = new SearchPicker(withPaste);
		r->showAt(QPoint(100, 100));
		QApplication::processEvents();
		ok(r->visibleNames().value(0) == QStringLiteral("Paste Blur") && r->visibleNames().size() == 6,
		   "a pinned item is the first row, above the folders");
		QTest::keyClicks(r->searchField(), QStringLiteral("paste"));
		QApplication::processEvents();
		ok(r->visibleNames().value(0) == QStringLiteral("Paste Blur"), "and typing finds it like anything else");
		r->hide();
		QApplication::processEvents();
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
