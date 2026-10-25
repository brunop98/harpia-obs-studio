#pragma once

#include <QApplication>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace harpia {

// Launch Services can deliver documents before the main window exists, or
// while a modal editor is loading. Keep them until opening is safe.
class ProjectOpenQueue : public QObject {
public:
	explicit ProjectOpenQueue(QObject *parent = nullptr) : QObject(parent)
	{
		qApp->installEventFilter(this);
		timer_.setSingleShot(true);
		connect(&timer_, &QTimer::timeout, this, [this]() { drain(); });
	}
	void setReceiver(QObject *receiver, std::function<void(const QString &)> open)
	{
		receiver_ = receiver;
		open_ = std::move(open);
		timer_.start(0);
	}
	bool enqueue(const QString &path)
	{
		if (path.isEmpty() || !path.endsWith(QStringLiteral(".harpiaproj"), Qt::CaseInsensitive))
			return false;
		const QString absolute = QFileInfo(path).absoluteFilePath();
		if (!pending_.contains(absolute))
			pending_.append(absolute);
		if (receiver_)
			timer_.start(0);
		return true;
	}

protected:
	bool eventFilter(QObject *, QEvent *event) override
	{
		if (event->type() != QEvent::FileOpen)
			return false;
		auto *file = static_cast<QFileOpenEvent *>(event);
		if (!enqueue(file->file().isEmpty() && file->url().isLocalFile()
			     ? file->url().toLocalFile() : file->file()))
			return false;
		file->accept();
		return true;
	}

private:
	void drain()
	{
		if (!receiver_ || !open_ || pending_.isEmpty())
			return;
		if (opening_ || QApplication::activeModalWidget()) {
			timer_.start(100);
			return;
		}
		opening_ = true;
		const QString path = pending_.takeFirst();
		open_(path);
		opening_ = false;
		if (!pending_.isEmpty())
			timer_.start(0);
	}
	QPointer<QObject> receiver_;
	std::function<void(const QString &)> open_;
	QStringList pending_;
	QTimer timer_;
	bool opening_ = false;
};

} // namespace harpia
