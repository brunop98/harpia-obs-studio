// Exercise the real Mac authorization coordinator with fake native endpoints.
// No TCC reads, screen streams, OS prompts or privacy setting changes.
#include "platform/mac/MacPlatform.hpp"
#include "platform/mac/MacAudioPermissions.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <cstdio>
#include <functional>
#include <thread>
#include <utility>

namespace mp = harpia::mac_platform;
using ProbeReply = std::function<void(bool, QString)>;
static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}
static void drain()
{
	for (int i = 0; i < 5; ++i)
		QCoreApplication::processEvents(QEventLoop::AllEvents);
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	QObject receiver;
	ProbeReply nativeReply;
	int probes = 0;
	mp::setScreenCaptureTestBackend([] { return false; }, [&](ProbeReply reply) {
		++probes;
		nativeReply = std::move(reply);
	});
	check(mp::screenCaptureStatus() == mp::PermissionStatus::Denied && probes == 0,
	      "read-only startup checks never prompt or create capture streams");
	bool delivered = false;
	bool granted = false;
	bool onGuiThread = false;
	mp::requestScreenCapture(&receiver, [&](bool result) {
		delivered = true;
		granted = result;
		onGuiThread = QThread::currentThread() == application.thread();
	});
	drain();
	check(probes == 1 && !delivered, "Record waits for the asynchronous native result");
	std::thread completion([&] { nativeReply(true, {}); });
	completion.join();
	drain();
	check(delivered && granted && onGuiThread, "native success is delivered on the GUI thread");
	check(mp::screenCaptureStatus() == mp::PermissionStatus::Granted && harpia::mac_audio::permissionGranted(),
	      "SCK success authorizes recorder and audio gates despite negative Quartz preflight");
	mp::invalidateScreenCaptureAuthorization();
	check(mp::screenCaptureStatus() == mp::PermissionStatus::Denied && !harpia::mac_audio::permissionGranted(),
	      "a new attempt or failed stream discards prior SCK authorization");
	delivered = false;
	mp::requestScreenCapture(&receiver, [&](bool result) { delivered = true; granted = result; });
	drain();
	nativeReply(false, QStringLiteral("SCStreamErrorDomain code=-3801: user denied"));
	drain();
	check(delivered && !granted && mp::screenCaptureStatus() == mp::PermissionStatus::Denied,
	      "a real native denial never authorizes recording");
	check(mp::screenCaptureDiagnostics().contains(QStringLiteral("code=-3801: user denied")),
	      "permission diagnostics retain the actual native denial");
	delivered = false;
	mp::requestScreenCapture(&receiver, [&](bool result) { delivered = true; granted = result; });
	drain();
	mp::invalidateScreenCaptureAuthorization();
	nativeReply(true, {});
	drain();
	check(delivered && !granted && mp::screenCaptureStatus() == mp::PermissionStatus::Denied,
	      "late success from an invalidated attempt cannot restore authorization");
	const int previousProbes = probes;
	auto *deletedReceiver = new QObject;
	mp::requestScreenCapture(deletedReceiver, [&](bool) { ++failures; });
	delete deletedReceiver;
	drain();
	check(probes == previousProbes, "a deleted receiver cannot initiate a native request");
	deletedReceiver = new QObject;
	mp::requestScreenCapture(deletedReceiver, [&](bool) { ++failures; });
	drain();
	delete deletedReceiver;
	nativeReply(true, {});
	drain();
	check(probes == previousProbes + 1, "a late native completion is safe after its receiver closes");
	mp::setScreenCaptureTestBackend([] { return true; }, [&](ProbeReply) { ++probes; });
	check(mp::screenCaptureStatus() == mp::PermissionStatus::Granted && probes == previousProbes + 1,
	      "an already positive Quartz preflight stays read-only");
	return failures ? 1 : 0;
}
