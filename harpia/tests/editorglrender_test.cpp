// Render a real shader on a worker while the GUI synchronously joins it. This
// catches a GUI-thread surface request that would otherwise deadlock on export.
#include "editor/shader/EditorGlSurface.hpp"
#include "editor/shader/ShaderRenderer.hpp"

#include <QApplication>
#include <QImage>
#include <QThread>

#include <cstdio>
#include <future>
#include <thread>

using namespace harpia;
static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
static bool colored(const QImage &image)
{
	const QColor color = image.pixelColor(image.width() / 2, image.height() / 2);
	return image.size() == QSize(64, 48) && color.red() <= 1 && color.green() >= 254 &&
	       color.blue() <= 1 && color.alpha() >= 254;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	bool recoveryReady = false;
#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_GUI_GL_SURFACES)
	std::promise<bool> firstAttempt;
	std::promise<void> prepared;
	auto mayRetry = prepared.get_future();
	std::thread recover([&]() {
		ShaderRenderer renderer;
		firstAttempt.set_value(renderer.ensureGl());
		mayRetry.wait();
		renderer.release();
		recoveryReady = renderer.ensureGl();
		renderer.release();
	});
	check(!firstAttempt.get_future().get(), "an early worker reports unavailable prepared surfaces");
	check(prepareEditorGlSurfaces(4), "GUI surfaces are prepared before the export worker retries");
	prepared.set_value();
	recover.join();
#else
	check(prepareEditorGlSurfaces(4), "GUI surfaces are prepared before the export worker starts");
#endif
	{
		ShaderRenderer gui;
		if (!gui.ensureGl()) {
			std::printf("SKIP: OpenGL is unavailable: %s\n", qPrintable(gui.lastError()));
			gui.release();
			releaseEditorGlSurfaces();
			return failures ? 1 : 0;
		}
#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_GUI_GL_SURFACES)
		check(recoveryReady, "explicit release permits retry after a temporary surface shortage");
#endif
		const ShaderLayerSource shader{QStringLiteral(
			"#version 330 core\nout vec4 color;\nvoid main(){color=vec4(0,1,0,1);}\n"), {}};
		QString guiError;
		check(gui.setChain({shader}, &guiError), "the GUI compiles the supported GLSL shader");
		QImage input(64, 48, QImage::Format_RGBA8888);
		input.fill(Qt::red);
		check(colored(gui.apply(input, 0, 0, {{}})), "the GUI shader renders the expected pixels");
		bool workerReady = false, compiled = false;
		QImage workerOutput;
		QString error;
		std::thread worker([&]() {
			ShaderRenderer renderer;
			workerReady = renderer.ensureGl();
			if (workerReady) {
				compiled = renderer.setChain({shader}, &error);
				workerOutput = renderer.apply(input, 0, 0, {{}});
			} else {
				error = renderer.lastError();
			}
			renderer.release();
		});
		worker.join(); // must finish without the GUI pumping a queued invocation
		if (!error.isEmpty())
			std::printf("worker detail: %s\n", qPrintable(error));
		check(workerReady && compiled, "a joined worker obtains its own context and compiles the shader");
		check(!workerOutput.isNull() && colored(workerOutput), "worker export pixels match the GUI shader");
		check(!releaseEditorGlSurfaces(), "GUI cleanup cannot invalidate a live GUI renderer");
		check(colored(gui.apply(input, 0, 1, {{}})), "the retained renderer still works after partial cleanup");
		gui.release();
		check(releaseEditorGlSurfaces(), "all surfaces return after both rendering contexts are released");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
