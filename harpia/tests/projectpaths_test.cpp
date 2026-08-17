// Where a project is saved when the user has not said.
//
// The old answer was "next to the video it was started from", which is fine for
// one clip and wrong for everything else: a project drawing on four files from
// four folders has no such place, and projects end up scattered through the
// user's video collection with nowhere to look for last week's work.
//
// The naming rules are what is checkable without a filesystem, and they are
// also where the damage would be: a suggested path that already exists is a
// Save dialog pre-filled with someone else's project, one keypress from
// overwriting it.
#include "editor/ProjectPaths.hpp"

#include <QCoreApplication>
#include <QSet>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- the folder itself --\n");
	{
		const QString base = harpiaBaseFolder();
		const QString proj = defaultProjectsFolder();
		ok(!base.isEmpty(), "there is always a base folder, even with no Movies location");
		ok(proj.startsWith(base), "and projects live inside it");
		ok(proj.endsWith(QStringLiteral("Projects")), "in a folder that says what it holds");
		// Nothing is created by asking. main() creates it on purpose; the
		// editor asks for the path in places that must not touch the disk.
		ok(defaultProjectsFolder() == proj, "asking twice gives the same answer");
	}

	std::printf("\n-- what a new project is called --\n");
	{
		ok(suggestedProjectStem(QStringLiteral("/videos/Holiday clip.mp4")) ==
			   QStringLiteral("Holiday clip"),
		   "named after the media it was started from");
		ok(suggestedProjectStem(QStringLiteral("/videos/take.2.final.mov")) ==
			   QStringLiteral("take.2.final"),
		   "and only the LAST extension is dropped, so a dotted name survives");
		ok(suggestedProjectStem(QString()) == QStringLiteral("Untitled"),
		   "a project begun from a blank editor has no media to be named after");
	}

	std::printf("\n-- and it never lands on a file that is already there --\n");
	{
		QSet<QString> taken;
		const QString dir = QStringLiteral("/p");
		const auto exists = [&taken](const QString &p) { return taken.contains(p); };

		const QString first = uniqueProjectPath(dir, QStringLiteral("Holiday"), exists);
		ok(first == QStringLiteral("/p/Holiday.harpiaproj"), "the plain name when it is free");

		taken.insert(first);
		const QString second = uniqueProjectPath(dir, QStringLiteral("Holiday"), exists);
		ok(second == QStringLiteral("/p/Holiday 2.harpiaproj"),
		   "and a numbered one when it is not");

		taken.insert(second);
		ok(uniqueProjectPath(dir, QStringLiteral("Holiday"), exists) ==
			   QStringLiteral("/p/Holiday 3.harpiaproj"),
		   "counting on past every name that is taken");

		// The case that must not hang. A predicate that always says yes is a
		// full disk, a permission problem, or a stub in a test like this one.
		const QString giveUp =
			uniqueProjectPath(dir, QStringLiteral("Holiday"),
					  [](const QString &) { return true; });
		ok(giveUp == QStringLiteral("/p/Holiday.harpiaproj"),
		   "with everything taken it gives back the plain name rather than spinning, "
		   "and lets the save fail with a real error");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
