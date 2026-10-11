// A stand-in Harpia for testing remote-control clients (the Unity scripts'
// HarpiaClient, run by unity/Tests under Mono): the REAL server
// (core/RemoteControl), the REAL session rules (core/RemoteSession) and the
// REAL recordings rules (core/RemoteClips over a ClipLibrary scan), with a
// pretend recorder that only remembers what it was asked.
//
//   remotehost_tool <port> [seconds] [recordings folder]
//
// Prints "listening <port>" once ready. GET /status reports the pretend
// recorder's state plus what the last commands carried, so the client test can
// check that its requests arrived as meant. Exits after `seconds` (default 30).
//
// The recordings are the folder's files. Like the app, the first list has no
// durations yet ("pending") and a thumbnail is "pending" the first time it is
// asked for; deleting removes the file outright (the app uses the recycle bin).
#include "core/RemoteClips.hpp"
#include "core/RemoteControl.hpp"
#include "core/RemoteSession.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QSet>
#include <QTimer>

#include <cstdio>

using namespace harpia;

struct PretendRecorder : RemoteHandler {
	RemoteSession session;
	RecorderSnapshot snap;
	QRect lastArea;
	QString lastClient, lastLabel;
	int lastDiscardMs = -1;
	QJsonArray calls;
	QString folder, lastEdit;
	int lists = 0;
	QSet<QString> thumbsAsked;

	QVector<ClipInfo> clips() const
	{
		if (folder.isEmpty())
			return {};
		ClipLibrary::PresetByFolder presets;
		presets.insert(folder, QStringLiteral("Test preset"));
		return ClipLibrary::scan({folder}, presets);
	}

	QString state() const
	{
		if (snap.stopping)
			return QStringLiteral("stopping");
		if (snap.recording)
			return snap.paused ? QStringLiteral("paused") : QStringLiteral("recording");
		return QStringLiteral("idle");
	}
	RemoteReply answer(const RemoteSession::Answer &a)
	{
		if (!a.ok)
			return RemoteReply::fail(a.status, a.message);
		QJsonObject o{{QStringLiteral("state"), state()}};
		if (!a.message.isEmpty())
			o.insert(QStringLiteral("note"), a.message);
		return RemoteReply::ok(o);
	}

	QJsonObject remoteStatus() override
	{
		return {{QStringLiteral("app"), QStringLiteral("Harpia test host")},
			{QStringLiteral("version"), QStringLiteral("0.0.0")},
			{QStringLiteral("state"), state()},
			{QStringLiteral("remote"), session.owns()},
			{QStringLiteral("preset"), QStringLiteral("Test \"preset\"")},
			{QStringLiteral("lastArea"),
			 QJsonObject{{QStringLiteral("x"), lastArea.x()},
				     {QStringLiteral("y"), lastArea.y()},
				     {QStringLiteral("width"), lastArea.width()},
				     {QStringLiteral("height"), lastArea.height()}}},
			{QStringLiteral("lastClient"), lastClient},
			{QStringLiteral("lastLabel"), lastLabel},
			{QStringLiteral("lastDiscardMs"), lastDiscardMs},
			{QStringLiteral("lastEdit"), lastEdit},
			{QStringLiteral("calls"), calls}};
	}
	RemoteReply remoteStart(const RemoteStartRequest &req) override
	{
		calls.append(QStringLiteral("start"));
		const auto a = session.start(snap);
		if (a.proceed) {
			lastArea = req.areaPx;
			lastClient = req.client;
			lastLabel = req.label;
			session.began();
			snap.recording = true;
			snap.paused = false;
		}
		return answer(a);
	}
	RemoteReply remotePause() override
	{
		calls.append(QStringLiteral("pause"));
		const auto a = session.pause(snap);
		if (a.proceed)
			snap.paused = true;
		return answer(a);
	}
	RemoteReply remoteResume() override
	{
		calls.append(QStringLiteral("resume"));
		const auto a = session.resume(snap);
		if (a.proceed)
			snap.paused = false;
		return answer(a);
	}
	RemoteReply remoteStop(int ms) override
	{
		calls.append(QStringLiteral("stop"));
		const auto a = session.stop(snap);
		if (a.proceed) {
			lastDiscardMs = ms;
			session.stopRequested(ms);
			snap = RecorderSnapshot{}; // a pretend recorder stops at once
			session.ended();
		}
		return answer(a);
	}
	RemoteReply remoteShowArea(const QRect &r) override
	{
		calls.append(QStringLiteral("show"));
		lastArea = r;
		return RemoteReply::ok();
	}
	RemoteReply remoteRecordings(int limit) override
	{
		calls.append(QStringLiteral("list"));
		QVector<ClipInfo> all = clips();
		const bool pending = lists++ == 0;
		if (!pending)
			for (ClipInfo &c : all)
				c.durationMs = 4200;
		return RemoteReply::ok(remote_clips::listJson(all, limit, pending));
	}
	RemoteReply remoteThumbnail(const QString &path) override
	{
		calls.append(QStringLiteral("thumb"));
		const QVector<ClipInfo> all = clips();
		const int i = remote_clips::find(all, path);
		if (i < 0)
			return RemoteReply::fail(404, QStringLiteral("not one of Harpia's recordings"));
		if (all[i].isAudio)
			return RemoteReply::fail(404, QStringLiteral("an audio recording has no picture"));
		if (!thumbsAsked.contains(all[i].filePath)) {
			thumbsAsked.insert(all[i].filePath);
			RemoteReply r = RemoteReply::fail(202, QStringLiteral("the thumbnail is being made"));
			r.body.insert(QStringLiteral("pending"), true);
			return r;
		}
		QImage img(32, 18, QImage::Format_RGB32);
		img.fill(QColor(0x30, 0x80, 0xc0));
		return RemoteReply::bytes(remote_clips::png(img), "image/png");
	}
	RemoteReply remoteDelete(const QStringList &paths) override
	{
		calls.append(QStringLiteral("delete"));
		return remote_clips::deleteClips(clips(), paths, [](const QString &p) { return QFile::remove(p); });
	}
	RemoteReply remoteEdit(const QString &path) override
	{
		calls.append(QStringLiteral("edit"));
		const QVector<ClipInfo> all = clips();
		const int i = remote_clips::find(all, path);
		if (i < 0)
			return RemoteReply::fail(404, QStringLiteral("not one of Harpia's recordings"));
		lastEdit = all[i].filePath;
		return RemoteReply::ok({{QStringLiteral("path"), lastEdit}});
	}
};

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv); // QImage needs no GUI to write a PNG
	const quint16 port = argc > 1 ? quint16(QByteArray(argv[1]).toUInt()) : kRemoteDefaultPort;
	const int seconds = argc > 2 ? QByteArray(argv[2]).toInt() : 30;
	PretendRecorder recorder;
	if (argc > 3)
		recorder.folder = QString::fromLocal8Bit(argv[3]);
	RemoteControlServer server(recorder);
	if (!server.listen(port)) {
		std::fprintf(stderr, "cannot listen on %u: %s\n", unsigned(port), qPrintable(server.errorString()));
		return 1;
	}
	std::printf("listening %u\n", unsigned(port));
	std::fflush(stdout);
	QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
	return app.exec();
}
