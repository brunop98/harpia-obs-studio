#include "core/RemoteClips.hpp"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace harpia::remote_clips {

QJsonObject toJson(const ClipInfo &clip)
{
	const QString kind = clip.isAudio ? QStringLiteral("audio")
			     : clip.isGif ? QStringLiteral("gif")
					  : QStringLiteral("video");
	return {{QStringLiteral("path"), clip.filePath},
		{QStringLiteral("name"), clip.fileName},
		{QStringLiteral("folder"), QFileInfo(clip.filePath).absolutePath()},
		{QStringLiteral("sizeBytes"), double(clip.sizeBytes)},
		{QStringLiteral("size"), clip.humanSize()},
		{QStringLiteral("modified"), clip.modified.toUTC().toString(Qt::ISODate)},
		{QStringLiteral("modifiedMs"), double(clip.modified.toMSecsSinceEpoch())},
		{QStringLiteral("age"), clip.relativeAge()},
		{QStringLiteral("date"), clip.exactDate()},
		{QStringLiteral("durationMs"), double(clip.durationMs)},
		{QStringLiteral("duration"), ClipInfo::durationString(clip.durationMs)},
		{QStringLiteral("preset"), clip.presetName},
		{QStringLiteral("kind"), kind}};
}

QJsonObject listJson(const QVector<ClipInfo> &clips, int limit, bool durationsPending)
{
	QJsonArray list;
	for (int i = 0; i < clips.size() && i < limit; ++i)
		list.append(toJson(clips[i]));
	return {{QStringLiteral("recordings"), list},
		{QStringLiteral("total"), clips.size()},
		{QStringLiteral("pending"), durationsPending}};
}

bool samePath(const QString &a, const QString &b)
{
	if (a.isEmpty() || b.isEmpty())
		return false;
	const QString ca = QDir::cleanPath(QDir::fromNativeSeparators(a));
	const QString cb = QDir::cleanPath(QDir::fromNativeSeparators(b));
#ifdef Q_OS_WIN
	return ca.compare(cb, Qt::CaseInsensitive) == 0;
#else
	return ca == cb;
#endif
}

int find(const QVector<ClipInfo> &clips, const QString &path)
{
	for (int i = 0; i < clips.size(); ++i)
		if (samePath(clips[i].filePath, path))
			return i;
	return -1;
}

RemoteReply deleteClips(const QVector<ClipInfo> &clips, const QStringList &paths,
			const std::function<bool(const QString &)> &remove,
			const std::function<QString(const QString &)> &refuse)
{
	QJsonArray deleted, failed;
	QString firstError;
	bool anyRecording = false;
	const auto fail = [&](const QString &path, const QString &why) {
		failed.append(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("error"), why}});
		if (firstError.isEmpty())
			firstError = why;
	};
	for (const QString &path : paths) {
		const int i = find(clips, path);
		const QString name = QFileInfo(path).fileName();
		if (i < 0) {
			fail(path, QStringLiteral("\"%1\" is not one of Harpia's recordings").arg(name));
			continue;
		}
		anyRecording = true;
		const QString file = clips[i].filePath; // the library's own spelling
		if (const QString why = refuse ? refuse(file) : QString(); !why.isEmpty()) {
			fail(path, QStringLiteral("\"%1\": %2").arg(name, why));
			continue;
		}
		if (remove && remove(file))
			deleted.append(file);
		else
			fail(path, QStringLiteral("Could not delete \"%1\" (is it open in another program?)").arg(name));
	}
	const QJsonObject lists{{QStringLiteral("deleted"), deleted}, {QStringLiteral("failed"), failed}};
	if (failed.isEmpty())
		return RemoteReply::ok(lists);
	RemoteReply r = RemoteReply::fail(anyRecording ? 409 : 404, firstError);
	for (auto it = lists.begin(); it != lists.end(); ++it)
		r.body.insert(it.key(), it.value());
	return r;
}

QByteArray png(const QImage &img)
{
	QByteArray out;
	QBuffer buf(&out);
	buf.open(QIODevice::WriteOnly);
	img.save(&buf, "PNG");
	return out;
}

} // namespace harpia::remote_clips
