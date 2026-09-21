// The editing console: one line of JavaScript, one undo step, nothing touched
// on an error.
//
// The engine is exercised directly, without a window: a model goes in, a
// snippet runs, the copy that comes back is inspected. That is exactly the
// contract the window relies on -- it swaps the copy in and records one
// snapshot -- so what is checked here is what the console does.
#include "editor/EditConsole.hpp"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void near(double got, double want, const char *w, double tol = 1e-6)
{
	const bool good = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %.4f, want %.4f)\n", good ? "PASS" : "FAIL", w, got, want);
	if (!good)
		++failures;
}

static TlClip clip(int source, qint64 start, qint64 len)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = source;
	c.srcStartMs = 0;
	c.srcEndMs = len;
	c.outStartMs = start;
	return c;
}

// V2 (one clip), V1 (three clips, the middle one out of vector order), A1.
static TimelineModel model()
{
	TimelineModel m;
	TlTrack v2;
	v2.kind = TlTrack::Kind::Video;
	v2.name = QStringLiteral("V2");
	v2.clips.append(clip(9, 500, 2000));
	TlTrack v1;
	v1.kind = TlTrack::Kind::Video;
	v1.name = QStringLiteral("V1");
	v1.clips.append(clip(1, 0, 1000));
	v1.clips.append(clip(3, 2000, 1000)); // vector index 1 is slot 2
	v1.clips.append(clip(2, 1000, 1000)); // vector index 2 is slot 1
	TlTrack a1;
	a1.kind = TlTrack::Kind::Audio;
	a1.name = QStringLiteral("A1");
	a1.clips.append(clip(7, 0, 3000));
	m.tracks = {v2, v1, a1};
	return m;
}

static EditConsole::Input input(const TimelineModel &m, QVector<QPair<int, int>> sel = {{1, 0}})
{
	EditConsole::Input in;
	in.model = m;
	in.selection = sel;
	in.canvas = QSize(1920, 1080);
	in.playheadMs = 0;
	in.sourceName = [](int id) { return QStringLiteral("src%1.mp4").arg(id); };
	return in;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	if (!EditConsole::available()) {
		std::printf("  SKIP no scripting engine in this build\n\nALL PASSED (0 failures)\n");
		return 0;
	}

	std::printf("\n-- the example: clip.position = [500, 300] --\n");
	{
		const EditConsole::Result r = EditConsole::run(QStringLiteral("clip.position = [500, 300]"), input(model()));
		ok(r.ok && r.error.isEmpty(), "the line runs");
		ok(r.changed, "and reports a change");
		const TlClip &c = r.model.tracks[1].clips[0];
		near(c.posX, 500.0 / 1920.0, "x is 500 canvas pixels, stored as a fraction");
		near(c.posY, 300.0 / 1080.0, "y is 300");
		near(c.scale, 1.0, "scale untouched");
		ok(r.model.tracks[1].clips[1] == model().tracks[1].clips[1], "the other clips are untouched");
		std::printf("     output: \"%s\"\n", qPrintable(r.output));
		ok(r.output.contains(QStringLiteral("500")), "the last expression's value is echoed");
	}

	std::printf("\n-- x, y, scale, opacity; reading back --\n");
	{
		EditConsole::Result r = EditConsole::run(QStringLiteral("clip.x = 960; clip.scale = 1.5; clip.opacity = 0.25"),
							 input(model()));
		const TlClip &c = r.model.tracks[1].clips[0];
		near(c.posX, 0.5, "x alone moves only x");
		near(c.posY, 0.5, "y stays centred");
		near(c.scale, 1.5, "scale");
		near(c.opacity, 0.25, "opacity");
		r = EditConsole::run(QStringLiteral("clip.opacity = 7; clip.scale = -1"), input(model()));
		near(r.model.tracks[1].clips[0].opacity, 1.0, "opacity is clamped to 1");
		near(r.model.tracks[1].clips[0].scale, 0.01, "and scale to its floor");

		r = EditConsole::run(QStringLiteral("print(clip.position, clip.start, clip.duration, clip.name, clip.ref)"),
				     input(model()));
		std::printf("     output: \"%s\"\n", qPrintable(r.output));
		ok(r.ok && !r.changed, "reading changes nothing");
		ok(r.output.contains(QStringLiteral("[960,540]")) && r.output.contains(QStringLiteral(" 0 1000 ")) &&
			   r.output.contains(QStringLiteral("src1.mp4")) && r.output.contains(QStringLiteral("V1[0]")),
		   "position in pixels, start and duration in ms, the source name, and the V1[0] reference");
	}

	std::printf("\n-- many clips at once: templates --\n");
	{
		EditConsole::Result r = EditConsole::run(
			QStringLiteral("clips.forEach(c => c.scale = 1.1); print(clips.length, tracks.V1.length)"),
			input(model(), {}));
		ok(r.ok && r.changed, "a loop over every clip runs with nothing selected");
		ok(r.output == QStringLiteral("5 3"), "5 clips in all, 3 on V1");
		bool all = true;
		for (const TlTrack &t : r.model.tracks)
			for (const TlClip &c : t.clips)
				all = all && std::abs(c.scale - 1.1) < 1e-9;
		ok(all, "and every clip is at 1.1");

		// Slot order is by START, not by vector order.
		r = EditConsole::run(QStringLiteral("tracks.V1.map(c => c.name).join(',')"), input(model(), {}));
		ok(r.output == QStringLiteral("src1.mp4,src2.mp4,src3.mp4"), "a lane's clips come in start order");
		r = EditConsole::run(QStringLiteral("tracks.V1[1].x = 100"), input(model(), {}));
		near(r.model.tracks[1].clips[2].posX, 100.0 / 1920.0, "and V1[1] is the clip that starts second");

		// selection: several clips.
		r = EditConsole::run(QStringLiteral("selection.forEach((c, i) => c.y = 100 * (i + 1)); selection.length"),
				     input(model(), {{1, 0}, {1, 2}}));
		ok(r.output == QStringLiteral("2"), "selection holds both selected clips");
		near(r.model.tracks[1].clips[0].posY, 100.0 / 1080.0, "the primary got y = 100");
		near(r.model.tracks[1].clips[2].posY, 200.0 / 1080.0, "the other got y = 200");

		// run("name") through the template loader.
		EditConsole::Input in = input(model());
		in.loadTemplate = [](const QString &n) {
			return n == QStringLiteral("shrink") ? QStringLiteral("clips.forEach(c => c.scale *= 0.5)")
							      : QString();
		};
		r = EditConsole::run(QStringLiteral("run('shrink'); run('shrink')"), in);
		ok(r.ok, "a template runs by name");
		near(r.model.tracks[0].clips[0].scale, 0.25, "twice, compounding");
		r = EditConsole::run(QStringLiteral("run('nope')"), in);
		ok(!r.ok && r.error.contains(QStringLiteral("nope")), "an unknown template is an error naming it");
	}

	std::printf("\n-- an error changes nothing; a runaway stops --\n");
	{
		EditConsole::Result r = EditConsole::run(
			QStringLiteral("clips[0].scale = 3; clips[1].scale = 3; clip.positon.x = 5"), input(model()));
		std::printf("     error: \"%s\"\n", qPrintable(r.error.section('\n', 0, 0)));
		ok(!r.ok && !r.error.isEmpty(), "a typo is reported");
		ok(!r.changed && r.model == model(), "and NOTHING changed, not even the lines before it");

		r = EditConsole::run(QStringLiteral("while (true) {}"), input(model()));
		std::printf("     error: \"%s\"\n", qPrintable(r.error));
		ok(!r.ok && r.error.contains(QStringLiteral("did not finish")), "an endless loop is stopped and named");

		r = EditConsole::run(QStringLiteral("clip.scale = clip.scale"), input(model()));
		ok(r.ok && !r.changed, "setting a value to itself is not a change (so no undo step)");

		r = EditConsole::run(QStringLiteral("1 + 1"), input(model(), {}));
		ok(r.ok && r.output == QStringLiteral("2"), "with nothing selected the console still works");
		r = EditConsole::run(QStringLiteral("clip.x = 1"), input(model(), {}));
		ok(!r.ok, "but `clip` is null then, and says so");
	}

	std::printf("\n-- a locked lane, and the playhead pose --\n");
	{
		TimelineModel m = model();
		m.tracks[1].locked = true;
		EditConsole::Result r = EditConsole::run(QStringLiteral("clips.forEach(c => c.scale = 2)"), input(m, {}));
		ok(r.ok && r.changed, "the loop runs");
		near(r.model.tracks[1].clips[0].scale, 1.0, "but the locked lane's clips are left alone");
		near(r.model.tracks[0].clips[0].scale, 2.0, "while the others change");

		// A keyed clip: the pose at the playhead is what is read and, as with
		// the Inspector, the base transform is what is written.
		TimelineModel km = model();
		TlKeyframe k0, k1;
		k0.tMs = 0;
		k0.tf.scale = 1.0;
		k1.tMs = 1000;
		k1.tf.scale = 3.0;
		km.tracks[1].clips[0].keys = {k0, k1};
		EditConsole::Input in = input(km);
		in.playheadMs = 500;
		r = EditConsole::run(QStringLiteral("clip.scale"), in);
		ok(r.output.startsWith(QStringLiteral("2")), "a keyed clip reads its pose AT the playhead (2.0 halfway)");
	}

	ok(!EditConsole::helpText().isEmpty() &&
		   EditConsole::run(QStringLiteral("help()"), input(model())).output.contains(QStringLiteral("position")),
	   "help() prints the property list");
	ok(EditConsole::refName(model(), 1, 1) == QStringLiteral("V1[2]"), "refName gives the slot, not the vector index");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
