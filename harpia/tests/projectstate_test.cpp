// Save -> Open -> Save again gives the same file.
//
// projectwindow_test proves the Full-editing timeline survives. This covers
// everything around it: the sources, the Trim range / speed / crop, the
// Multi-Cut cuts, the voiceover takes AND the voiceover mix, the editing mode
// and which source was active. A project is hand-built with every one of
// those away from its default, opened in a fresh editor, saved again, and the
// two files are compared KEY BY KEY -- so anything written but not read back
// (or read but not written) shows up by name, whatever feature it belongs to.
#include "editor/VideoEditorWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
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

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}

static QJsonObject readJson(const QString &p)
{
	QFile f(p);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}

static void writeJson(const QString &p, const QJsonObject &o)
{
	QFile f(p);
	if (f.open(QIODevice::WriteOnly))
		f.write(QJsonDocument(o).toJson());
}

// Keys whose values differ, recursively into objects, with a path.
static void diff(const QJsonValue &a, const QJsonValue &b, const QString &path, QStringList &out)
{
	if (a.isObject() && b.isObject()) {
		const QJsonObject oa = a.toObject(), ob = b.toObject();
		QStringList keys = oa.keys();
		for (const QString &k : ob.keys())
			if (!keys.contains(k))
				keys << k;
		for (const QString &k : keys)
			diff(oa.value(k), ob.value(k), path + QLatin1Char('.') + k, out);
		return;
	}
	if (a.isArray() && b.isArray()) {
		const QJsonArray aa = a.toArray(), ab = b.toArray();
		if (aa.size() != ab.size()) {
			out << QStringLiteral("%1 (%2 vs %3 items)").arg(path).arg(aa.size()).arg(ab.size());
			return;
		}
		for (int i = 0; i < aa.size(); ++i)
			diff(aa[i], ab[i], QStringLiteral("%1[%2]").arg(path).arg(i), out);
		return;
	}
	if (a != b)
		out << path;
}

// Open gives sources new ids (the editor's own), and rewrites every reference
// to match. What must survive is WHICH source each thing points at, so ids are
// replaced by their position in the sources list before comparing.
static void canonicalIds(QJsonObject &root)
{
	QHash<int, int> pos;
	QJsonArray src = root.value("sources").toArray();
	for (int i = 0; i < src.size(); ++i) {
		QJsonObject o = src[i].toObject();
		pos.insert(o.value("id").toInt(), i);
		o["id"] = i;
		src[i] = o;
	}
	root["sources"] = src;
	const auto map = [&pos](const QJsonValue &v) { return QJsonValue(pos.value(v.toInt(), -1)); };
	QJsonArray segs = root.value("segments").toArray();
	for (int i = 0; i < segs.size(); ++i) {
		QJsonObject o = segs[i].toObject();
		o["source"] = map(o.value("source"));
		segs[i] = o;
	}
	root["segments"] = segs;
	if (root.contains("activeSource"))
		root["activeSource"] = map(root.value("activeSource"));
	QJsonArray tracks = root.value("tracks").toArray();
	for (int t = 0; t < tracks.size(); ++t) {
		QJsonObject to = tracks[t].toObject();
		QJsonArray clips = to.value("clips").toArray();
		for (int c = 0; c < clips.size(); ++c) {
			QJsonObject co = clips[c].toObject();
			if (co.contains("source"))
				co["source"] = map(co.value("source"));
			clips[c] = co;
		}
		to["clips"] = clips;
		tracks[t] = to;
	}
	root["tracks"] = tracks;
}

static QPushButton *button(QWidget &w, const QString &text)
{
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// The voiceover mix controls, found the way a user finds them.
static QSlider *origVolume(QWidget &w)
{
	for (QSlider *s : w.findChildren<QSlider *>())
		if (s->toolTip().startsWith(QStringLiteral("Volume of the video's own audio")))
			return s;
	return nullptr;
}
static QCheckBox *duck(QWidget &w)
{
	for (QCheckBox *c : w.findChildren<QCheckBox *>())
		if (c->text().startsWith(QStringLiteral("Duck original")))
			return c;
	return nullptr;
}

// Build the rich project from a plain save of `video`: two sources, a trim,
// a speed, a crop, cuts across both sources, a voiceover take and its mix.
static QJsonObject richProject(const QJsonObject &base, const QString &second, const QString &wav,
			       const QString &assetsRel, const QString &mode, int activeSource)
{
	QJsonObject root = base;
	QJsonArray sources = root.value("sources").toArray();
	const int firstId = sources.isEmpty() ? 1 : sources[0].toObject().value("id").toInt();
	QJsonObject s2;
	s2["id"] = 77;
	s2["path"] = QDir::toNativeSeparators(second);
	s2["name"] = QStringLiteral("av_blue.mp4");
	s2["durationMs"] = 4000.0;
	s2["width"] = 320;
	s2["height"] = 180;
	sources.append(s2);
	root["sources"] = sources;
	root["trimStart"] = 500.0;
	root["trimEnd"] = 3000.0;
	root["speed"] = 1.5;
	QJsonObject crop;
	crop["enabled"] = true;
	crop["x"] = 20;
	crop["y"] = 10;
	crop["w"] = 200;
	crop["h"] = 120;
	root["crop"] = crop;
	QJsonArray segs;
	for (int k = 0; k < 2; ++k) {
		QJsonObject o;
		o["srcStart"] = double(k * 1000);
		o["srcEnd"] = double(k * 1000 + 800);
		o["speed"] = k ? 2.0 : 1.0;
		o["source"] = k ? 77 : firstId;
		segs.append(o);
	}
	root["segments"] = segs;
	QJsonArray vo;
	QJsonObject v;
	v["file"] = assetsRel + QStringLiteral("/vo_000.wav");
	v["outStart"] = 250.0;
	v["duration"] = 1500.0;
	v["srcStart"] = 100.0;
	v["srcTotal"] = 2000.0;
	v["volume"] = 0.8;
	v["fadeIn"] = 120;
	v["fadeOut"] = 240;
	vo.append(v);
	root["voiceovers"] = vo;
	QJsonObject mix;
	mix["originalVolume"] = 30;
	mix["duck"] = true;
	root["voiceoverMix"] = mix;
	root["editMode"] = mode;
	root["activeSource"] = activeSource == 2 ? 77 : firstId;
	Q_UNUSED(wav);
	return root;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString red = media + "/av_red.mp4", blue = media + "/av_blue.mp4", wav = media + "/tone.wav";
	for (const QString &f : {red, blue, wav})
		if (!QFile::exists(f)) {
			std::printf("missing %s\n", qPrintable(f));
			return 2;
		}
	QTemporaryDir dir;

	// A plain save to start from: the real writer's shape, ids and keys.
	const QString plain = dir.filePath("plain.harpiaproj");
	{
		VideoEditorWindow w(red);
		w.resize(1200, 800);
		w.show();
		settle(300);
		ok(w.saveProjectTo(plain, true).isEmpty(), "a plain project saved");
	}

	for (const QString mode : {QStringLiteral("multicut"), QStringLiteral("trim")}) {
		std::printf("\n-- %s project, second source active --\n", qPrintable(mode));
		const QString name = QStringLiteral("rich_%1").arg(mode);
		const QString first = dir.filePath(name + ".harpiaproj");
		const QString assetsRel = name + QStringLiteral("_assets");
		QDir().mkpath(dir.filePath(assetsRel));
		QFile::copy(wav, dir.filePath(assetsRel + "/vo_000.wav"));
		writeJson(first, richProject(readJson(plain), blue, wav, assetsRel, mode, 2));

		VideoEditorWindow w{QString()};
		w.resize(1200, 800);
		w.show();
		settle(300);
		ok(w.openProjectAt(first), "opened");
		settle(900);

		// What the editor shows, not only what it writes.
		QPushButton *modeBtn = button(w, mode == "trim" ? QStringLiteral("Simple Trim") : QStringLiteral("Multi-Cut"));
		ok(modeBtn && modeBtn->isChecked(), mode == "trim" ? "reopened in Trim" : "reopened in Multi-Cut");
		ok(QFileInfo(w.sourceFileForTest(w.activeSourceIdForTest())).fileName() == "av_blue.mp4",
		   "the second source is the active one");
		QSlider *vol = origVolume(w);
		QCheckBox *dk = duck(w);
		ok(vol && vol->value() == 30, "the original-audio volume came back (30%)");
		ok(dk && dk->isChecked(), "and ducking under narration");

		// Save again: every key must match the file it was opened from.
		const QString second = dir.filePath(name + "_again.harpiaproj");
		ok(w.saveProjectTo(second, true).isEmpty(), "saved again");
		QJsonObject a = readJson(first), b = readJson(second);
		for (QJsonObject *o : {&a, &b}) {
			o->remove("saved"); // when, not what
			// The take is copied into the NEW project's assets folder, so its
			// relative path names that folder; the rest of the take must match.
			QJsonArray vo = o->value("voiceovers").toArray();
			for (int i = 0; i < vo.size(); ++i) {
				QJsonObject v = vo[i].toObject();
				v.remove("file");
				vo[i] = v;
			}
			(*o)["voiceovers"] = vo;
			canonicalIds(*o);
		}
		QStringList changed;
		diff(a, b, QStringLiteral("project"), changed);
		for (const QString &c : changed)
			std::printf("     differs: %s\n", qPrintable(c));
		ok(changed.isEmpty(), "save -> open -> save: every key identical");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
