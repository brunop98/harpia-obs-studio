// Qt's macOS offscreen surfaces are GUI objects even when their GL contexts
// render on workers. Exercise the Mac ownership path with the Linux Qt plugin;
// no actual OpenGL context or Mac SDK is required for this lifetime contract.
#include "editor/shader/EditorGlSurface.hpp"

#include <QApplication>
#include <QOffscreenSurface>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <thread>
#include <vector>

using namespace harpia;
static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	bool queued = false;
	QTimer::singleShot(0, &app, [&]() { queued = true; });
	QOffscreenSurface *unprepared = nullptr;
	std::thread before([&]() { unprepared = createEditorGlSurface(); });
	before.join(); // deliberately do not dispatch GUI events while joining
	check(!unprepared && !queued, "an unprepared worker fails without waiting for the GUI");
	QString error;
	bool wrongThread = true;
	std::thread wrong([&]() { wrongThread = prepareEditorGlSurfaces(4, &error); });
	wrong.join();
	check(!wrongThread && !error.isEmpty(), "native surface preparation is rejected on a worker");
	check(prepareEditorGlSurfaces(4), "the GUI prepares reusable offscreen surfaces");
	std::vector<QOffscreenSurface *> leased;
	std::thread acquire([&]() {
		for (int i = 0; i < 4; ++i)
			leased.push_back(createEditorGlSurface());
	});
	acquire.join();
	bool guiOwned = leased.size() == 4;
	for (auto *surface : leased)
		guiOwned = guiOwned && surface && surface->isValid() && surface->thread() == app.thread();
	check(guiOwned, "worker renderers borrow valid surfaces owned by the GUI thread");
	bool distinct = true;
	for (size_t i = 0; i < leased.size(); ++i)
		for (size_t j = 0; j < i; ++j)
			distinct = distinct && leased[i] != leased[j];
	check(distinct, "concurrent renderers do not lease the same native window");
	QOffscreenSurface *exhausted = reinterpret_cast<QOffscreenSurface *>(1);
	std::thread full([&]() { exhausted = createEditorGlSurface(); });
	full.join();
	check(!exhausted && !queued, "pool exhaustion fails gracefully without a blocking GUI invocation");
	QPointer<QOffscreenSurface> retained = leased.front();
	check(!releaseEditorGlSurfaces() && retained,
	      "cleanup retains a surface still owned by a rendering context");
	std::thread giveBack([&]() {
		for (auto *surface : leased)
			destroyEditorGlSurface(surface);
	});
	giveBack.join();
	check(retained, "worker release returns the lease without deleting a native GUI object");
	bool destroyedOnGui = false;
	QObject::connect(retained, &QObject::destroyed, &app, [&]() {
		destroyedOnGui = QThread::currentThread() == app.thread();
	}, Qt::DirectConnection);
	check(releaseEditorGlSurfaces() && retained.isNull() && destroyedOnGui,
	      "after worker joins, the GUI destroys the returned native surfaces");
	check(prepareEditorGlSurfaces(2), "surface storage can be prepared again after cleanup");
	QOffscreenSurface *reuse = nullptr;
	std::thread useAgain([&]() { reuse = createEditorGlSurface(); destroyEditorGlSurface(reuse); });
	useAgain.join();
	check(reuse && releaseEditorGlSurfaces(), "a later renderer can lease and release fresh storage");
	check(!queued, "surface ownership operations never pump unrelated GUI callbacks");
	QApplication::processEvents();
	check(queued, "the caller remains in control of normal event dispatch");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
