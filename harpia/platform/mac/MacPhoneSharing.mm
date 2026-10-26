#include "platform/PhoneSharingNative.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <utility>

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

// This file is built with ARC. The QObject session retains the native service
// and its weak delegate until a terminal callback or receiver destruction.
// There is no self-retain cycle and no timeout that could interrupt a large
// transfer while the native sharing window is still working.
@class HarpiaAirDropDelegate;

namespace {

QString text(NSString *value)
{
	return value ? QString::fromUtf8(value.UTF8String) : QString();
}

bool guiThread()
{
	auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
	return application && QThread::currentThread() == application->thread() && [NSThread isMainThread];
}

bool fail(QString *error, const QString &message)
{
	if (error)
		*error = message;
	return false;
}

class AirDropSession final : public QObject {
public:
	AirDropSession(QObject *receiver, NSSharingService *service,
		       std::function<void(const QString &)> completion);
	~AirDropSession() override;
	void finish(const QString &error);

private:
	QPointer<QObject> receiver_;
	std::function<void(const QString &)> completion_;
	__strong NSSharingService *service_ = nil;
	__strong HarpiaAirDropDelegate *delegate_ = nil;
	bool finished_ = false;
};

// AppKit does not promise that sharingServiceNamed returns a fresh instance.
// Keep another window from replacing the current service's weak delegate.
// Accessed only on the GUI/main thread; this does not own the session.
QPointer<AirDropSession> activeSession;

} // namespace

@interface HarpiaAirDropDelegate : NSObject <NSSharingServiceDelegate> {
@public
	QPointer<AirDropSession> session;
	__weak NSWindow *sourceWindow;
}
@end

namespace {

AirDropSession::AirDropSession(QObject *receiver, NSSharingService *service,
			     std::function<void(const QString &)> completion)
	: QObject(receiver), receiver_(receiver), completion_(std::move(completion)), service_(service)
{
	delegate_ = [[HarpiaAirDropDelegate alloc] init];
	delegate_->session = this;
	delegate_->sourceWindow = NSApp.keyWindow ?: NSApp.mainWindow;
	service_.delegate = delegate_;
}

AirDropSession::~AirDropSession()
{
	delegate_->session.clear();
	service_.delegate = nil;
	if (activeSession == this)
		activeSession.clear();
}

void AirDropSession::finish(const QString &error)
{
	if (finished_)
		return;
	finished_ = true;
	delegate_->session.clear();
	service_.delegate = nil;
	service_ = nil;
	if (activeSession == this)
		activeSession.clear();
	const QPointer<QObject> receiver = receiver_;
	auto completion = std::move(completion_);
	const QString message = error;
	if (receiver && completion) {
		QMetaObject::invokeMethod(receiver, [receiver, completion = std::move(completion), message]() {
			if (receiver)
				completion(message);
		}, Qt::QueuedConnection);
	}
	deleteLater();
}

// AppKit normally calls delegates on the main thread. Dispatch unconditionally
// so the QObject and its QPointer are never accessed from a native worker, and
// a synchronous native failure cannot delete a session during performWithItems.
void complete(HarpiaAirDropDelegate *delegate, const QString &error)
{
	const QString message = error;
	dispatch_async(dispatch_get_main_queue(), ^{
		if (delegate->session)
			delegate->session->finish(message);
	});
}

} // namespace

@implementation HarpiaAirDropDelegate

- (void)sharingService:(NSSharingService *)service didShareItems:(NSArray *)items
{
	(void)service;
	(void)items;
	complete(self, QString());
}

- (void)sharingService:(NSSharingService *)service didFailToShareItems:(NSArray *)items error:(NSError *)error
{
	(void)service;
	(void)items;
	QString message;
	if ([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSUserCancelledError)
		message = QStringLiteral("AirDrop was canceled.");
	else
		message = text(error.localizedDescription);
	if (message.isEmpty())
		message = QStringLiteral("AirDrop could not send these files.");
	complete(self, message);
}

- (NSWindow *)sharingService:(NSSharingService *)service sourceWindowForShareItems:(NSArray *)items
	sharingContentScope:(NSSharingContentScope *)scope
{
	(void)service;
	(void)items;
	if (scope)
		*scope = NSSharingContentScopeItem;
	return sourceWindow ?: NSApp.mainWindow;
}

@end

namespace harpia::phone_sharing::native {

bool airDropSupported()
{
	if (!guiThread() || !NSApp)
		return false;
	@autoreleasepool {
		@try {
			NSSharingService *service = [NSSharingService sharingServiceNamed:NSSharingServiceNameSendViaAirDrop];
			return service && [service canPerformWithItems:nil];
		} @catch (NSException *exception) {
			(void)exception;
			return false;
		}
	}
}

bool sendWithAirDrop(const QStringList &absoluteFiles, QObject *receiver,
		     std::function<void(const QString &)> completion, QString *error)
{
	if (error)
		error->clear();
	if (!guiThread() || !receiver || receiver->thread() != QThread::currentThread() || !NSApp)
		return fail(error, QStringLiteral("AirDrop must be opened from Harpia's main window thread."));
	if (activeSession)
		return fail(error, QStringLiteral("An AirDrop transfer is already open. Finish or cancel it "
					 "in the AirDrop window before sending more files."));
	if (absoluteFiles.isEmpty())
		return fail(error, QStringLiteral("Choose at least one exported file to send with AirDrop."));
	@autoreleasepool {
		NSMutableArray<NSURL *> *items = [NSMutableArray arrayWithCapacity:NSUInteger(absoluteFiles.size())];
		for (const QString &path : absoluteFiles) {
			const QFileInfo file(path);
			if (path.contains(QChar::Null) || !file.isAbsolute() || !file.exists() ||
			    !file.isFile() || !file.isReadable())
				return fail(error, QStringLiteral("This file is missing or cannot be read:\n%1").arg(path));
			const QString absolute = file.absoluteFilePath();
			NSString *nativePath = [[NSString alloc] initWithCharacters:
				reinterpret_cast<const unichar *>(absolute.utf16()) length:NSUInteger(absolute.size())];
			if (!nativePath)
				return fail(error, QStringLiteral("AirDrop could not read the filename:\n%1").arg(path));
			[items addObject:[NSURL fileURLWithPath:nativePath isDirectory:NO]];
		}

		QPointer<AirDropSession> session;
		@try {
			NSSharingService *service = [NSSharingService sharingServiceNamed:NSSharingServiceNameSendViaAirDrop];
			if (!service || ![service canPerformWithItems:items])
				return fail(error, QStringLiteral("AirDrop is not available for these files on this Mac. "
							 "You can use LocalSend instead."));
			session = new AirDropSession(receiver, service, std::move(completion));
			activeSession = session;
			[service performWithItems:items];
			return true;
		} @catch (NSException *exception) {
			delete session.data();
			QString detail = text(exception.reason);
			if (detail.isEmpty())
				detail = QStringLiteral("The system sharing service could not open.");
			return fail(error, QStringLiteral("Could not open AirDrop: %1").arg(detail));
		}
	}
}

} // namespace harpia::phone_sharing::native
