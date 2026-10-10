// A stand-in Harpia for testing remote-control clients (the Unity package's
// HarpiaClient, run by unity/.../Tests~ under Mono): the REAL server
// (core/RemoteControl) and the REAL session rules (core/RemoteSession), with a
// pretend recorder that only remembers what it was asked.
//
//   remotehost_tool <port> [seconds]
//
// Prints "listening <port>" once ready. GET /status reports the pretend
// recorder's state plus what the last commands carried, so the client test can
// check that its requests arrived as meant. Exits after `seconds` (default 30).
#include "core/RemoteControl.hpp"
#include "core/RemoteSession.hpp"

#include <QCoreApplication>
#include <QJsonArray>
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
};

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	const quint16 port = argc > 1 ? quint16(QByteArray(argv[1]).toUInt()) : kRemoteDefaultPort;
	const int seconds = argc > 2 ? QByteArray(argv[2]).toInt() : 30;
	PretendRecorder recorder;
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
