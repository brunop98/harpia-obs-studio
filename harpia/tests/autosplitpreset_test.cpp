// Auto Split moved from app-wide preferences into recording profiles. Exercise
// the real JSON store: old preferences migrate once, explicit profile settings
// win, new profiles start disabled, and malformed limits never wrap.
#include "core/AutoSplitSettings.hpp"
#include "model/PresetStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;

namespace {
int failures = 0;

void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

bool writeProfiles(const QString &directory, const QJsonArray &profiles)
{
	QDir().mkpath(directory);
	QFile file(directory + QStringLiteral("/presets.json"));
	return file.open(QIODevice::WriteOnly) &&
	       file.write(QJsonDocument(QJsonObject{{QStringLiteral("presets"), profiles}}).toJson()) > 0;
}

QJsonObject profile(const QString &id)
{
	return {{QStringLiteral("id"), id}, {QStringLiteral("name"), id},
		{QStringLiteral("format"), QStringLiteral("mp4")}};
}

QJsonArray readProfiles(const QString &directory)
{
	QFile file(directory + QStringLiteral("/presets.json"));
	return file.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("presets")).toArray()
		: QJsonArray();
}
} // namespace

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	// XDG_CONFIG_HOME isolates the legacy native QSettings namespace only on
	// Unix. Never clear the user's real registry/CFPreferences while testing.
	(void)argc;
	(void)argv;
	std::printf("SKIP: legacy settings migration needs isolated Unix QSettings paths.\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	qputenv("XDG_CONFIG_HOME", (temporary.path() + QStringLiteral("/settings")).toUtf8());
	QCoreApplication application(argc, argv);
	QSettings legacy(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	legacy.clear();
	AutoSplitSettings old;
	old.enabled = true;
	old.mode = AutoSplitSettings::Mode::Size;
	old.minutes = 7;
	old.sizeMB = 49;
	auto_split_settings::save(legacy, old);
	legacy.sync();

	const QString freshPath = temporary.path() + QStringLiteral("/fresh");
	QDir().mkpath(freshPath);
	PresetStore fresh(freshPath.toStdString());
	fresh.load("/recordings");
	const Preset first = fresh.presets().front();
	check(!first.autoSplit && first.autoSplitMode == 0 && first.autoSplitMinutes == 30 &&
		first.autoSplitSizeMB == 500, "new profiles use disabled defaults despite an enabled legacy preference");
	Preset custom = first;
	custom.autoSplit = true;
	custom.autoSplitMode = 1;
	custom.autoSplitMinutes = 3;
	custom.autoSplitSizeMB = 99;
	fresh.upsert(custom);
	Preset other = Preset::makeDefault("/other");
	other.id = "other";
	fresh.upsert(other);
	PresetStore reopened(freshPath.toStdString());
	reopened.load("/recordings");
	const Preset *roundTrip = reopened.find(custom.id);
	const Preset *separate = reopened.find("other");
	check(roundTrip && roundTrip->autoSplit && roundTrip->autoSplitMode == 1 &&
		roundTrip->autoSplitMinutes == 3 && roundTrip->autoSplitSizeMB == 99,
		"enabled file-size settings survive profile save and reload");
	check(separate && !separate->autoSplit && separate->autoSplitMinutes == 30 && separate->autoSplitSizeMB == 500,
		"a second profile keeps independent disabled settings");
	const auto saved = readProfiles(freshPath);
	check(saved.size() == 2 && saved[0].toObject().contains(QStringLiteral("auto_split")) &&
		saved[0].toObject().contains(QStringLiteral("auto_split_mode")) &&
		saved[0].toObject().contains(QStringLiteral("auto_split_minutes")) &&
		saved[0].toObject().contains(QStringLiteral("auto_split_size_mb")),
		"every new profile saves explicit per-profile split keys");

	const QString migratedPath = temporary.path() + QStringLiteral("/legacy");
	QJsonObject explicitProfile = profile(QStringLiteral("explicit"));
	explicitProfile.insert(QStringLiteral("auto_split"), false);
	explicitProfile.insert(QStringLiteral("auto_split_mode"), 0);
	explicitProfile.insert(QStringLiteral("auto_split_minutes"), 12);
	explicitProfile.insert(QStringLiteral("auto_split_size_mb"), 800);
	QJsonObject partialProfile = profile(QStringLiteral("partial"));
	partialProfile.insert(QStringLiteral("auto_split_minutes"), 15);
	check(writeProfiles(migratedPath, {profile(QStringLiteral("legacy")), explicitProfile, partialProfile}),
		"legacy, explicit and partial profile fixtures are written");
	PresetStore migrated(migratedPath.toStdString());
	migrated.load("/recordings");
	const Preset *oldProfile = migrated.find("legacy");
	check(oldProfile && oldProfile->autoSplit && oldProfile->autoSplitMode == 1 &&
		oldProfile->autoSplitMinutes == 7 && oldProfile->autoSplitSizeMB == 49,
		"old existing profiles inherit the previous saved app-wide setting once");
	const Preset *explicitSettings = migrated.find("explicit");
	check(explicitSettings && !explicitSettings->autoSplit && explicitSettings->autoSplitMode == 0 &&
		explicitSettings->autoSplitMinutes == 12 && explicitSettings->autoSplitSizeMB == 800,
		"explicit profile settings override the legacy enabled preference");
	const Preset *partial = migrated.find("partial");
	check(partial && !partial->autoSplit && partial->autoSplitMode == 0 &&
		partial->autoSplitMinutes == 15 && partial->autoSplitSizeMB == 500,
		"partially populated profiles use defaults for missing keys without remigrating");
	const QJsonArray migratedJson = readProfiles(migratedPath);
	check(migratedJson.size() == 3 && migratedJson[0].toObject().value(QStringLiteral("auto_split")).toBool() &&
		migratedJson[0].toObject().value(QStringLiteral("auto_split_size_mb")).toInt() == 49,
		"migration immediately persists per-profile settings to disk");
	old.enabled = false;
	old.sizeMB = 1000;
	auto_split_settings::save(legacy, old);
	legacy.sync();
	PresetStore migratedAgain(migratedPath.toStdString());
	migratedAgain.load("/recordings");
	oldProfile = migratedAgain.find("legacy");
	check(oldProfile && oldProfile->autoSplit && oldProfile->autoSplitSizeMB == 49,
		"restarting does not reapply changed legacy global preferences");

	const QString malformedPath = temporary.path() + QStringLiteral("/malformed");
	QJsonObject malformed = profile(QStringLiteral("malformed"));
	malformed.insert(QStringLiteral("auto_split"), true);
	malformed.insert(QStringLiteral("auto_split_mode"), 99);
	malformed.insert(QStringLiteral("auto_split_minutes"), QStringLiteral("broken"));
	malformed.insert(QStringLiteral("auto_split_size_mb"), 1e100);
	QJsonObject negative = profile(QStringLiteral("negative"));
	negative.insert(QStringLiteral("auto_split_minutes"), -100);
	negative.insert(QStringLiteral("auto_split_size_mb"), -1);
	QJsonObject fractional = profile(QStringLiteral("fractional"));
	fractional.insert(QStringLiteral("auto_split_minutes"), 1.5);
	fractional.insert(QStringLiteral("auto_split_size_mb"), QStringLiteral("999999999999999999"));
	check(writeProfiles(malformedPath, {malformed, negative, fractional}), "malformed profile fixtures are written");
	PresetStore invalid(malformedPath.toStdString());
	invalid.load("/recordings");
	const Preset *bounded = invalid.find("malformed");
	check(bounded && bounded->autoSplit && bounded->autoSplitMode == 0 && bounded->autoSplitMinutes == 30 &&
		bounded->autoSplitSizeMB == auto_split_settings::maxSizeMB,
		"invalid mode and text use defaults while huge numeric limits clamp without wrapping");
	const Preset *minimums = invalid.find("negative");
	check(minimums && minimums->autoSplitMinutes == 1 && minimums->autoSplitSizeMB == 1,
		"negative limits clamp to the minimum supported values");
	const Preset *defaults = invalid.find("fractional");
	check(defaults && defaults->autoSplitMinutes == 30 && defaults->autoSplitSizeMB == 500,
		"fractional and string limits cannot silently turn into tiny split files");
	custom.autoSplitMode = -1;
	custom.autoSplitMinutes = -10;
	custom.autoSplitSizeMB = 2147483647;
	invalid.upsert(custom);
	PresetStore normalized(malformedPath.toStdString());
	normalized.load("/recordings");
	const Preset *canonical = normalized.find(custom.id);
	check(canonical && canonical->autoSplitMode == 0 && canonical->autoSplitMinutes == 1 &&
		canonical->autoSplitSizeMB == auto_split_settings::maxSizeMB,
		"programmatic invalid limits are normalized when saved");

	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
