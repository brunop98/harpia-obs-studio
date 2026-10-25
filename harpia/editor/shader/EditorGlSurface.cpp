#include "EditorGlSurface.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QThread>

#include <algorithm>
#include <vector>

namespace harpia {

QSurfaceFormat editorGlFormat()
{
	QSurfaceFormat format;
	format.setRenderableType(QSurfaceFormat::OpenGL);
	format.setProfile(QSurfaceFormat::CoreProfile);
#ifdef Q_OS_MACOS
	format.setVersion(4, 1);
#else
	format.setVersion(3, 3);
#endif
	return format;
}

#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_GUI_GL_SURFACES)
namespace {
struct Entry {
	QOffscreenSurface *surface;
	bool leased = false;
};
struct Surfaces {
	QMutex mutex;
	std::vector<Entry> entries;
};
Surfaces &surfaces()
{
	// No destructor may walk native windows after QApplication has torn down
	// its screens. The application explicitly releases idle surfaces instead.
	static auto *storage = new Surfaces;
	return *storage;
}
bool onGuiThread()
{
	auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
	return app && QThread::currentThread() == app->thread();
}
} // namespace
#endif

bool prepareEditorGlSurfaces(int minimumCount, QString *error)
{
	if (error)
		error->clear();
#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_GUI_GL_SURFACES)
	if (!onGuiThread()) {
		if (error)
			*error = QStringLiteral("OpenGL surfaces must be prepared on the GUI thread.");
		return false;
	}
	minimumCount = std::clamp(minimumCount, 1, 96);
	auto &storage = surfaces();
	for (;;) {
		{
			QMutexLocker lock(&storage.mutex);
			if (int(storage.entries.size()) >= minimumCount)
				return true;
		}
		// Native create/destroy may emit QObject signals; do not hold the pool
		// mutex while calling into the platform window implementation.
		auto *surface = new QOffscreenSurface;
		surface->setFormat(editorGlFormat());
		surface->create();
		if (!surface->isValid()) {
			delete surface;
			if (error)
				*error = QStringLiteral("Could not prepare an offscreen OpenGL surface.");
			return false;
		}
		QMutexLocker lock(&storage.mutex);
		storage.entries.push_back({surface, false});
	}
#else
	Q_UNUSED(minimumCount);
	Q_UNUSED(error);
	return true;
#endif
}

bool releaseEditorGlSurfaces()
{
#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_GUI_GL_SURFACES)
	if (!onGuiThread())
		return false;
	auto &storage = surfaces();
	std::vector<QOffscreenSurface *> idle;
	bool allReturned = true;
	{
		QMutexLocker lock(&storage.mutex);
		for (auto it = storage.entries.begin(); it != storage.entries.end();) {
			if (it->leased) {
				allReturned = false;
				++it;
			} else {
				idle.push_back(it->surface);
				it = storage.entries.erase(it);
			}
		}
	}
	for (auto *surface : idle)
		delete surface;
	if (!allReturned)
		qWarning("harpia: OpenGL surface cleanup deferred: a renderer still owns a surface");
	return allReturned;
#else
	return true;
#endif
}

QOffscreenSurface *createEditorGlSurface()
{
#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_GUI_GL_SURFACES)
	if (!QCoreApplication::instance())
		return nullptr;
	auto &storage = surfaces();
	// GUI callers can grow the pool. Worker callers only lease what has already
	// been prepared; no queued/blocking GUI call can deadlock a worker join.
	if (onGuiThread()) {
		int count;
		bool free = false;
		{
			QMutexLocker lock(&storage.mutex);
			count = int(storage.entries.size());
			for (const auto &entry : storage.entries)
				free = free || !entry.leased;
		}
		if (!free && !prepareEditorGlSurfaces(count ? count + 4 : 24))
			return nullptr;
	}
	QMutexLocker lock(&storage.mutex);
	for (auto &entry : storage.entries)
		if (!entry.leased) {
			entry.leased = true;
			return entry.surface;
		}
	qWarning("harpia: no prepared OpenGL surface is available; using the CPU effect path");
	return nullptr;
#else
	auto *surface = new QOffscreenSurface;
	surface->setFormat(editorGlFormat());
	surface->create();
	if (!surface->isValid()) {
		delete surface;
		return nullptr;
	}
	return surface;
#endif
}

void destroyEditorGlSurface(QOffscreenSurface *surface)
{
	if (!surface)
		return;
#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_GUI_GL_SURFACES)
	auto &storage = surfaces();
	QMutexLocker lock(&storage.mutex);
	for (auto &entry : storage.entries)
		if (entry.surface == surface) {
			entry.leased = false;
			return;
		}
	// An unknown pointer must not be destroyed from a rendering worker.
	qWarning("harpia: ignored an unknown offscreen OpenGL surface");
#else
	if (!QCoreApplication::instance())
		return;
	if (surface->thread() == QThread::currentThread())
		delete surface;
	else
		surface->deleteLater();
#endif
}

} // namespace harpia
