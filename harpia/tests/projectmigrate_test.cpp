// Two project-level settings that became things on the timeline.
//
// Inverse Selection was one spec sitting next to the tracks, and the shader
// chain was a list applied to the finished picture. Both are now clips: a
// spotlight is an effect clip carrying its own areas, and a shader is a
// component on a clip. Opening an old project has to turn each into the thing
// that replaced it, and the ways that can go wrong lose the user's work
// silently:
//
//   - the spotlight dropped, so a tutorial highlight is simply gone;
//   - migrated to a zero-length clip, which cannot be grabbed to fix;
//   - the shader chain dropped, or its parameter values reset to defaults;
//   - the chain migrated in the wrong order, so a grade lands before a blur
//     that used to run first;
//   - either one migrated a SECOND time on the next open, doubling it.
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ShaderComponent.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPushButton>
#include <QStandardPaths>
#include <QThread>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eq(double got, double want, const char *w, double tol = 1e-6)
{
	const bool g = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %g, want %g)\n", g ? "PASS" : "FAIL", w, got, want);
	if (!g)
		++failures;
}
static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// Every effect clip on every track, with the components it carries.
static QVector<const TlClip *> effectClips(const TimelineModel &m)
{
	QVector<const TlClip *> out;
	for (const TlTrack &t : m.tracks)
		for (const TlClip &c : t.clips)
			if (c.type == TlClip::Type::Effect)
				out.append(&c);
	return out;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// The bundled shaders are seeded from Qt resources, which this test binary
	// does not link -- so without this the shader half of the migration would
	// quietly not be exercised at all, and the run would still say ALL PASSED.
	const QString shadersDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
				   QStringLiteral("/harpia/shaders");
	QDir().mkpath(shadersDir);
	{
		QFile sf(shadersDir + QStringLiteral("/migratetest.frag"));
		sf.open(QIODevice::WriteOnly);
		sf.write("//@param amount float 0 1 0.3 Amount\n"
			 "void mainImage(out vec4 o, in vec2 p){ o = vec4(amount); }\n");
	}

	VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
	w.resize(1200, 800);
	w.show();
	QApplication::processEvents();
	if (QPushButton *full = button(&w, QStringLiteral("Full Editing"))) {
		full->click();
		settle(400);
	}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "the editor came up in Full editing");
	if (!tv)
		return 1;

	// Which shaders exist depends on what the folder was seeded with, so pick
	// one that actually registered rather than assuming a name.
	QString shader;
	for (const ComponentType &t : ComponentRegistry::instance().all())
		if (t.category == QStringLiteral("Shader")) {
			shader = t.id.mid(QStringLiteral("harpia.shader.").size());
			break;
		}
	std::printf("     using the shader %s\n",
		    shader.isEmpty() ? "(none registered)" : qPrintable(shader));

	const QString path = QDir::temp().filePath(QStringLiteral("harpia-migrate-test.harpiaproj"));
	QFile::remove(path);

	// A v3 project in the OLD shape: a project-level spotlight, a project-level
	// shader chain, and one ordinary clip so the timeline has a length.
	QString json = QStringLiteral(R"({
	  "harpiaProject": 3,
	  "tracks": [ { "kind": 0, "name": "V1", "clips": [
	      { "type":"text", "srcStart":0, "srcEnd":4000, "outStart":0,
		"textStyle": {"text":"hi"} } ] } ],
	  "spotlight": { "enabled": true, "dim": 0.42, "blur": 0.1,
	    "masks": [ { "name":"Spotlight", "shape":1, "enabled":true,
			 "pose": {"cx":0.25,"cy":0.75,"w":0.3,"h":0.2} } ] }%1
	})");
	if (!shader.isEmpty())
		json = json.arg(QStringLiteral(",\n\t  \"effects\": [ {\"name\":\"%1\", "
					       "\"params\":{}} ]")
					.arg(shader));
	else
		json = json.arg(QString());

	QFile f(path);
	f.open(QIODevice::WriteOnly);
	f.write(json.toUtf8());
	f.close();

	w.openProjectAt(path);
	settle(900);

	const TimelineModel m = tv->model();
	const QVector<const TlClip *> fx = effectClips(m);
	std::printf("     effect clips after opening: %d\n", int(fx.size()));

	std::printf("\n-- the project-level spotlight became a clip --\n");
	{
		const TlClip *spot = nullptr;
		for (const TlClip *c : fx)
			if (c->fx.type == FxType::InverseSelection)
				spot = c;
		ok(spot != nullptr, "there is an Inverse Selection clip");
		if (spot) {
			ok(spot->fx.spot.enabled, "still switched on");
			eq(spot->fx.spot.dimOpacity, 0.42, "with the dim it was saved with");
			ok(spot->fx.spot.masks.size() == 1, "and its one area");
			if (!spot->fx.spot.masks.isEmpty())
				eq(spot->fx.spot.masks[0].pose.cx, 0.25, "where it was put");
			// "Whole project" means a clip you can actually see and grab.
			ok(spot->outDurationMs() > 0, "spanning a real stretch of the timeline");
			ok(spot->outStartMs == 0, "from the beginning");
		}
	}

	if (!shader.isEmpty()) {
		std::printf("\n-- the project shader chain became components --\n");
		const TlClip *sh = nullptr;
		for (const TlClip *c : fx)
			if (!c->components.isEmpty())
				sh = c;
		ok(sh != nullptr, "there is a clip carrying the shader");
		if (sh) {
			ok(sh->components.size() == 1, "one component, for the one shader");
			ok(sh->components[0].typeId == shaderComponentId(shader),
			   "of the shader's own type");
			ok(sh->outDurationMs() > 0, "over a real stretch, not a zero-width clip");
		}
	}

	std::printf("\n-- saving and reopening does not migrate again --\n");
	{
		const int before = effectClips(tv->model()).size();
		w.saveProjectTo(path, /*quiet=*/true);
		settle(600);
		w.openProjectAt(path);
		settle(900);
		const int after = effectClips(tv->model()).size();
		std::printf("     effect clips before %d, after %d\n", before, after);
		ok(after == before, "the same clips, not a second copy of each");
	}

	QFile::remove(path);
	QFile::remove(shadersDir + QStringLiteral("/migratetest.frag"));
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
