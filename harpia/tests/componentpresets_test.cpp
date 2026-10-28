// User presets survive projects and restarts without losing animations or
// replacing unrelated presets, even when files are invalid or writes fail.
#include "editor/component/ComponentPresets.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
using namespace harpia::component_presets;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
QByteArray readBytes(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeBytes(const QString &path, const QByteArray &bytes)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(); }
class Noop : public IComponent {
	void evaluate(const EvalContext &, ClipState &) const override {}
};
ComponentType type(const QString &id, double def = .75)
{
	ComponentType result;
	result.id = id;
	result.version = 3;
	result.props = {{QStringLiteral("strength"), QStringLiteral("Strength"), PropType::Float, 0, 1, def},
			{QStringLiteral("on"), QStringLiteral("On"), PropType::Bool, 0, 1, 1}};
	result.make = [] { return std::make_unique<Noop>(); };
	return result;
}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir directory;
	check(directory.isValid(), "an isolated preset directory is available");
	const QString path = directory.filePath(QStringLiteral("nested/component-presets.json"));
	Store store(path);
	QString error = QStringLiteral("old error");
	check(store.load(&error).isEmpty() && error.isEmpty() && !QFile::exists(path),
	      "a new user's absent preset file is empty without an error or a write");
	ComponentRegistry reg;
	reg.add(type(QStringLiteral("test.crt")));
	reg.add(type(QStringLiteral("test.other")));
	ComponentInstance crt;
	crt.typeId = QStringLiteral("test.crt");
	crt.instanceId = QStringLiteral("project-local-id");
	crt.enabled = false;
	crt.inMs = 220;
	crt.outMs = 430;
	crt.props.insert(QStringLiteral("on"), false);
	crt.props.insert(QStringLiteral("color"), quint32(0x80442211));
	crt.props.insert(QStringLiteral("string"), QStringLiteral("arbitrary plugin property"));
	crt.props.insert(QStringLiteral("structured"), QVariantMap{{QStringLiteral("x"), 3}});
	crt.keys.insert(QStringLiteral("strength"), {{20, .2, TlEase::Linear}, {900, .9, TlEase::Bezier, .17, .81}});
	crt.unknown.insert(QStringLiteral("pluginData"), QJsonObject{{QStringLiteral("keep"), true}});
	const QString id = store.save(QStringLiteral("  Minha televisão 📺  "), crt, reg, &error);
	check(!id.isEmpty() && error.isEmpty() && QFile::exists(path), "saving a named preset creates the per-user file and its folder");
	Store reopened(path);
	auto presets = reopened.load(&error);
	check(error.isEmpty() && presets.size() == 1 && presets[0].id == id && presets[0].name == QStringLiteral("Minha televisão 📺"),
	      "a separate store restores the same Unicode name and stable preset id");
	const QJsonObject saved = presets.value(0).component;
	check(!saved.contains(QStringLiteral("id")), "project-local component identities are never stored in reusable presets");
	const ComponentInstance restored = componentFromJson(saved, reg);
	check(!restored.enabled && restored.inMs == 220 && restored.outMs == 430 && restored.keys == crt.keys,
	      "enabled state, fade timing, keyframes, easing and Bezier handles survive disk reload");
	check(restored.props.value(QStringLiteral("strength")).toDouble() == .75 &&
	      restored.props.value(QStringLiteral("on")).toBool() == false &&
	      restored.props.value(QStringLiteral("color")).toUInt() == quint32(0x80442211),
	      "saving snapshots omitted defaults while retaining explicit bool and packed colour values");
	check(restored.props.value(QStringLiteral("structured")).toMap().value(QStringLiteral("x")).toInt() == 3 &&
	      restored.props.value(QStringLiteral("string")).toString() == crt.props.value(QStringLiteral("string")).toString() &&
	      restored.unknown.value(QStringLiteral("pluginData")) == crt.unknown.value(QStringLiteral("pluginData")),
	      "string, structured and unknown plugin properties remain intact");
	reg.add(type(QStringLiteral("test.crt"), .1));
	check(componentFromJson(reopened.load().value(0).component, reg).props.value(QStringLiteral("strength")).toDouble() == .75,
	      "a later default change cannot silently change a saved look");
	crt.props.insert(QStringLiteral("strength"), .4);
	const QString replaced = reopened.save(QStringLiteral("MINHA TELEVISÃO 📺"), crt, reg, &error);
	check(replaced == id && reopened.load().size() == 1 && reopened.load()[0].component.value(QStringLiteral("props")).toObject().value(QStringLiteral("strength")).toDouble() == .4,
	      "replacing a case-insensitive name updates that type's preset and preserves its id");
	ComponentInstance other = crt;
	other.typeId = QStringLiteral("test.other");
	const QString otherId = reopened.save(QStringLiteral("MINHA TELEVISÃO 📺"), other, reg, &error);
	check(!otherId.isEmpty() && otherId != id && reopened.load().size() == 2,
	      "equal names on different component types never overwrite one another");
	check(!reopened.save(QStringLiteral("  "), crt, reg, &error).size() && !error.isEmpty() && reopened.load().size() == 2,
	      "an empty name fails without changing existing presets");
	check(reopened.save(QString(257, QLatin1Char('x')), crt, reg, &error).isEmpty() && !error.isEmpty(),
	      "an excessively long preset name reports a useful failure");
	check(reopened.remove(id, &error) && error.isEmpty() && reopened.load().size() == 1 && reopened.load()[0].id == otherId,
	      "deleting a preset removes only that stable id");
	const QByteArray beforeAbsent = readBytes(path);
	check(reopened.remove(QStringLiteral("already absent"), &error) && error.isEmpty() && readBytes(path) == beforeAbsent,
	      "deleting an already absent id does not rewrite unrelated presets");
	const QByteArray beforeLock = readBytes(path);
	{
		QLockFile lock(path + QStringLiteral(".lock"));
		check(lock.tryLock(), "a second editor's active write lock can be simulated");
		check(reopened.save(QStringLiteral("blocked"), crt, reg, &error).isEmpty() && !error.isEmpty() && readBytes(path) == beforeLock,
		      "a busy store reports its lock and preserves the complete existing file");
		check(!reopened.remove(otherId, &error) && !error.isEmpty() && readBytes(path) == beforeLock,
		      "deleting during another editor's write cannot drop its changes");
	}
	ComponentInstance huge = crt;
	huge.props.insert(QStringLiteral("large"), QString(17 * 1024 * 1024, QLatin1Char('x')));
	check(reopened.save(QStringLiteral("too large"), huge, reg, &error).isEmpty() && !error.isEmpty() && readBytes(path) == beforeLock,
	      "an oversized write is rejected without truncating existing presets");
	ComponentRegistry absent;
	QJsonObject opaque{{QStringLiteral("type"), QStringLiteral("plugin.missing")}, {QStringLiteral("version"), 9},
		{QStringLiteral("future"), QJsonArray{QStringLiteral("unrecognized"), QJsonObject{{QStringLiteral("nested"), 8}}}},
		{QStringLiteral("props"), QJsonObject{{QStringLiteral("external"), QStringLiteral("file-name")}}}};
	const QJsonObject futureKey{{QStringLiteral("t"), 14}, {QStringLiteral("v"), .5},
				    {QStringLiteral("newField"), QStringLiteral("keep")}};
	opaque.insert(QStringLiteral("keys"), QJsonObject{{QStringLiteral("channel"), QJsonArray{futureKey}}});
	const QString unknownId = reopened.save(QStringLiteral("Missing plugin"), componentFromJson(opaque, absent), absent, &error);
	QJsonObject unknownSaved;
	for (const Preset &preset : reopened.load())
		if (preset.id == unknownId)
			unknownSaved = preset.component;
	check(!unknownId.isEmpty() && unknownSaved == opaque,
	      "a missing plugin's schema version, custom payload and future key fields survive saving unchanged");
	QJsonObject root = QJsonDocument::fromJson(readBytes(path)).object();
	root.insert(QStringLiteral("futureRoot"), QStringLiteral("preserve"));
	QJsonArray entries = root.value(QStringLiteral("presets")).toArray();
	QJsonObject entry = entries[0].toObject();
	entry.insert(QStringLiteral("futureEntry"), 42);
	entries[0] = entry;
	root.insert(QStringLiteral("presets"), entries);
	check(writeBytes(path, json(root)), "a supported-version fixture with extra envelope fields can be installed");
	check(!reopened.save(QStringLiteral("MINHA TELEVISÃO 📺"), other, reg, &error).isEmpty() &&
	      QJsonDocument::fromJson(readBytes(path)).object().value(QStringLiteral("futureRoot")).toString() == QStringLiteral("preserve") &&
	      QJsonDocument::fromJson(readBytes(path)).object().value(QStringLiteral("presets")).toArray()[0].toObject().value(QStringLiteral("futureEntry")).toInt() == 42,
	      "a normal update retains unrecognized root and preset envelope fields");
	int migrations = 0;
	auto plugin = type(QStringLiteral("plugin.missing"));
	plugin.version = 10;
	plugin.migrate = [&](int, QJsonObject &) { ++migrations; };
	reg.add(plugin);
	check(!reopened.load().isEmpty() && !reopened.save(QStringLiteral("another"), other, reg, &error).isEmpty() && migrations == 0,
	      "browsing or saving unrelated presets never runs plugin migrations");
	const ComponentInstance oldSchema = componentFromJson(opaque, absent);
	const QString pendingId = reopened.save(QStringLiteral("Pending migration"), oldSchema, reg, &error);
	QJsonObject pending;
	for (const Preset &preset : reopened.load())
		if (preset.id == pendingId)
			pending = preset.component;
	check(!pendingId.isEmpty() && pending == opaque && migrations == 0,
	      "saving a plugin awaiting migration preserves its old schema without injecting incompatible defaults");
	for (const QByteArray &bad : {QByteArrayLiteral("{truncated"), QByteArrayLiteral("[]"),
		QByteArrayLiteral("{\"version\":2,\"presets\":[]}"), QByteArrayLiteral("{\"version\":1.5,\"presets\":[]}"),
		QByteArrayLiteral("{\"version\":1,\"presets\":{}}"),
		QByteArrayLiteral("{\"version\":1,\"presets\":[{\"id\":\"a\",\"name\":\"Broken\",\"component\":{}}]}")}) {
		check(writeBytes(path, bad), "an invalid or future preset file fixture can be installed");
		check(reopened.load(&error).isEmpty() && !error.isEmpty() &&
		      reopened.save(QStringLiteral("do not overwrite"), crt, reg, &error).isEmpty() && !error.isEmpty() &&
		      !reopened.remove(otherId, &error) && !error.isEmpty() && readBytes(path) == bad,
		      "invalid or newer preset data reports errors and remains byte-for-byte unchanged on edits");
	}
	const QByteArray duplicate = QByteArrayLiteral("{\"version\":1,\"presets\":[{\"id\":\"a\",\"name\":\"First\",\"component\":{\"type\":\"x\"}},{\"id\":\"a\",\"name\":\"Second\",\"component\":{\"type\":\"y\"}}]}");
	writeBytes(path, duplicate);
	check(reopened.load(&error).isEmpty() && !error.isEmpty() && readBytes(path) == duplicate,
	      "duplicate stable ids are rejected before one preset can replace the wrong entry");
	QTemporaryDir blocked;
	const QString obstruction = blocked.filePath(QStringLiteral("not-a-folder"));
	writeBytes(obstruction, QByteArrayLiteral("untouched"));
	Store blockedStore(obstruction + QStringLiteral("/presets.json"));
	check(blockedStore.save(QStringLiteral("blocked"), crt, reg, &error).isEmpty() && !error.isEmpty() && readBytes(obstruction) == QByteArrayLiteral("untouched"),
	      "a failed destination-folder creation leaves the obstructing file intact");
	Store unavailable{QString()};
	check(unavailable.load(&error).isEmpty() && !error.isEmpty() && unavailable.save(QStringLiteral("blocked"), crt, reg, &error).isEmpty(),
	      "an unavailable config location cannot accidentally save in the working directory");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
