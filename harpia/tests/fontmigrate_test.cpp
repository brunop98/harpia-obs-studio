// The one-shot migration that lets the new text scale reach the editor's
// timecode and inspector fonts on an install that already saved the old ones.
//
// Driven through DevPanel::loadChrome() against a real QSettings, because the
// bug this guards is entirely about persisted state: a fresh run passes no
// matter what the migration does.
#include "editor/DevPanel.hpp"
#include "ui/UiText.hpp"

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// The registry the panel actually uses -- an explicit ("Harpia","Recorder"),
// not the application scope. Clearing the wrong one is how a settings test
// passes while testing nothing.
static QSettings dev()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
static void put(const char *key, int v)
{
	QSettings s = dev();
	s.beginGroup(QStringLiteral("devLayout"));
	s.setValue(QLatin1String(key), v);
	s.endGroup();
	s.sync();
}
static QVariant get(const char *key)
{
	QSettings s = dev();
	s.beginGroup(QStringLiteral("devLayout"));
	return s.value(QLatin1String(key));
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// Keep the real user's settings out of this: point QSettings at a temp dir.
	QTemporaryDir tmp;
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
	QSettings::setDefaultFormat(QSettings::IniFormat);

	QApplication app(argc, argv);
	applyTextScale(app);
	std::printf("     text scale gives timecode %d, readout %d\n", uiTimecodePx(), uiReadoutPx());

	std::printf("\n-- an install carrying the old fixed sizes --\n");
	{
		QSettings s = dev();
		s.clear();
		s.sync();
	}
	put("win/tcFont", 18); // exactly what shipped before
	put("win/insFont", 13);
	put("win/btnH", 33); // an unrelated tuned value, to prove the blast radius

	EditorChromeParams p = DevPanel::loadChrome();
	std::printf("     loaded timecode %d, readout %d, buttonH %d\n", p.timecodeFontPx,
		    p.inspectorFontPx, p.buttonH);
	ok(p.timecodeFontPx == uiTimecodePx(), "the timecode font follows the scale now");
	ok(p.inspectorFontPx == uiReadoutPx(), "and so does the inspector readout");
	ok(p.buttonH == 33, "nothing else in the group was touched");

	std::printf("\n-- and it only happens once --\n");
	// Having migrated, a size set deliberately afterwards must stick -- including
	// the old default itself, which a value-comparison migration would eat.
	put("win/tcFont", 18);
	EditorChromeParams p2 = DevPanel::loadChrome();
	std::printf("     after deliberately choosing 18 again: %d\n", p2.timecodeFontPx);
	ok(p2.timecodeFontPx == 18, "18 chosen on purpose survives the next launch");
	ok(get("win/fontsFollowScale").toBool(), "the flag is set, so it will not re-run");

	std::printf("\n-- a fresh install needs no migration --\n");
	{
		QSettings s = dev();
		s.clear();
		s.sync();
	}
	EditorChromeParams p3 = DevPanel::loadChrome();
	ok(p3.timecodeFontPx == uiTimecodePx(), "it starts at the scaled size");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
