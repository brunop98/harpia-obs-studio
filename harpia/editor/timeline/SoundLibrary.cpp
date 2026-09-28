#include "SoundLibrary.hpp"

#include "SoundRules.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace harpia {

namespace {
const char *kKey = "soundLibrary/entries";
}

QVector<SoundSlot> SoundLibrary::fromJson(const QByteArray &json)
{
	// Built-ins first, always, in their order; a stored entry only supplies
	// the file behind one. Then the custom entries as stored.
	QVector<SoundSlot> out;
	for (const QString &b : SoundRules::builtinSounds()) {
		SoundSlot s;
		s.name = b;
		s.builtin = true;
		out.append(s);
	}
	const QJsonArray arr = QJsonDocument::fromJson(json).array();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		const QString name = o.value(QStringLiteral("name")).toString().trimmed();
		if (name.isEmpty())
			continue;
		const QString file = o.value(QStringLiteral("file")).toString();
		bool merged = false;
		for (SoundSlot &s : out)
			if (s.name.compare(name, Qt::CaseInsensitive) == 0) {
				s.file = file;
				merged = true;
				break;
			}
		if (!merged) {
			SoundSlot s;
			s.name = name;
			s.file = file;
			out.append(s);
		}
	}
	return out;
}

QByteArray SoundLibrary::toJson(const QVector<SoundSlot> &entries)
{
	QJsonArray arr;
	for (const SoundSlot &s : entries) {
		// A built-in with no file of yours is the default: nothing to store.
		if (s.builtin && s.file.isEmpty())
			continue;
		QJsonObject o;
		o[QStringLiteral("name")] = s.name;
		o[QStringLiteral("file")] = s.file;
		arr.append(o);
	}
	return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QVector<SoundSlot> SoundLibrary::load()
{
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	return fromJson(st.value(QString::fromLatin1(kKey)).toByteArray());
}

void SoundLibrary::save(const QVector<SoundSlot> &entries)
{
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	st.setValue(QString::fromLatin1(kKey), toJson(entries));
}

const SoundSlot *SoundLibrary::find(const QVector<SoundSlot> &entries, const QString &name)
{
	for (const SoundSlot &s : entries)
		if (s.name.compare(name, Qt::CaseInsensitive) == 0)
			return &s;
	return nullptr;
}

QString SoundLibrary::resolve(const QVector<SoundSlot> &entries, const QString &name, const QString &cacheDir)
{
	const SoundSlot *s = find(entries, name);
	if (!s)
		return QString();
	if (!s->file.isEmpty() && QFileInfo(s->file).isFile())
		return QFileInfo(s->file).absoluteFilePath();
	if (!s->builtin || cacheDir.isEmpty())
		return QString();
	QDir().mkpath(cacheDir);
	QString base = s->name.toLower();
	base.replace(QLatin1Char(' '), QLatin1Char('_'));
	const QString wav = QDir(cacheDir).filePath(base + QStringLiteral(".wav"));
	if (!QFileInfo::exists(wav) && !SoundRules::writeBuiltin(s->name, wav))
		return QString();
	return wav;
}

QString SoundLibrary::defaultCacheDir()
{
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sounds");
}

} // namespace harpia
