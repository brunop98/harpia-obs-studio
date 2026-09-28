#include "SoundPresets.hpp"

#include "TimelineJson.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <algorithm>

namespace harpia {

namespace {
QString &testDir()
{
	static QString d;
	return d;
}
} // namespace

QJsonObject SoundPresets::toJson(const TimelineModel &m, const QString &name,
				 const std::function<SoundRef(int)> &refOf)
{
	QJsonObject o;
	o[QStringLiteral("harpiaPreset")] = 1;
	o[QStringLiteral("name")] = name;
	QJsonArray tags;
	for (const TlTag &t : m.tags) {
		QJsonObject to;
		to[QStringLiteral("name")] = t.name;
		to[QStringLiteral("color")] = t.color.name(QColor::HexRgb);
		tags.append(to);
	}
	o[QStringLiteral("tags")] = tags;
	QJsonArray rules;
	for (const TlSoundRule &r : m.soundRules) {
		QJsonObject ro = soundRuleToJson(r);
		// Ids mean nothing in another project.
		ro.remove(QStringLiteral("id"));
		ro.remove(QStringLiteral("source"));
		ro.remove(QStringLiteral("tag"));
		ro[QStringLiteral("lane")] = -1;
		if (r.trigger == TlSoundRule::Trigger::Tagged) {
			const QString tn = m.tagName(r.tagId);
			if (tn.isEmpty())
				continue; // a rule for a tag that is gone
			ro[QStringLiteral("tagName")] = tn;
		}
		const SoundRef ref = refOf ? refOf(r.sourceId) : SoundRef();
		if (ref.isEmpty())
			continue; // nothing to play: not worth carrying
		if (!ref.slot.isEmpty())
			ro[QStringLiteral("slot")] = ref.slot;
		else
			ro[QStringLiteral("file")] = ref.file;
		rules.append(ro);
	}
	o[QStringLiteral("rules")] = rules;
	return o;
}

PresetMergeResult SoundPresets::merge(TimelineModel &m, const QJsonObject &preset,
				      const std::function<int(const SoundRef &)> &sourceFor)
{
	PresetMergeResult res;
	for (const QJsonValue &tv : preset.value(QStringLiteral("tags")).toArray()) {
		const QJsonObject to = tv.toObject();
		const QString n = to.value(QStringLiteral("name")).toString().trimmed();
		if (n.isEmpty() || m.tagNamed(n))
			continue;
		m.ensureTag(n, QColor(to.value(QStringLiteral("color")).toString()));
		++res.tagsAdded;
	}
	for (const QJsonValue &rv : preset.value(QStringLiteral("rules")).toArray()) {
		const QJsonObject ro = rv.toObject();
		TlSoundRule r = soundRuleFromJson(ro);
		r.lane = -1;
		if (r.trigger == TlSoundRule::Trigger::Tagged) {
			const QString tn = ro.value(QStringLiteral("tagName")).toString();
			r.tagId = m.ensureTag(tn);
			if (r.tagId <= 0)
				continue;
		}
		SoundRef ref;
		ref.slot = ro.value(QStringLiteral("slot")).toString();
		ref.file = ro.value(QStringLiteral("file")).toString();
		r.sourceId = sourceFor ? sourceFor(ref) : -1;
		if (r.sourceId <= 0) {
			++res.soundsMissing;
			continue;
		}
		if (r.soundName.isEmpty())
			r.soundName = !ref.slot.isEmpty() ? ref.slot : QFileInfo(ref.file).fileName();
		bool dup = false;
		for (const TlSoundRule &e : m.soundRules)
			if (e.trigger == r.trigger && e.transitionType == r.transitionType &&
			    e.componentId == r.componentId && e.tagId == r.tagId && e.sourceId == r.sourceId) {
				dup = true;
				break;
			}
		if (dup) {
			++res.rulesSkipped;
			continue;
		}
		r.id = m.nextSoundRuleId();
		m.soundRules.append(r);
		++res.rulesAdded;
	}
	return res;
}

void SoundPresets::setDirForTesting(const QString &dir)
{
	testDir() = dir;
}

QString SoundPresets::dir()
{
	if (!testDir().isEmpty())
		return testDir();
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/presets");
}

QString SoundPresets::fileFor(const QString &name)
{
	QString f;
	for (const QChar ch : name.trimmed())
		f += (ch.isLetterOrNumber() || ch == QLatin1Char(' ') || ch == QLatin1Char('-') ||
		      ch == QLatin1Char('_'))
			     ? ch
			     : QLatin1Char('_');
	if (f.isEmpty())
		f = QStringLiteral("preset");
	return QDir(dir()).filePath(f + QStringLiteral(".json"));
}

QStringList SoundPresets::names()
{
	QStringList out;
	const QDir d(dir());
	for (const QFileInfo &fi : d.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
		QFile f(fi.absoluteFilePath());
		if (!f.open(QIODevice::ReadOnly))
			continue;
		const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
		const QString n = o.value(QStringLiteral("name")).toString();
		if (o.value(QStringLiteral("harpiaPreset")).toInt() >= 1 && !n.isEmpty())
			out << n;
	}
	std::sort(out.begin(), out.end(),
		  [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
	return out;
}

bool SoundPresets::save(const QString &name, const QJsonObject &preset)
{
	QDir().mkpath(dir());
	QJsonObject o = preset;
	o[QStringLiteral("name")] = name.trimmed();
	QFile f(fileFor(name));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	return f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) > 0;
}

QJsonObject SoundPresets::load(const QString &name)
{
	QFile f(fileFor(name));
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}

bool SoundPresets::remove(const QString &name)
{
	return QFile::remove(fileFor(name));
}

} // namespace harpia
