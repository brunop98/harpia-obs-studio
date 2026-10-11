// The local remote control (core/RemoteControl, core/RemoteSession).
//
//   * Requests parse, in one piece or arriving in fragments; malformed or
//     oversized ones are refused.
//   * Web pages are refused (Origin header, a foreign Host, anything but
//     GET /status without the X-Harpia-Client header -- the recordings list
//     and thumbnails included); Unity/curl-style requests are accepted.
//   * Each command reaches the recorder with its arguments; wrong paths,
//     methods and bodies get 404/405/400.
//   * Over real sockets on 127.0.0.1: start/pause/resume/stop/status/show,
//     a second command while one is still running answers "busy".
//   * The area outline (/area/show) lands where the area is on each screen,
//     scaled, and only on the screens it touches; it goes away by itself.
//   * The session rules: a remote start never takes over a hand-started
//     recording; only its own recording can be paused or stopped; repeats are
//     harmless; a stop can ask for a short run to be deleted.
//   * The recordings: list (limit, query decoding), thumbnail (binary PNG over
//     a real socket), delete and edit reach the recorder; only files Harpia
//     listed can be deleted, and the answer says what happened to each.
#include "core/RemoteClips.hpp"
#include "core/RemoteControl.hpp"
#include "core/RemoteSession.hpp"
#include "ui/AreaFlash.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

struct FakeRecorder : RemoteHandler {
	QStringList calls;
	RemoteStartRequest lastStart;
	int lastStopMs = -1;
	QRect lastShown;
	std::function<void()> duringStart; // runs inside remoteStart (e.g. a nested event loop)

	QJsonObject remoteStatus() override { return {{QStringLiteral("state"), QStringLiteral("idle")}}; }
	RemoteReply remoteStart(const RemoteStartRequest &req) override
	{
		calls << QStringLiteral("start");
		lastStart = req;
		if (duringStart)
			duringStart();
		return RemoteReply::ok({{QStringLiteral("state"), QStringLiteral("recording")}});
	}
	RemoteReply remotePause() override
	{
		calls << QStringLiteral("pause");
		return RemoteReply::ok();
	}
	RemoteReply remoteResume() override
	{
		calls << QStringLiteral("resume");
		return RemoteReply::ok();
	}
	RemoteReply remoteStop(int ms) override
	{
		calls << QStringLiteral("stop");
		lastStopMs = ms;
		return RemoteReply::fail(409, QStringLiteral("no recording started by this client"));
	}
	RemoteReply remoteShowArea(const QRect &r) override
	{
		calls << QStringLiteral("show");
		lastShown = r;
		return RemoteReply::ok();
	}
	int lastLimit = -1;
	QString lastThumb, lastEdit;
	QStringList lastDelete;
	RemoteReply remoteRecordings(int limit) override
	{
		calls << QStringLiteral("list");
		lastLimit = limit;
		return RemoteReply::ok({{QStringLiteral("recordings"), QJsonArray()}, {QStringLiteral("total"), 0}});
	}
	RemoteReply remoteThumbnail(const QString &path) override
	{
		calls << QStringLiteral("thumb");
		lastThumb = path;
		QImage img(16, 9, QImage::Format_RGB32);
		img.fill(Qt::red);
		return RemoteReply::bytes(remote_clips::png(img), "image/png");
	}
	RemoteReply remoteDelete(const QStringList &paths) override
	{
		calls << QStringLiteral("delete");
		lastDelete = paths;
		return RemoteReply::ok();
	}
	RemoteReply remoteEdit(const QString &path) override
	{
		calls << QStringLiteral("edit");
		lastEdit = path;
		return RemoteReply::ok();
	}
};

static HttpRequest req(const char *method, const char *path, const QByteArray &body = {},
		       QHash<QByteArray, QByteArray> headers = {})
{
	HttpRequest r;
	r.method = method;
	r.path = path;
	r.body = body;
	r.headers = headers;
	if (!r.headers.contains("host"))
		r.headers.insert("host", "127.0.0.1:47811");
	if (!r.headers.contains("x-harpia-client") && r.method == "POST")
		r.headers.insert("x-harpia-client", "test");
	return r;
}

// One raw request over a real socket; returns the whole response.
static QByteArray roundTrip(quint16 port, const QByteArray &raw, int splitAt = -1)
{
	QTcpSocket s;
	s.connectToHost(QStringLiteral("127.0.0.1"), port);
	QElapsedTimer t;
	t.start();
	while (s.state() != QAbstractSocket::ConnectedState && t.elapsed() < 2000)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	if (splitAt > 0) {
		s.write(raw.left(splitAt));
		s.flush();
		for (int i = 0; i < 10; ++i) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
			QThread::msleep(5);
		}
		s.write(raw.mid(splitAt));
	} else {
		s.write(raw);
	}
	QByteArray out;
	while (t.elapsed() < 3000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		out += s.readAll();
		if (s.state() == QAbstractSocket::UnconnectedState)
			break;
	}
	return out + s.readAll();
}

static QByteArray post(const char *path, const QByteArray &body, quint16 port, bool clientHeader = true)
{
	QByteArray r = QByteArray("POST ") + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
		       "\r\nContent-Type: application/json\r\n";
	if (clientHeader)
		r += "X-Harpia-Client: Unity test\r\n";
	r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
	return r;
}

static QJsonObject jsonOf(const QByteArray &response)
{
	const int i = response.indexOf("\r\n\r\n");
	return QJsonDocument::fromJson(i < 0 ? QByteArray() : response.mid(i + 4)).object();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- parsing --\n");
	{
		HttpRequest r;
		const QByteArray full = "POST /record/stop?x=1 HTTP/1.1\r\nHost: 127.0.0.1:47811\r\nContent-Length: "
					"2\r\nX-Harpia-Client: a\r\n\r\n{}";
		ok(parseHttpRequest(full, &r) == HttpParse::Complete, "a whole request");
		ok(r.method == "POST" && r.path == "/record/stop" && r.body == "{}", "method, path (no query), body");
		ok(r.query == "x=1", "the query, kept apart");
		ok(r.headers.value("x-harpia-client") == "a", "header names in lower case");
		ok(parseHttpRequest(full.left(30), &r) == HttpParse::Incomplete, "half a head: wait for more");
		ok(parseHttpRequest(full.left(full.size() - 1), &r) == HttpParse::Incomplete, "half a body: wait");
		ok(parseHttpRequest("GARBAGE\r\n\r\n", &r) == HttpParse::Bad, "garbage is refused");
		ok(parseHttpRequest("POST / HTTP/1.1\r\nContent-Length: 999999\r\n\r\n", &r) == HttpParse::Bad,
		   "an oversized body is refused");
	}

	std::printf("\n-- who may talk --\n");
	{
		ok(untrustedReason(req("POST", "/record/start"), 47811).isEmpty(), "Unity/curl style: accepted");
		ok(untrustedReason(req("GET", "/status", {}, {{"host", "localhost:47811"}}), 47811).isEmpty(),
		   "localhost works too");
		ok(!untrustedReason(req("POST", "/record/start", {}, {{"origin", "https://evil.example"}}), 47811).isEmpty(),
		   "a web page (Origin header) is refused");
		ok(!untrustedReason(req("POST", "/record/start", {}, {{"host", "evil.example:47811"}}), 47811).isEmpty(),
		   "a re-pointed domain (foreign Host) is refused");
		HttpRequest noHeader = req("POST", "/record/start");
		noHeader.headers.remove("x-harpia-client");
		ok(!untrustedReason(noHeader, 47811).isEmpty(), "a command without X-Harpia-Client is refused");
		ok(untrustedReason(req("GET", "/status"), 47811).isEmpty(), "status needs no client header (curl)");
		// An <img> or <script> tag sends no Origin; the header keeps it out.
		ok(!untrustedReason(req("GET", "/recordings"), 47811).isEmpty(), "the list needs the client header");
		ok(!untrustedReason(req("GET", "/recordings/thumbnail"), 47811).isEmpty(), "so do thumbnails");
		ok(untrustedReason(req("GET", "/recordings", {}, {{"x-harpia-client", "Unity"}}), 47811).isEmpty(),
		   "with it: accepted");
	}

	std::printf("\n-- routing --\n");
	{
		FakeRecorder f;
		RemoteReply r = dispatchRemote(
			req("POST", "/record/start",
			    R"({"x":100,"y":50.6,"width":1280,"height":720,"client":"Unity 6000.0","label":"Arena"})"),
			f);
		ok(r.status == 200 && f.lastStart.areaPx == QRect(100, 51, 1280, 720), "start carries the area (rounded)");
		ok(f.lastStart.client == QStringLiteral("Unity 6000.0") && f.lastStart.label == QStringLiteral("Arena"),
		   "and who asked, and the label");
		ok(dispatchRemote(req("POST", "/record/start", R"({"x":1})"), f).status == 400, "start without an area: 400");
		ok(dispatchRemote(req("POST", "/record/start", "not json"), f).status == 400, "a body that is not JSON: 400");
		dispatchRemote(req("POST", "/record/stop", R"({"discardIfShorterThanMs":3000})"), f);
		ok(f.lastStopMs == 3000, "stop carries the minimum length");
		dispatchRemote(req("POST", "/record/stop"), f);
		ok(f.lastStopMs == 0, "and defaults to keeping everything");
		ok(dispatchRemote(req("GET", "/record/start"), f).status == 405, "a command by GET: 405");
		ok(dispatchRemote(req("POST", "/status"), f).status == 405, "status by POST: 405");
		ok(dispatchRemote(req("POST", "/nope"), f).status == 404, "an unknown path: 404");
		dispatchRemote(req("POST", "/area/show", R"({"x":0,"y":0,"width":640,"height":360})"), f);
		ok(f.lastShown == QRect(0, 0, 640, 360), "show carries the area");
	}

	std::printf("\n-- recordings: routing --\n");
	{
		FakeRecorder f;
		HttpRequest list = req("GET", "/recordings");
		dispatchRemote(list, f);
		ok(f.lastLimit == kRemoteListDefault, "the list without ?limit: the default");
		list.query = "limit=5";
		dispatchRemote(list, f);
		ok(f.lastLimit == 5, "?limit=5");
		list.query = "limit=999999";
		dispatchRemote(list, f);
		ok(f.lastLimit == kRemoteListMax, "an absurd limit is capped");
		list.query = "limit=0";
		dispatchRemote(list, f);
		ok(f.lastLimit == 1, "and zero is one");
		ok(dispatchRemote(req("POST", "/recordings"), f).status == 405, "the list by POST: 405");

		HttpRequest thumb = req("GET", "/recordings/thumbnail");
		ok(dispatchRemote(thumb, f).status == 400, "a thumbnail without ?path=: 400");
		thumb.query = "path=C%3A%2FUsers%2FAna%20Lu%2FV%C3%ADdeos%2Fa%2Bb%20%231.mp4";
		const RemoteReply t = dispatchRemote(thumb, f);
		ok(f.lastThumb == QStringLiteral("C:/Users/Ana Lu/V\u00eddeos/a+b #1.mp4"),
		   "?path= decoded: spaces, accents, + and # survive");
		ok(t.contentType == "image/png" && t.raw.startsWith("\x89PNG"), "the answer is a PNG");

		dispatchRemote(req("POST", "/recordings/delete", R"({"path":"/v/a.mp4"})"), f);
		ok(f.lastDelete == QStringList{QStringLiteral("/v/a.mp4")}, "delete one");
		dispatchRemote(req("POST", "/recordings/delete", R"({"paths":["/v/a.mp4","/v/b.mp4","/v/a.mp4"]})"), f);
		ok(f.lastDelete == (QStringList{QStringLiteral("/v/a.mp4"), QStringLiteral("/v/b.mp4")}),
		   "delete several, repeats once");
		ok(dispatchRemote(req("POST", "/recordings/delete", "{}"), f).status == 400, "delete nothing: 400");
		QJsonArray many;
		for (int i = 0; i <= kRemoteDeleteMax; ++i)
			many.append(QStringLiteral("/v/%1.mp4").arg(i));
		ok(dispatchRemote(req("POST", "/recordings/delete",
				      QJsonDocument(QJsonObject{{QStringLiteral("paths"), many}}).toJson()),
				  f)
				   .status == 400,
		   "too many at once: 400");
		ok(dispatchRemote(req("GET", "/recordings/delete"), f).status == 405, "delete by GET: 405");
		dispatchRemote(req("POST", "/recordings/edit", R"({"path":"/v/a.mp4"})"), f);
		ok(f.lastEdit == QStringLiteral("/v/a.mp4"), "edit");
		ok(dispatchRemote(req("POST", "/recordings/edit", "{}"), f).status == 400, "edit nothing: 400");
		ok(dispatchRemote(req("GET", "/recordings/nope", {}, {{"x-harpia-client", "u"}}), f).status == 404,
		   "an unknown recordings path: 404");
	}

	std::printf("\n-- recordings: which files --\n");
	{
		QTemporaryDir tmp;
		const QString dir = tmp.path();
		const auto make = [&](const QString &name) {
			QFile f(dir + QLatin1Char('/') + name);
			f.open(QIODevice::WriteOnly);
			f.write("x");
			return f.fileName();
		};
		const QString a = make(QStringLiteral("Arena 1.mp4"));
		const QString b = make(QStringLiteral("Arena 2.mkv"));
		const QString w = make(QStringLiteral("Arena 2_webcam.mp4"));
		const QString m = make(QStringLiteral("voice.m4a"));
		const QString stranger = make(QStringLiteral("notes.txt"));
		ClipLibrary::PresetByFolder presets;
		presets.insert(dir, QStringLiteral("Unity"));
		QVector<ClipInfo> clips = ClipLibrary::scan({dir}, presets);
		ok(clips.size() == 3, "the library: the recordings, not the webcam companion or other files");

		clips[0].durationMs = 61000;
		const QJsonObject one = remote_clips::toJson(clips[0]);
		ok(one.value(QStringLiteral("path")).toString() == clips[0].filePath &&
			   one.value(QStringLiteral("name")).toString() == clips[0].fileName &&
			   one.value(QStringLiteral("folder")).toString() == QDir(dir).absolutePath(),
		   "a clip: path, name, folder");
		ok(one.value(QStringLiteral("durationMs")).toDouble() == 61000 &&
			   !one.value(QStringLiteral("duration")).toString().isEmpty() &&
			   one.value(QStringLiteral("preset")).toString() == QStringLiteral("Unity") &&
			   one.value(QStringLiteral("sizeBytes")).toDouble() == 1,
		   "duration, preset, size");
		ok(one.value(QStringLiteral("modified")).toString().endsWith(QLatin1Char('Z')) &&
			   one.value(QStringLiteral("modifiedMs")).toDouble() > 0 &&
			   !one.value(QStringLiteral("age")).toString().isEmpty(),
		   "when (UTC, ms, and in words)");
		const int audio = remote_clips::find(clips, m);
		ok(audio >= 0 && remote_clips::toJson(clips[audio]).value(QStringLiteral("kind")).toString() ==
					 QStringLiteral("audio"),
		   "an audio recording says so");

		const QJsonObject list = remote_clips::listJson(clips, 2, true);
		ok(list.value(QStringLiteral("recordings")).toArray().size() == 2 &&
			   list.value(QStringLiteral("total")).toInt() == 3 &&
			   list.value(QStringLiteral("pending")).toBool(),
		   "the list: at most `limit`, the total, and whether to ask again");

		ok(remote_clips::find(clips, a) >= 0, "a listed path is found");
		ok(remote_clips::find(clips, dir + QStringLiteral("/sub/../Arena 1.mp4")) >= 0, "also when not cleaned");
		ok(remote_clips::find(clips, QDir::toNativeSeparators(a)) >= 0, "or with native separators");
		ok(remote_clips::find(clips, stranger) < 0 && remote_clips::find(clips, w) < 0,
		   "a file that is not a recording is not");
#ifndef Q_OS_WIN
		ok(remote_clips::find(clips, a.toUpper()) < 0, "case matters where the file system cares");
#endif

		QStringList removed;
		const auto remove = [&](const QString &p) {
			removed << p;
			return QFile::remove(p);
		};
		RemoteReply r = remote_clips::deleteClips(clips, {stranger, QStringLiteral("/etc/passwd")}, remove);
		ok(r.status == 404 && removed.isEmpty() && QFile::exists(stranger),
		   "files Harpia did not list: 404, nothing touched");
		ok(r.body.value(QStringLiteral("failed")).toArray().size() == 2 &&
			   r.body.value(QStringLiteral("error")).toString().contains(QStringLiteral("not one of")),
		   "and each one is named");

		r = remote_clips::deleteClips(clips, {b}, remove, [&](const QString &p) {
			return p == clips[remote_clips::find(clips, b)].filePath ? QStringLiteral("it is being recorded")
										 : QString();
		});
		ok(r.status == 409 && removed.isEmpty() && QFile::exists(b), "a refused file: 409, left alone");
		ok(r.body.value(QStringLiteral("error")).toString().contains(QStringLiteral("being recorded")),
		   "with the reason");

		r = remote_clips::deleteClips(clips, {a, stranger}, remove);
		ok(r.status == 409 && !QFile::exists(a) && QFile::exists(stranger),
		   "a mix: the recording goes, the stranger stays");
		ok(r.body.value(QStringLiteral("deleted")).toArray().size() == 1 &&
			   r.body.value(QStringLiteral("failed")).toArray().size() == 1,
		   "and the answer says which is which");

		r = remote_clips::deleteClips(clips, {m}, [](const QString &) { return false; });
		ok(r.status == 409 && r.body.value(QStringLiteral("error")).toString().contains(QStringLiteral("open in")),
		   "a file that will not go (open elsewhere): 409, says why");

		r = remote_clips::deleteClips(clips, {QDir::toNativeSeparators(b)}, remove);
		ok(r.status == 200 && r.body.value(QStringLiteral("ok")).toBool() && !QFile::exists(b),
		   "a recording: deleted, ok");
		ok(r.body.value(QStringLiteral("deleted")).toArray().first().toString() == clips[remote_clips::find(clips, b)].filePath,
		   "reported by the library's own spelling of its path");
	}

	std::printf("\n-- over real sockets --\n");
	{
		FakeRecorder f;
		RemoteControlServer server(f);
		quint16 port = 0;
		for (quint16 p = 47900; p < 47950 && !port; ++p)
			if (server.listen(p))
				port = p;
		ok(port != 0 && server.isListening(), "listening on 127.0.0.1");
		if (!port)
			return 1;

		QByteArray resp = roundTrip(port, "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
		ok(resp.startsWith("HTTP/1.1 200") && jsonOf(resp).value(QStringLiteral("ok")).toBool() &&
			   jsonOf(resp).value(QStringLiteral("state")).toString() == QStringLiteral("idle"),
		   "GET /status answers");

		const QByteArray start = post("/record/start", R"({"x":10,"y":20,"width":800,"height":450})", port);
		resp = roundTrip(port, start, 25);
		ok(resp.startsWith("HTTP/1.1 200") && f.lastStart.areaPx == QRect(10, 20, 800, 450),
		   "a start sent in two pieces arrives whole");
		ok(jsonOf(resp).value(QStringLiteral("state")).toString() == QStringLiteral("recording"), "with the answer");

		resp = roundTrip(port, post("/record/pause", {}, port));
		ok(resp.startsWith("HTTP/1.1 200") && f.calls.endsWith(QStringLiteral("pause")), "pause");
		resp = roundTrip(port, post("/record/resume", {}, port));
		ok(resp.startsWith("HTTP/1.1 200") && f.calls.endsWith(QStringLiteral("resume")), "resume");
		resp = roundTrip(port, post("/record/stop", R"({"discardIfShorterThanMs":2500})", port));
		ok(resp.startsWith("HTTP/1.1 409") && f.lastStopMs == 2500, "stop, and an error comes back as 409");
		ok(jsonOf(resp).value(QStringLiteral("error")).toString().contains(QStringLiteral("no recording")),
		   "with the reason");

		const QByteArray thumbReq = "GET /recordings/thumbnail?path=%2Fv%2Fa%20b.mp4 HTTP/1.1\r\nHost: "
					    "127.0.0.1\r\nX-Harpia-Client: Unity test\r\n\r\n";
		resp = roundTrip(port, thumbReq);
		const int bodyAt = resp.indexOf("\r\n\r\n") + 4;
		ok(resp.startsWith("HTTP/1.1 200") && resp.contains("Content-Type: image/png\r\n") &&
			   resp.mid(bodyAt).startsWith("\x89PNG") && f.lastThumb == QStringLiteral("/v/a b.mp4"),
		   "a thumbnail comes back as PNG bytes");
		ok(resp.contains("Content-Length: " + QByteArray::number(resp.size() - bodyAt) + "\r\n"),
		   "with its exact length");
		resp = roundTrip(port, "GET /recordings HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
		ok(resp.startsWith("HTTP/1.1 403"), "the list without the client header: refused");

		const int before = f.calls.size();
		resp = roundTrip(port, post("/record/start", R"({"x":0,"y":0,"width":8,"height":8})", port, false));
		ok(resp.startsWith("HTTP/1.1 403") && f.calls.size() == before, "no client header: refused, nothing run");
		QByteArray browser = post("/record/start", R"({"x":0,"y":0,"width":8,"height":8})", port);
		browser.replace("\r\n\r\n", "\r\nOrigin: http://example.com\r\n\r\n");
		resp = roundTrip(port, browser);
		ok(resp.startsWith("HTTP/1.1 403") && f.calls.size() == before, "a browser request: refused");
		resp = roundTrip(port, "NONSENSE\r\n\r\n");
		ok(resp.startsWith("HTTP/1.1 400"), "garbage: 400");

		// A start that opens a dialog spins an event loop; a stop arriving
		// then must not run in the middle of it.
		QByteArray nestedResp;
		f.duringStart = [&]() { nestedResp = roundTrip(port, post("/record/stop", {}, port)); };
		resp = roundTrip(port, start);
		f.duringStart = nullptr;
		ok(resp.startsWith("HTTP/1.1 200"), "the first command completes");
		ok(nestedResp.startsWith("HTTP/1.1 503"), "a command arriving meanwhile is answered 'busy'");

		RemoteControlServer second(f);
		ok(!second.listen(port), "a second server cannot take the same port");
		ok(!second.errorString().isEmpty(), "and says why");
	}

	std::printf("\n-- the area outline --\n");
	{
		ok(AreaFlash::toLocal(QRect(1920 + 300, 150, 1200, 600), QRect(1920, 0, 2880, 1620), 1.5) ==
			   QRectF(200, 100, 800, 400),
		   "a 150% screen: physical px become its logical coordinates");
		QScreen *scr = QGuiApplication::primaryScreen();
		const double dpr = scr->devicePixelRatio();
		const QRect phys(scr->geometry().topLeft() * dpr, scr->geometry().size() * dpr);
		const QRect area(phys.x() + 40, phys.y() + 30, 320, 180);
		QVector<QWidget *> layers = AreaFlash::show(area, {{scr, QRect()}}, 300, QStringLiteral("320 x 180"));
		ok(layers.size() == 1 && layers.front()->isVisible(), "one layer, over the screen holding the area");
		QPointer<QWidget> layer = layers.value(0);
		ok(AreaFlash::show(QRect(phys.right() + 5000, 0, 100, 100), {{scr, QRect()}}, 300, {}).isEmpty(),
		   "an area on no screen: nothing shown");
		QElapsedTimer t;
		t.start();
		while (layer && t.elapsed() < 2000) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		}
		ok(!layer, "and it goes away by itself");
	}

	std::printf("\n-- the session rules --\n");
	{
		RemoteSession s;
		RecorderSnapshot idle, rec, paused, stopping;
		rec.recording = true;
		paused.recording = paused.paused = true;
		stopping.stopping = true;
		ok(s.start(idle).proceed, "idle: a remote start goes ahead");
		RecorderSnapshot manual = rec;
		ok(!s.start(manual).proceed && !s.start(manual).ok && s.start(manual).status == 409,
		   "a hand-started recording is never taken over");
		RecorderSnapshot counting;
		counting.countingDown = true;
		ok(!s.start(counting).proceed, "nor a countdown in progress");
		ok(!s.stop(rec).proceed && s.stop(rec).status == 409, "a hand-started recording cannot be stopped remotely");
		ok(!s.pause(rec).proceed, "or paused");

		s.began();
		ok(s.owns(), "after a remote start, the session owns the recording");
		ok(!s.start(rec).proceed && s.start(rec).ok, "a repeated start is harmless");
		ok(s.pause(rec).proceed, "pause goes ahead");
		ok(!s.pause(paused).proceed && s.pause(paused).ok, "pause while paused: harmless");
		ok(s.resume(paused).proceed && !s.resume(rec).proceed && s.resume(rec).ok, "resume likewise");
		ok(s.stop(rec).proceed, "stop goes ahead");
		ok(!s.stop(stopping).proceed && s.stop(stopping).ok, "stop while stopping: harmless");

		s.stopRequested(3000);
		ok(s.discard(2500) && !s.discard(3000) && !s.discard(60000), "a run under the minimum is deleted, others kept");
		s.ended();
		ok(!s.owns() && !s.discard(10), "and once it ends, the session lets go");

		s.began();
		ok(!s.discard(100), "a stop that did not ask (e.g. Stop pressed in Harpia) keeps the file");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
