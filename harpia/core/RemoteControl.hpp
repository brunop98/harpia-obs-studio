#pragma once

// Local remote control: lets a tool on this computer -- the Unity Play Mode
// Recorder package (unity/com.harpia.playmode-recorder) -- start, pause,
// resume and stop recordings.
//
// A tiny HTTP/1.1 + JSON server on 127.0.0.1 only. HTTP because every tool can
// speak it (Unity's HttpClient, curl for a quick test) and because each
// command is one request and one answer.
//
// Who may talk to it. Listening on the loopback interface keeps other
// computers out; what is left is a web page in a browser on THIS computer,
// which can send requests to 127.0.0.1 too. So a request is refused when:
//   * it carries an Origin header (browsers send one on every cross-site
//     request; Unity and curl do not),
//   * its Host is not 127.0.0.1 / localhost (a page that re-points its own
//     domain at 127.0.0.1 -- "DNS rebinding" -- still sends its own name), or
//   * a command (POST) lacks the X-Harpia-Client header (a custom header that a
//     page cannot add without a preflight, which this server never approves).
//
// Commands
//   GET  /status              -> {"ok":true,"state":"idle|starting|recording|paused|stopping|countdown",...}
//   POST /record/start        {"x":..,"y":..,"width":..,"height":.., "client":"..", "label":".."}
//                             x/y/width/height: the area in physical pixels of the whole desktop
//   POST /record/pause
//   POST /record/resume
//   POST /record/stop         {"discardIfShorterThanMs": 3000}
//   POST /area/show           {"x":..,"y":..,"width":..,"height":..}   outline it on screen for a moment
// Answers are {"ok":true,...} or {"ok":false,"error":"..."} with a matching
// HTTP status (400 bad request, 403 refused, 404/405, 409 conflict, 503 busy).

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>

#include "core/RemoteSettings.hpp"

class QTcpServer;
class QTcpSocket;

namespace harpia {

// ---- The protocol, pure (tested without a socket) --------------------------

struct HttpRequest {
	QByteArray method;                     // "GET", "POST", ...
	QByteArray path;                       // without the query string
	QHash<QByteArray, QByteArray> headers; // names in lower case
	QByteArray body;
};

enum class HttpParse { Incomplete, Complete, Bad };

// Parses one request from the start of `buf`. Bodies over `maxBody` bytes and
// malformed heads are Bad.
HttpParse parseHttpRequest(const QByteArray &buf, HttpRequest *out, int maxBody = 64 * 1024);

// Why a request is refused (empty when it is welcome); see the header comment.
QString untrustedReason(const HttpRequest &r, quint16 port);

// One complete HTTP response carrying `json`.
QByteArray httpResponse(int status, const QJsonObject &json);

// {"ok":false,"error":message}
QJsonObject remoteError(const QString &message);

// The area in a start / show body, in physical desktop pixels.
bool rectFromJson(const QJsonObject &o, QRect *out);

// ---- What the recorder does with the commands ------------------------------

struct RemoteStartRequest {
	QRect areaPx;   // physical pixels of the whole desktop
	QString client; // "Unity 2022.3.10f1", for the log
	QString label;  // e.g. the scene name, for the log
};

struct RemoteReply {
	int status = 200;
	QJsonObject body; // {"ok":...}

	static RemoteReply ok(const QJsonObject &extra = {});
	static RemoteReply fail(int status, const QString &message);
};

class RemoteHandler {
public:
	virtual ~RemoteHandler() = default;
	virtual QJsonObject remoteStatus() = 0;
	virtual RemoteReply remoteStart(const RemoteStartRequest &req) = 0;
	virtual RemoteReply remotePause() = 0;
	virtual RemoteReply remoteResume() = 0;
	virtual RemoteReply remoteStop(int discardShorterThanMs) = 0;
	virtual RemoteReply remoteShowArea(const QRect &areaPx) = 0;
};

// Routes a parsed, trusted request to the handler.
RemoteReply dispatchRemote(const HttpRequest &r, RemoteHandler &h);

// ---- The server --------------------------------------------------------------

class RemoteControlServer : public QObject {
	Q_OBJECT
public:
	explicit RemoteControlServer(RemoteHandler &handler, QObject *parent = nullptr);
	~RemoteControlServer() override;

	// Listen on 127.0.0.1:port (closing any previous socket). False if the port
	// is taken; errorString() says why.
	bool listen(quint16 port);
	void close();
	bool isListening() const;
	quint16 port() const;
	QString errorString() const;

private:
	void onNewConnection();
	void onReadyRead(QTcpSocket *s);
	void respond(QTcpSocket *s, const RemoteReply &reply);

	RemoteHandler &handler_;
	QTcpServer *server_ = nullptr;
	QHash<QTcpSocket *, QByteArray> buffers_;
	// A handler call can open a dialog, which runs its own event loop -- and
	// with it this server. A second command arriving then is answered "busy"
	// rather than run in the middle of the first.
	bool busy_ = false;
};

} // namespace harpia
