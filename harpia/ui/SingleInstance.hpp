// One Harpia at a time, and launching a second one brings the first forward.
//
// A recorder genuinely cannot run twice: two processes would fight over the
// capture devices, the audio graph, the output folder and the preset file, and
// the second obs_startup() is the kind of thing that fails in a way nobody can
// read. So the check has to happen before ANY of that starts.
//
// A lock alone is not enough for what people expect here. Double-clicking the
// icon when the app is already running should show you the app, not an error --
// so this is a lock that can also carry a message, which means a real socket
// rather than a shared-memory flag.
#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace harpia {

class SingleInstance : public QObject {
	Q_OBJECT
public:
	// `key` names the lock. Callers should pass something app-specific;
	// userScopedKey() decorates it so two people logged into the same machine
	// each get their own instance rather than blocking each other.
	explicit SingleInstance(const QString &key, QObject *parent = nullptr);
	~SingleInstance() override;

	// true  -> we are the first instance, and now hold the lock.
	// false -> another instance is already running; it has been asked to show
	//          itself, and this process should exit without starting anything.
	bool acquire();

	// The key with the current user folded in. Two accounts on one machine are
	// two separate users of the app, not a conflict.
	static QString userScopedKey(const QString &base);

	// Are we actually listening? acquire() returning true is not the same
	// thing: if the lock cannot be taken at all it still lets the app start,
	// because a second window is a smaller problem than an app that will not
	// open. A test checking the lock has to tell those two cases apart.
	bool isHoldingLock() const;

signals:
	// A second copy was launched and asked us to come to the front.
	void anotherInstanceStarted();

private:
	void onConnection();

	QString key_;
	QLocalServer *server_ = nullptr;
};

// Bring a window to the front from a background process.
//
// raise() and activateWindow() are not enough on Windows: the OS only lets the
// process that currently owns the foreground hand it over. The second instance
// therefore grants permission before it exits (see acquire()), and this then
// takes it. Without both halves the taskbar button flashes and nothing rises.
void raiseWindowToFront(QWidget *w);

} // namespace harpia
