#pragma once

// A web server inside a test: each path answers with a fixed status, type and
// body. Enough HTTP for what a paste does with a link -- one GET, redirects
// followed, the body streamed -- and nothing more. Listens on 127.0.0.1 at a
// free port; url("/x") is the address of /x.
//
// `stall` sends the headers and the first part of the body and then holds the
// connection open, for checking that a cancelled download leaves nothing
// behind.

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

struct TinyRoute {
	int status = 200;
	QByteArray type;
	QByteArray body;
	QByteArray location; // for a redirect
	bool stall = false;
	qint64 claimLength = -1; // Content-Length to announce; the body's own size when -1
};

class TinyHttpServer : public QTcpServer {
public:
	QHash<QByteArray, TinyRoute> routes;

	TinyHttpServer()
	{
		connect(this, &QTcpServer::newConnection, this, [this]() {
			while (QTcpSocket *s = nextPendingConnection())
				serve(s);
		});
		listen(QHostAddress::LocalHost, 0);
	}

	QString url(const char *path) const
	{
		return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(QLatin1String(path));
	}

private:
	void serve(QTcpSocket *s)
	{
		auto buf = std::make_shared<QByteArray>();
		connect(s, &QTcpSocket::readyRead, s, [this, s, buf]() {
			buf->append(s->readAll());
			if (buf->indexOf("\r\n\r\n") < 0)
				return;
			const QByteArray line = buf->left(buf->indexOf("\r\n"));
			const QByteArray path = line.split(' ').value(1);
			const TinyRoute r = routes.value(path, TinyRoute{404, "text/plain", "missing", {}, false, -1});
			QByteArray head = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\n";
			if (!r.type.isEmpty())
				head += "Content-Type: " + r.type + "\r\n";
			if (!r.location.isEmpty())
				head += "Location: " + r.location + "\r\n";
			head += "Content-Length: " +
				QByteArray::number(r.claimLength >= 0 ? r.claimLength : r.body.size()) + "\r\n";
			head += "Connection: close\r\n\r\n";
			s->write(head);
			s->write(r.body);
			if (!r.stall)
				s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};
