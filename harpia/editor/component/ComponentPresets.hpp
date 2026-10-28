#pragma once

// Named component snapshots belong to the user, rather than to a project. Keep
// the component's serialized form until it is applied so plugin migrations run
// with the installed type, not while browsing or saving an unrelated preset.

#include "ComponentJson.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <utility>

namespace harpia::component_presets {

struct Preset {
	QString id;
	QString name;
	QJsonObject component;
};

inline QString defaultPath()
{
	const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	return base.isEmpty() ? QString() : base + QStringLiteral("/harpia/component-presets.json");
}

class Store {
public:
	explicit Store(QString path = defaultPath()) : path_(std::move(path)) {}
	const QString &path() const { return path_; }

	QVector<Preset> load(QString *error = nullptr) const
	{
		QJsonObject root;
		if (!read(root, error))
			return {};
		QVector<Preset> presets;
		const QJsonArray array = root.value(QStringLiteral("presets")).toArray();
		presets.reserve(array.size());
		for (const QJsonValue value : array) {
			const QJsonObject object = value.toObject();
			presets.append({object.value(QStringLiteral("id")).toString(),
					object.value(QStringLiteral("name")).toString(),
					object.value(QStringLiteral("component")).toObject()});
		}
		return presets;
	}

	// A duplicate name replaces only the same component type and keeps its id.
	// The UI can ask before replacing by comparing against load(). Writes read
	// the latest file under a lock, so a second editor cannot lose other presets.
	QString save(const QString &name, const ComponentInstance &instance,
		     const ComponentRegistry &reg, QString *error = nullptr) const
	{
		clearError(error);
		const QString trimmed = name.trimmed();
		if (trimmed.isEmpty() || trimmed.size() > 256 || instance.typeId.trimmed().isEmpty()) {
			fail(error, QStringLiteral("Enter a preset name (1–256 characters) and a component type."));
			return {};
		}
		if (!prepareDirectory(error))
			return {};
		QLockFile lock(path_ + QStringLiteral(".lock"));
		if (!lock.tryLock(250)) {
			fail(error, QStringLiteral("Component presets are being changed in another window. Try again."));
			return {};
		}
		QJsonObject root;
		if (!read(root, error))
			return {};
		QJsonArray array = root.value(QStringLiteral("presets")).toArray();
		QString id;
		int replace = -1;
		for (int i = 0; i < array.size(); ++i) {
			const QJsonObject old = array[i].toObject();
			if (old.value(QStringLiteral("name")).toString().compare(trimmed, Qt::CaseInsensitive) == 0 &&
			    old.value(QStringLiteral("component")).toObject().value(QStringLiteral("type")).toString() == instance.typeId) {
				id = old.value(QStringLiteral("id")).toString();
				replace = i;
				break;
			}
		}
		if (id.isEmpty())
			id = QUuid::createUuid().toString(QUuid::WithoutBraces);
		ComponentInstance snapshot = instance;
		// A component retained while its plugin was missing still owns that old
		// schema. Let its migration fill new fields when it is applied; inserting
		// today's defaults here could prevent that migration detecting an absent
		// field. Ordinary live components have already migrated and have no raw
		// version, so their omitted defaults can safely be frozen.
		if (const ComponentType *type = reg.find(instance.typeId);
		    type && (!snapshot.unknown.contains(QStringLiteral("version")) ||
			     snapshot.unknown.value(QStringLiteral("version")).toInt() == type->version))
			for (const PropDef &def : type->props)
				if (!snapshot.props.contains(def.key))
					snapshot.props.insert(def.key, def.def);
		QJsonObject component = componentToJson(snapshot, reg);
		component.remove(QStringLiteral("id")); // each applied copy gets its own identity
		QJsonObject object = replace >= 0 ? array[replace].toObject() : QJsonObject();
		object.insert(QStringLiteral("id"), id);
		object.insert(QStringLiteral("name"), trimmed);
		object.insert(QStringLiteral("component"), component);
		if (replace >= 0)
			array.replace(replace, object);
		else
			array.append(object);
		root.insert(QStringLiteral("presets"), array);
		return write(root, error) ? id : QString();
	}

	bool remove(const QString &id, QString *error = nullptr) const
	{
		clearError(error);
		if (!prepareDirectory(error))
			return false;
		QLockFile lock(path_ + QStringLiteral(".lock"));
		if (!lock.tryLock(250)) {
			fail(error, QStringLiteral("Component presets are being changed in another window. Try again."));
			return false;
		}
		QJsonObject root;
		if (!read(root, error))
			return false;
		QJsonArray array = root.value(QStringLiteral("presets")).toArray();
		for (int i = 0; i < array.size(); ++i)
			if (array[i].toObject().value(QStringLiteral("id")).toString() == id) {
				array.removeAt(i);
				root.insert(QStringLiteral("presets"), array);
				return write(root, error);
			}
		return true; // already absent is harmless
	}

private:
	static constexpr qint64 maxBytes = 16 * 1024 * 1024;
	QString path_;
	static void clearError(QString *error) { if (error) error->clear(); }
	static bool fail(QString *error, const QString &message)
	{
		if (error)
			*error = message;
		return false;
	}
	bool prepareDirectory(QString *error) const
	{
		if (path_.isEmpty())
			return fail(error, QStringLiteral("The component preset folder is unavailable."));
		if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
			return fail(error, QStringLiteral("Could not create the component preset folder: %1").arg(QFileInfo(path_).absolutePath()));
		return true;
	}
	bool read(QJsonObject &root, QString *error) const
	{
		clearError(error);
		if (path_.isEmpty())
			return fail(error, QStringLiteral("The component preset folder is unavailable."));
		const QFileInfo info(path_);
		if (!info.exists() && !info.isSymLink()) {
			root = {{QStringLiteral("version"), 1}, {QStringLiteral("presets"), QJsonArray()}};
			return true;
		}
		QFile file(path_);
		if (!info.isFile() || !file.open(QIODevice::ReadOnly))
			return fail(error, QStringLiteral("Could not read component presets: %1").arg(path_));
		if (file.size() > maxBytes)
			return fail(error, QStringLiteral("Component presets exceed the 16 MB size limit."));
		const QByteArray bytes = file.read(maxBytes + 1);
		if (file.error() != QFileDevice::NoError || bytes.size() > maxBytes)
			return fail(error, QStringLiteral("Could not read the complete component preset file."));
		QJsonParseError parseError;
		const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
		if (parseError.error != QJsonParseError::NoError || !document.isObject())
			return fail(error, QStringLiteral("The component preset file is invalid. It has been left unchanged."));
		root = document.object();
		if (root.value(QStringLiteral("version")).toDouble(-1) != 1.0)
			return fail(error, QStringLiteral("This component preset file uses an unsupported version. It has been left unchanged."));
		if (!root.value(QStringLiteral("presets")).isArray())
			return fail(error, QStringLiteral("The component preset list is invalid. It has been left unchanged."));
		QSet<QString> ids;
		for (const QJsonValue value : root.value(QStringLiteral("presets")).toArray()) {
			const QJsonObject object = value.toObject();
			const QString id = object.value(QStringLiteral("id")).toString();
			const QString name = object.value(QStringLiteral("name")).toString();
			const QJsonValue component = object.value(QStringLiteral("component"));
			if (!value.isObject() || id.isEmpty() || ids.contains(id) || name.trimmed().isEmpty() ||
			    !component.isObject() || component.toObject().value(QStringLiteral("type")).toString().trimmed().isEmpty())
				return fail(error, QStringLiteral("A component preset is invalid. The file has been left unchanged."));
			ids.insert(id);
		}
		return true;
	}
	bool write(const QJsonObject &root, QString *error) const
	{
		const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
		if (bytes.size() > maxBytes)
			return fail(error, QStringLiteral("Component presets exceed the 16 MB size limit."));
		QSaveFile file(path_);
		file.setDirectWriteFallback(false);
		if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
			return fail(error, QStringLiteral("Could not save component presets: %1").arg(file.errorString()));
		return true;
	}
};

} // namespace harpia::component_presets
