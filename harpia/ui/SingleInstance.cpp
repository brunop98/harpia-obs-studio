#include "SingleInstance.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace harpia {
namespace {

// Long enough that a busy first instance still answers, short enough that a
// dead socket file does not hold up a legitimate launch. Only paid on the
// second launch, never on the first.
constexpr int kConnectTimeoutMs = 700;
constexpr int kIoTimeoutMs = 700;

} // namespace

QString SingleInstance::userScopedKey(const QString &base)
{
	// The home path rather than a user name: it is always present, always
	// differs between accounts, and needs no platform-specific lookup. Hashed
	// because a socket name has a length limit on Unix and a path would blow it.
	const QByteArray h =
		QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha1)
			.toHex()
			.left(12);
	return base + QLatin1Char('-') + QString::fromLatin1(h);
}

SingleInstance::SingleInstance(const QString &key, QObject *parent) : QObject(parent), key_(key) {}

SingleInstance::~SingleInstance()
{
	if (server_)
		server_->close();
}

bool SingleInstance::isHoldingLock() const
{
	return server_ && server_->isListening();
}

bool SingleInstance::acquire()
{
	// Try to TALK to an existing instance first. This doubles as the staleness
	// check: on Unix a crashed process leaves its socket file behind, and the
	// only reliable way to tell a live server from a dead file is to try.
	{
		QLocalSocket probe;
		probe.connectToServer(key_);
		if (probe.waitForConnected(kConnectTimeoutMs)) {
			// The server sends its process id first. On Windows the
			// foreground can only be handed over, never taken, so this
			// process -- which has it, having just been launched -- grants
			// it before asking the other one to rise.
			qint64 pid = 0;
			if (probe.waitForReadyRead(kIoTimeoutMs))
				pid = QString::fromUtf8(probe.readLine()).trimmed().toLongLong();
#ifdef Q_OS_WIN
			if (pid > 0)
				AllowSetForegroundWindow(DWORD(pid));
#else
			Q_UNUSED(pid);
#endif
			probe.write("raise\n");
			probe.flush();
			probe.waitForBytesWritten(kIoTimeoutMs);
			probe.disconnectFromServer();
			return false;
		}
	}

	// Nobody answered. Anything left at that name is debris from a process that
	// did not shut down cleanly; removing it is safe precisely because the
	// connect above proved there is no listener.
	QLocalServer::removeServer(key_);

	server_ = new QLocalServer(this);
	// Otherwise a socket created by this user is only reachable by this user,
	// which is what we want -- the key is user-scoped for the same reason.
	server_->setSocketOptions(QLocalServer::UserAccessOption);
	connect(server_, &QLocalServer::newConnection, this, &SingleInstance::onConnection);
	if (!server_->listen(key_)) {
		// Could not listen at all: no lock is possible, so allow the launch
		// rather than refuse to start. A second window is a far smaller problem
		// than an app that will not open.
		delete server_;
		server_ = nullptr;
		return true;
	}
	return true;
}

void SingleInstance::onConnection()
{
	QLocalSocket *c = server_->nextPendingConnection();
	if (!c)
		return;
	c->write(QByteArray::number(QCoreApplication::applicationPid()) + "\n");
	c->flush();
	connect(c, &QLocalSocket::readyRead, this, [this, c]() {
		const QByteArray msg = c->readAll();
		if (msg.contains("raise"))
			emit anotherInstanceStarted();
		c->disconnectFromServer();
	});
	connect(c, &QLocalSocket::disconnected, c, &QLocalSocket::deleteLater);
}

void raiseWindowToFront(QWidget *w)
{
	if (!w)
		return;
	// Un-minimise without disturbing maximised: clearing the whole state word
	// would un-maximise a window the user had maximised on purpose.
	w->setWindowState((w->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
	w->show();
	w->raise();
	w->activateWindow();
#ifdef Q_OS_WIN
	SetForegroundWindow(HWND(w->winId()));
#endif
}

} // namespace harpia
