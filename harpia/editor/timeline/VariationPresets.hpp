#pragma once

// Saved lists of text variations: a name and the alternatives, one per line,
// so a list typed once ("I like cats / I like vultures / I like parrots") can
// be put on any caption again without retyping it.
//
// Kept in a plain, readable JSON file next to the text-style presets, so a
// list can be shared by copying the file:
//
//   { "Pets": ["I like cats", "I like vultures", "I like parrots"] }
//
// Pure file helpers: the window only adds the buttons.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QStringList>

namespace harpia {

namespace variation_presets {

// A list as it is stored: blank lines dropped (they are never a version).
inline QStringList cleaned(const QStringList &lines)
{
	QStringList out;
	for (const QString &l : lines)
		if (!l.trimmed().isEmpty())
			out << l;
	return out;
}

inline QMap<QString, QStringList> fromJson(const QByteArray &bytes)
{
	QMap<QString, QStringList> out;
	const QJsonObject root = QJsonDocument::fromJson(bytes).object();
	for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
		if (!it.value().isArray() || it.key().trimmed().isEmpty())
			continue;
		QStringList lines;
		for (const QJsonValue &v : it.value().toArray())
			if (v.isString())
				lines << v.toString();
		lines = cleaned(lines);
		if (!lines.isEmpty())
			out.insert(it.key(), lines);
	}
	return out;
}

inline QByteArray toJson(const QMap<QString, QStringList> &presets)
{
	QJsonObject root;
	for (auto it = presets.constBegin(); it != presets.constEnd(); ++it)
		root.insert(it.key(), QJsonArray::fromStringList(cleaned(it.value())));
	return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

inline QMap<QString, QStringList> load(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return fromJson(f.readAll());
}

// Written whole or not at all: a crash halfway must not cost every list.
inline bool save(const QString &path, const QMap<QString, QStringList> &presets)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(toJson(presets));
	return f.commit();
}

} // namespace variation_presets

} // namespace harpia
