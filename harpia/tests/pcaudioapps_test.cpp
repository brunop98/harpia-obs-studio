// "PC audio from": whose sound PC Audio records.
//
//   * All apps is the whole mix, as before; so is any choice where Windows
//     cannot single apps out.
//   * Only these apps is a capture per app: by file name, any case, once
//     each, at most eight. None picked records no PC sound -- and the picker
//     will not let that happen by accident.
//   * All apps except one is one capture that leaves that app out; with none
//     picked, it is all apps.
//   * The choice is kept (one for every preset), with the apps' names, and
//     both lists survive switching modes.
//   * The picker lists running apps and the remembered ones not running now,
//     keeps ticks across Refresh, and says what the choice will do.
#include "ui/PcAudioAppsDialog.hpp"

#include <QApplication>
#include <QListWidget>
#include <QRadioButton>
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

static SystemAudioApp app(const char *exe, const char *name, bool playing, bool sound = true)
{
	SystemAudioApp a;
	a.exe = QString::fromLatin1(exe);
	a.name = QString::fromLatin1(name);
	a.playing = playing;
	a.hasSound = sound;
	return a;
}

static QListWidgetItem *itemFor(QListWidget *l, const QString &exe)
{
	for (int i = 0; i < l->count(); ++i)
		if (l->item(i)->data(Qt::UserRole).toString() == exe)
			return l->item(i);
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication qa(argc, argv);

	std::printf("\n-- what to capture --\n");
	{
		ok(normalizeAppExe("C:\\Games\\Game.EXE") == "game.exe" && normalizeAppExe("  Spotify.exe ") == "spotify.exe" &&
			   normalizeAppExe("/opt/x/Chrome.exe") == "chrome.exe" && normalizeAppExe("   ").empty(),
		   "apps are known by their file name, in lower case");
		PcAudioChoice c;
		ok(pcAudioCaptures(c, true).size() == 1 && pcAudioCaptures(c, true)[0].wholeMix(), "All apps: the whole mix");
		c.from = PcAudioFrom::OnlyApps;
		c.onlyApps = {"Chrome.exe", "chrome.exe", "D:\\Games\\Game.exe"};
		const std::vector<PcAudioCapture> only = pcAudioCaptures(c, true);
		ok(only.size() == 2 && only[0].exe == "chrome.exe" && only[1].exe == "game.exe" && !only[0].exclude &&
			   !only[1].exclude,
		   "Only these apps: one capture each, once each");
		ok(pcAudioCaptures(c, false).size() == 1 && pcAudioCaptures(c, false)[0].wholeMix(),
		   "where Windows cannot single apps out: the whole mix, whatever the choice");
		c.onlyApps.clear();
		for (int i = 0; i < 11; ++i)
			c.onlyApps.push_back("app" + std::to_string(i) + ".exe");
		ok(int(pcAudioCaptures(c, true).size()) == kMaxPcAudioApps, "at most eight");
		c.onlyApps.clear();
		ok(pcAudioCaptures(c, true).empty(), "none picked: no PC sound");
		c.from = PcAudioFrom::AllExceptApp;
		c.exceptApp = "Spotify.exe";
		const std::vector<PcAudioCapture> but = pcAudioCaptures(c, true);
		ok(but.size() == 1 && but[0].exe == "spotify.exe" && but[0].exclude,
		   "All apps except one: one capture, leaving it out");
		c.exceptApp.clear();
		ok(pcAudioCaptures(c, true).size() == 1 && pcAudioCaptures(c, true)[0].wholeMix(),
		   "with none to leave out: all apps");
		ok(pcAudioFromKey(pcAudioFromKey(PcAudioFrom::OnlyApps)) == PcAudioFrom::OnlyApps &&
			   pcAudioFromKey(pcAudioFromKey(PcAudioFrom::AllExceptApp)) == PcAudioFrom::AllExceptApp &&
			   pcAudioFromKey(std::string("nonsense")) == PcAudioFrom::All,
		   "the mode is saved by name; an unknown one reads as All apps");
	}

	std::printf("\n-- the button --\n");
	{
		QHash<QString, QString> names;
		names.insert(QStringLiteral("spotify.exe"), QStringLiteral("Spotify"));
		names.insert(QStringLiteral("chrome.exe"), QStringLiteral("Google Chrome"));
		PcAudioChoice c;
		ok(pcAudioSummary(c, true, names) == QStringLiteral("All apps"), "All apps");
		c.from = PcAudioFrom::AllExceptApp;
		c.exceptApp = "spotify.exe";
		ok(pcAudioSummary(c, true, names) == QStringLiteral("All but Spotify"), "All but Spotify");
		ok(pcAudioSummary(c, false, names) == QStringLiteral("All apps"),
		   "where apps cannot be picked, it says what is recorded: all apps");
		c.from = PcAudioFrom::OnlyApps;
		c.onlyApps = {"chrome.exe"};
		ok(pcAudioSummary(c, true, names) == QStringLiteral("Only Google Chrome"), "Only Google Chrome");
		c.onlyApps = {"chrome.exe", "game.exe", "obs.exe"};
		ok(pcAudioSummary(c, true, names) == QStringLiteral("Only 3 apps"), "Only 3 apps");
		ok(pcAudioAppNames(c, names) ==
			   QStringList({QStringLiteral("Google Chrome"), QStringLiteral("game"), QStringLiteral("obs")}),
		   "named in full on hover");
		c.onlyApps = {"MyGame.exe"};
		ok(pcAudioSummary(c, true, names) == QStringLiteral("Only mygame"), "an app with no name known: its file");
	}

	std::printf("\n-- kept --\n");
	QTemporaryDir tmp;
	{
		QSettings s(tmp.filePath(QStringLiteral("harpia.ini")), QSettings::IniFormat);
		ok(pc_audio_settings::load(s) == PcAudioChoice(), "nothing saved: all apps");
		PcAudioChoice c;
		c.from = PcAudioFrom::AllExceptApp;
		c.onlyApps = {"chrome.exe", "game.exe"};
		c.exceptApp = "spotify.exe";
		QHash<QString, QString> names;
		names.insert(QStringLiteral("spotify.exe"), QStringLiteral("Spotify"));
		names.insert(QStringLiteral("chrome.exe"), QStringLiteral("Google Chrome"));
		names.insert(QStringLiteral("notepad.exe"), QStringLiteral("Notepad"));
		pc_audio_settings::save(s, c, names);
		s.sync();
		QSettings again(tmp.filePath(QStringLiteral("harpia.ini")), QSettings::IniFormat);
		QHash<QString, QString> back;
		ok(pc_audio_settings::load(again, &back) == c, "the mode and both lists come back");
		ok(back.value(QStringLiteral("spotify.exe")) == QStringLiteral("Spotify") &&
			   back.value(QStringLiteral("chrome.exe")) == QStringLiteral("Google Chrome"),
		   "with the apps' names");
		ok(!back.contains(QStringLiteral("notepad.exe")), "and only the names of apps in the choice");
	}

	std::printf("\n-- the picker --\n");
	QVector<SystemAudioApp> running = {app("chrome.exe", "Google Chrome", true),
					   app("Game.exe", "Space Game", false, false)};
	const auto lister = [&running]() { return running; };
	QHash<QString, QString> names;
	names.insert(QStringLiteral("spotify.exe"), QStringLiteral("Spotify"));
	{
		PcAudioChoice c;
		c.from = PcAudioFrom::OnlyApps;
		c.onlyApps = {"spotify.exe"};
		PcAudioAppsDialog d(c, true, lister, names);
		ok(d.modeButton(PcAudioFrom::OnlyApps)->isChecked(), "opens on the saved mode");
		QListWidget *only = d.onlyList();
		ok(only->count() == 3, "the running apps, and the remembered one");
		QListWidgetItem *sp = itemFor(only, QStringLiteral("spotify.exe"));
		ok(sp && sp->checkState() == Qt::Checked && sp->text().contains(QStringLiteral("Spotify")) &&
			   sp->text().contains(QStringLiteral("not running")),
		   "Spotify, not running now, still ticked, by its name");
		QListWidgetItem *ch = itemFor(only, QStringLiteral("chrome.exe"));
		ok(ch && ch->text().contains(QStringLiteral("playing")), "Chrome says it is playing");
		ok(itemFor(only, QStringLiteral("game.exe")) != nullptr, "a game with a window and no sound yet is there too");

		sp->setCheckState(Qt::Unchecked);
		ok(!d.okEnabled() && d.statusText().contains(QStringLiteral("Tick the apps")),
		   "nothing ticked: OK waits, and it says why");
		ch->setCheckState(Qt::Checked);
		itemFor(only, QStringLiteral("game.exe"))->setCheckState(Qt::Checked);
		ok(d.okEnabled() && d.statusText().contains(QStringLiteral("2 apps")), "two ticked: OK");

		running.append(app("discord.exe", "Discord", true));
		d.refreshForTest();
		ok(only->count() == 3 && itemFor(only, QStringLiteral("discord.exe")),
		   "Refresh adds an app started since");
		ok(!itemFor(only, QStringLiteral("spotify.exe")),
		   "and drops one that is neither running nor ticked any more");
		ok(itemFor(only, QStringLiteral("chrome.exe"))->checkState() == Qt::Checked &&
			   itemFor(only, QStringLiteral("game.exe"))->checkState() == Qt::Checked,
		   "and keeps what was ticked");

		d.modeButton(PcAudioFrom::AllExceptApp)->setChecked(true);
		ok(!d.okEnabled() && d.statusText().contains(QStringLiteral("leave out")), "All but one: pick one first");
		itemFor(d.exceptList(), QStringLiteral("discord.exe"))->setSelected(true);
		ok(d.okEnabled() && d.statusText().contains(QStringLiteral("except Discord")), "Everything except Discord");
		const PcAudioChoice out = d.choice();
		ok(out.from == PcAudioFrom::AllExceptApp && out.exceptApp == "discord.exe", "the choice: all but Discord");
		ok(out.onlyApps == std::vector<std::string>({"chrome.exe", "game.exe"}),
		   "and the Only list is kept for next time");
		ok(d.names().value(QStringLiteral("discord.exe")) == QStringLiteral("Discord"),
		   "names learned from the list, for the button");
	}
	{
		std::printf("\n-- the cap --\n");
		QVector<SystemAudioApp> many;
		for (int i = 0; i < 10; ++i)
			many.append(app(qPrintable(QStringLiteral("app%1.exe").arg(i)), qPrintable(QStringLiteral("App %1").arg(i)),
					false));
		PcAudioChoice c;
		c.from = PcAudioFrom::OnlyApps;
		PcAudioAppsDialog d(c, true, [many]() { return many; }, {});
		for (int i = 0; i < d.onlyList()->count(); ++i)
			d.onlyList()->item(i)->setCheckState(Qt::Checked);
		ok(int(d.choice().onlyApps.size()) == kMaxPcAudioApps, "a ninth tick does not stick");
		ok(d.statusText().contains(QStringLiteral("Up to 8")), "and it says so");
	}
	{
		std::printf("\n-- where apps cannot be picked --\n");
		PcAudioChoice c;
		c.from = PcAudioFrom::AllExceptApp;
		c.exceptApp = "spotify.exe";
		PcAudioAppsDialog d(c, false, lister, names);
		ok(d.modeButton(PcAudioFrom::All)->isChecked() && !d.modeButton(PcAudioFrom::OnlyApps)->isEnabled() &&
			   !d.modeButton(PcAudioFrom::AllExceptApp)->isEnabled(),
		   "All apps, the others off");
		ok(d.statusText().contains(QStringLiteral("Windows 10 version 2004")), "and it says what is needed");
		ok(d.okEnabled(), "OK still closes it");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
