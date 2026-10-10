#include "core/RemoteControl.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace harpia {

// ---- Protocol -----------------------------------------------------------------

HttpParse parseHttpRequest(const QByteArray &buf, HttpRequest *out, int maxBody)
{
	const int headEnd = buf.indexOf("\r\n\r\n");
	if (headEnd < 0)
		return buf.size() > 16 * 1024 ? HttpParse::Bad : HttpParse::Incomplete;
	const QList<QByteArray> lines = buf.left(headEnd).split('\n');
	if (lines.isEmpty())
		return HttpParse::Bad;
	const QList<QByteArray> first = lines.front().trimmed().split(' ');
	if (first.size() != 3 || !first[2].startsWith("HTTP/1."))
		return HttpParse::Bad;
	HttpRequest r;
	r.method = first[0].toUpper();
	r.path = first[1];
	if (const int q = r.path.indexOf('?'); q >= 0)
		r.path.truncate(q);
	for (int i = 1; i < lines.size(); ++i) {
		const QByteArray line = lines[i].trimmed();
		if (line.isEmpty())
			continue;
		const int colon = line.indexOf(':');
		if (colon <= 0)
			return HttpParse::Bad;
		r.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
	}
	bool okLen = true;
	const int length = r.headers.contains("content-length") ? r.headers.value("content-length").toInt(&okLen) : 0;
	if (!okLen || length < 0 || length > maxBody)
		return HttpParse::Bad;
	if (buf.size() < headEnd + 4 + length)
		return HttpParse::Incomplete;
	r.body = buf.mid(headEnd + 4, length);
	if (out)
		*out = r;
	return HttpParse::Complete;
}

QString untrustedReason(const HttpRequest &r, quint16 port)
{
	if (r.headers.contains("origin"))
		return QStringLiteral("requests from web pages are not accepted");
	// Host: "127.0.0.1:47811", "localhost:47811", "[::1]:47811", or no port.
	QByteArray host = r.headers.value("host").toLower();
	const QByteArray portSuffix = ':' + QByteArray::number(port);
	if (host.endsWith(portSuffix))
		host.chop(portSuffix.size());
	if (host != "127.0.0.1" && host != "localhost" && host != "[::1]")
		return QStringLiteral("unexpected Host header");
	if (r.method == "POST" && r.headers.value("x-harpia-client").isEmpty())
		return QStringLiteral("commands need an X-Harpia-Client header");
	return {};
}

QByteArray httpResponse(int status, const QJsonObject &json)
{
	const char *text = status == 200   ? "OK"
			   : status == 400 ? "Bad Request"
			   : status == 403 ? "Forbidden"
			   : status == 404 ? "Not Found"
			   : status == 405 ? "Method Not Allowed"
			   : status == 409 ? "Conflict"
			   : status == 503 ? "Service Unavailable"
					   : "Internal Server Error";
	const QByteArray body = QJsonDocument(json).toJson(QJsonDocument::Compact);
	QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + text + "\r\n";
	out += "Content-Type: application/json\r\n";
	out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	out += "Cache-Control: no-store\r\n";
	out += "Connection: close\r\n\r\n";
	out += body;
	return out;
}

QJsonObject remoteError(const QString &message)
{
	return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

bool rectFromJson(const QJsonObject &o, QRect *out)
{
	for (const char *k : {"x", "y", "width", "height"})
		if (!o.value(QLatin1String(k)).isDouble())
			return false;
	const auto v = [&](const char *k) { return int(std::lround(o.value(QLatin1String(k)).toDouble())); };
	const QRect r(v("x"), v("y"), v("width"), v("height"));
	if (r.width() <= 0 || r.height() <= 0)
		return false;
	if (out)
		*out = r;
	return true;
}

RemoteReply RemoteReply::ok(const QJsonObject &extra)
{
	RemoteReply r;
	r.body = extra;
	r.body.insert(QStringLiteral("ok"), true);
	return r;
}

RemoteReply RemoteReply::fail(int status, const QString &message)
{
	RemoteReply r;
	r.status = status;
	r.body = remoteError(message);
	return r;
}

RemoteReply dispatchRemote(const HttpRequest &r, RemoteHandler &h)
{
	const auto body = [&]() -> QJsonObject {
		if (r.body.trimmed().isEmpty())
			return {};
		return QJsonDocument::fromJson(r.body).object();
	};
	const bool bodyOk = r.body.trimmed().isEmpty() || QJsonDocument::fromJson(r.body).isObject();

	if (r.path == "/status") {
		if (r.method != "GET")
			return RemoteReply::fail(405, QStringLiteral("use GET"));
		return RemoteReply::ok(h.remoteStatus());
	}
	static const QByteArray commands[] = {"/record/start", "/record/pause", "/record/resume", "/record/stop",
					      "/area/show"};
	if (std::find(std::begin(commands), std::end(commands), r.path) == std::end(commands))
		return RemoteReply::fail(404, QStringLiteral("no such command"));
	if (r.method != "POST")
		return RemoteReply::fail(405, QStringLiteral("use POST"));
	if (!bodyOk)
		return RemoteReply::fail(400, QStringLiteral("the body must be a JSON object"));
	const QJsonObject b = body();

	if (r.path == "/record/start") {
		RemoteStartRequest req;
		if (!rectFromJson(b, &req.areaPx))
			return RemoteReply::fail(400, QStringLiteral("x, y, width and height (pixels) are required"));
		req.client = b.value(QStringLiteral("client")).toString().left(120);
		req.label = b.value(QStringLiteral("label")).toString().left(120);
		return h.remoteStart(req);
	}
	if (r.path == "/record/pause")
		return h.remotePause();
	if (r.path == "/record/resume")
		return h.remoteResume();
	if (r.path == "/record/stop") {
		const int minMs = std::max(0, b.value(QStringLiteral("discardIfShorterThanMs")).toInt(0));
		return h.remoteStop(minMs);
	}
	QRect area;
	if (!rectFromJson(b, &area))
		return RemoteReply::fail(400, QStringLiteral("x, y, width and height (pixels) are required"));
	return h.remoteShowArea(area);
}

// ---- Server -------------------------------------------------------------------

RemoteControlServer::RemoteControlServer(RemoteHandler &handler, QObject *parent)
	: QObject(parent), handler_(handler), server_(new QTcpServer(this))
{
	connect(server_, &QTcpServer::newConnection, this, &RemoteControlServer::onNewConnection);
}

RemoteControlServer::~RemoteControlServer() = default;

bool RemoteControlServer::listen(quint16 port)
{
	close();
	return server_->listen(QHostAddress::LocalHost, port);
}

void RemoteControlServer::close()
{
	if (server_->isListening())
		server_->close();
}

bool RemoteControlServer::isListening() const
{
	return server_->isListening();
}

quint16 RemoteControlServer::port() const
{
	return server_->serverPort();
}

QString RemoteControlServer::errorString() const
{
	return server_->errorString();
}

void RemoteControlServer::onNewConnection()
{
	while (QTcpSocket *s = server_->nextPendingConnection()) {
		buffers_.insert(s, {});
		connect(s, &QTcpSocket::readyRead, this, [this, s]() { onReadyRead(s); });
		connect(s, &QTcpSocket::disconnected, this, [this, s]() {
			buffers_.remove(s);
			s->deleteLater();
		});
		// A client that connects and says nothing is dropped. Stopped once a
		// whole request is in: the answer may wait on a dialog.
		auto *idle = new QTimer(s);
		idle->setObjectName(QStringLiteral("idle"));
		idle->setSingleShot(true);
		connect(idle, &QTimer::timeout, s, &QTcpSocket::abort);
		idle->start(5000);
	}
}

void RemoteControlServer::onReadyRead(QTcpSocket *s)
{
	if (!buffers_.contains(s))
		return;
	QByteArray &buf = buffers_[s];
	buf += s->readAll();
	HttpRequest req;
	const HttpParse state = parseHttpRequest(buf, &req);
	if (state == HttpParse::Incomplete)
		return;
	buffers_.remove(s); // one request per connection
	if (auto *idle = s->findChild<QTimer *>(QStringLiteral("idle")))
		idle->stop();
	if (state == HttpParse::Bad) {
		respond(s, RemoteReply::fail(400, QStringLiteral("malformed request")));
		return;
	}
	if (const QString why = untrustedReason(req, server_->serverPort()); !why.isEmpty()) {
		respond(s, RemoteReply::fail(403, why));
		return;
	}
	// Run the command from the event loop rather than inside this socket's
	// signal, and answer once it is done.
	QPointer<QTcpSocket> guard(s);
	QTimer::singleShot(0, this, [this, guard, req]() {
		if (!guard)
			return;
		if (busy_) {
			respond(guard, RemoteReply::fail(503, QStringLiteral("Harpia is busy (is a dialog open?)")));
			return;
		}
		busy_ = true;
		const RemoteReply reply = dispatchRemote(req, handler_);
		busy_ = false;
		if (guard)
			respond(guard, reply);
	});
}

void RemoteControlServer::respond(QTcpSocket *s, const RemoteReply &reply)
{
	s->write(httpResponse(reply.status, reply.body));
	s->disconnectFromHost();
}

} // namespace harpia
