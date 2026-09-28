// Save Project and Open Project, through the real editor window.
//
// projectfull_test proves the file format keeps every field. This proves the
// WINDOW does: the sources it lists, the ids it remaps, the files it relinks,
// the tags, rules, markers and lanes it puts back -- by saving a rich project
// from one editor and opening it in a brand-new, empty one, then comparing
// every clip. Source ids are compared by the FILE they point at, since a fresh
// editor numbers its media pool its own way.
//
// Needs real media: the runner (run_avsync.sh) makes a video, a still and a WAV.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
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

static void toFullEditing(VideoEditorWindow &w)
{
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Full Editing")) {
			b->click();
			settle(300);
			return;
		}
}

// The keys whose values differ between two JSON objects, for a readable failure.
static QStringList diffKeys(const QJsonObject &a, const QJsonObject &b)
{
	QStringList out;
	QSet<QString> keys;
	for (const QString &k : a.keys())
		keys.insert(k);
	for (const QString &k : b.keys())
		keys.insert(k);
	for (const QString &k : keys)
		if (a.value(k) != b.value(k))
			out << k;
	return out;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	if (argc < 2) {
		std::printf("usage: projectwindow_test <media dir with av_red.mp4, still.png, tone.wav>\n");
		return 2;
	}
	const QString media = QString::fromLocal8Bit(argv[1]);
	const QString video = media + QStringLiteral("/av_red.mp4");
	const QString still = media + QStringLiteral("/still.png");
	const QString wav = media + QStringLiteral("/tone.wav");
	const QString proj = media + QStringLiteral("/roundtrip.harpiaproj");
	QFile::remove(proj);

	// ---- Editor one: build a project on real media ----------------------
	VideoEditorWindow w1(video);
	w1.resize(1200, 800);
	w1.show();
	settle(300);
	toFullEditing(w1);
	TimelineView *tv1 = w1.findChild<TimelineView *>();
	ok(tv1 != nullptr, "editor one is in Full editing");
	if (!tv1)
		return 1;
	// The still and the sound come in the way a user brings them: dropped on the timeline.
	emit tv1->filesDropped({still}, 0, -1, 0);
	emit tv1->filesDropped({wav}, -1, int(tv1->model().tracks.size()), 0); // a new lane at the bottom
	settle(800);

	int vid = -1, img = -1, snd = -1;
	for (const TlTrack &t : tv1->model().tracks)
		for (const TlClip &c : t.clips) {
			const QString p = QFileInfo(w1.sourceFileForTest(c.sourceId)).fileName();
			if (p == QStringLiteral("av_red.mp4"))
				vid = c.sourceId;
			else if (p == QStringLiteral("still.png"))
				img = c.sourceId;
			else if (p == QStringLiteral("tone.wav"))
				snd = c.sourceId;
		}
	// And by id, for any that did not land as a clip (a sound dropped on a
	// picture lane goes to the media pool).
	for (int id = 0; id < 50; ++id) {
		const QString f = QFileInfo(w1.sourceFileForTest(id)).fileName();
		if (vid < 0 && f == QStringLiteral("av_red.mp4"))
			vid = id;
		if (img < 0 && f == QStringLiteral("still.png"))
			img = id;
		if (snd < 0 && f == QStringLiteral("tone.wav"))
			snd = id;
	}
	std::printf("     sources: video %d, still %d, sound %d\n", vid, img, snd);
	ok(vid >= 0 && img >= 0 && snd >= 0, "the video, the still and the sound are in the media pool");

	TimelineModel m;
	m.markers = {400, 1800};
	m.tags = {TlTag{3, QStringLiteral("Intro"), QColor(10, 20, 30)}, TlTag{9, QStringLiteral("Punch ★"), QColor(200, 90, 40)}};
	TlSoundRule rule;
	rule.id = 5;
	rule.trigger = TlSoundRule::Trigger::Tagged;
	rule.tagId = 9;
	rule.sourceId = snd;
	rule.soundName = QStringLiteral("tone.wav");
	rule.volume = 0.8;
	rule.offsetMs = 40;
	m.soundRules = {rule};

	TlClip v;
	v.type = TlClip::Type::Video;
	v.sourceId = vid;
	v.srcStartMs = 200;
	v.srcEndMs = 3000;
	v.speed = 1.25;
	v.posX = 0.4;
	v.scale = 0.8;
	v.rotation = 12;
	v.opacity = 0.9;
	v.crop = QRect(10, 10, 200, 120);
	{
		TlKeyframe a, b;
		a.tMs = 0;
		b.tMs = 900;
		b.tf.posX = 0.7;
		b.tf.scale = 1.3;
		b.pos.ease = TlEase::Bezier;
		b.pos.bez1 = 0.2;
		v.keys = {a, b};
		ComponentInstance blur;
		blur.typeId = QStringLiteral("harpia.blur");
		blur.instanceId = QStringLiteral("b");
		blur.inMs = 300;
		blur.props.insert(QStringLiteral("radius"), 0.25);
		blur.keys.insert(QStringLiteral("radius"), {PropKey{0, 0.1, TlEase::EaseOut}, PropKey{800, 0.5}});
		v.components = {blur};
	}
	v.transition.type = TransitionType::Push;
	v.transition.softness = 0.3;
	v.tags = {3};
	v.volume = 0.6;
	v.fadeInMs = 200;
	v.fadeOutCurve = FadeCurve::Exponential;

	TlClip pic;
	pic.type = TlClip::Type::Image;
	pic.sourceId = img;
	pic.outStartMs = 3000;
	pic.srcEndMs = 2000;
	pic.scale = 0.5;
	pic.posY = 0.3;
	pic.tags = {9};

	TlClip cap;
	cap.type = TlClip::Type::Text;
	cap.outStartMs = 500;
	cap.srcEndMs = 1500;
	cap.posY = 0.88;
	cap.text.text = QStringLiteral("Olá, legendas");
	cap.text.bold = true;
	cap.text.textCase = 2;
	cap.text.boxEnabled = true;
	cap.text.color = QColor(255, 220, 0);
	cap.words = {ClipWordTime{QStringLiteral("Olá,"), 0, 400}, ClipWordTime{QStringLiteral("legendas"), 420, 1100}};

	TlClip fx; // as the app makes one: the fx block is a carrier for older readers
	fx.type = TlClip::Type::Effect;
	fx.outStartMs = 0;
	fx.srcEndMs = 2500;
	fx.fx.params = fxDefaults(fx.fx.type);
	{
		ComponentInstance hue;
		hue.typeId = QStringLiteral("harpia.fx.hueShift");
		hue.instanceId = QStringLiteral("h");
		hue.props.insert(QStringLiteral("degrees"), 90.0);
		fx.components = {hue};
	}

	TlClip music;
	music.type = TlClip::Type::Video;
	music.sourceId = snd;
	music.outStartMs = 250;
	music.srcEndMs = 1500;
	music.volume = 0.4;
	music.fadeOutMs = 300;

	TlTrack subs;
	subs.name = QStringLiteral("Subtitles");
	subs.clips = {cap};
	TlTrack grade;
	grade.kind = TlTrack::Kind::Effect;
	grade.name = QStringLiteral("Grade");
	grade.clips = {fx};
	TlTrack v1;
	v1.name = QStringLiteral("Main");
	v1.color = QColor(0x22, 0x66, 0xaa);
	v1.clips = {v, pic};
	TlTrack a1;
	a1.kind = TlTrack::Kind::Audio;
	a1.name = QStringLiteral("Music");
	a1.gain = 0.7;
	a1.clips = {music};
	TlTrack spare;
	spare.name = QStringLiteral("Spare lane");
	m.tracks = {subs, grade, v1, a1, spare};

	tv1->setModel(m);
	emit tv1->clipsChanged(); // as an edit would: the rules' Sounds lane is (re)built
	settle(400);
	const TimelineModel saved = tv1->model();

	// Quiet: the same file, without the "Project saved." box a test cannot click.
	const QString err = w1.saveProjectTo(proj, /*quiet=*/true);
	settle(200);
	ok(err.isEmpty() && QFile::exists(proj), "Save Project wrote the file");

	// ---- Editor two: a brand-new, empty editor opens it -------------------
	VideoEditorWindow w2{QString()};
	w2.resize(1200, 800);
	w2.show();
	settle(300);
	ok(w2.openProjectAt(proj), "Open Project read it in a fresh editor");
	settle(1200);
	TimelineView *tv2 = w2.findChild<TimelineView *>();
	ok(tv2 != nullptr, "and it opened in Full editing");
	if (!tv2)
		return 1;
	const TimelineModel back = tv2->model();

	std::printf("\n-- what came back --\n");
	ok(back.tracks.size() == saved.tracks.size(), "every lane, the empty one and the Sounds lane too");
	ok(back.markers == saved.markers, "the markers");
	ok(back.tags == saved.tags, "the tag list, names and colours");
	bool rulesOk = back.soundRules.size() == saved.soundRules.size();
	for (int i = 0; rulesOk && i < back.soundRules.size(); ++i) {
		TlSoundRule a = saved.soundRules[i], b = back.soundRules[i];
		const bool sameFile = QFileInfo(w1.sourceFileForTest(a.sourceId)).fileName() ==
				      QFileInfo(w2.sourceFileForTest(b.sourceId)).fileName();
		b.sourceId = a.sourceId;
		rulesOk = sameFile && a == b;
	}
	ok(rulesOk, "the sound rules, pointing at the same sound file");

	int clipsCompared = 0;
	QStringList problems;
	for (int ti = 0; ti < std::min(saved.tracks.size(), back.tracks.size()); ++ti) {
		const TlTrack &x = saved.tracks[ti], &y = back.tracks[ti];
		TlTrack xs = x, ys = y;
		xs.clips.clear();
		ys.clips.clear();
		if (!(xs == ys))
			problems << QStringLiteral("track %1 (%2): %3")
					    .arg(ti)
					    .arg(x.name, diffKeys(trackToJson(xs), trackToJson(ys)).join(QStringLiteral(", ")));
		if (x.clips.size() != y.clips.size()) {
			problems << QStringLiteral("track %1: %2 clips became %3").arg(ti).arg(x.clips.size()).arg(y.clips.size());
			continue;
		}
		for (int ci = 0; ci < x.clips.size(); ++ci) {
			TlClip a = x.clips[ci], b = y.clips[ci];
			++clipsCompared;
			const bool reads = a.type == TlClip::Type::Video || a.type == TlClip::Type::Image;
			if (reads) {
				const QString fa = QFileInfo(w1.sourceFileForTest(a.sourceId)).fileName();
				const QString fb = QFileInfo(w2.sourceFileForTest(b.sourceId)).fileName();
				if (fa != fb)
					problems << QStringLiteral("track %1 clip %2: source %3 became %4").arg(ti).arg(ci).arg(fa, fb);
			}
			// Captions and effect clips carry a source id they never read.
			b.sourceId = a.sourceId;
			a.peaks.clear();
			b.peaks.clear();
			if (!(a == b))
				problems << QStringLiteral("track %1 clip %2: %3")
						    .arg(ti)
						    .arg(ci)
						    .arg(diffKeys(clipToJson(a), clipToJson(b)).join(QStringLiteral(", ")));
		}
	}
	std::printf("     %d clips compared\n", clipsCompared);
	for (const QString &p : problems)
		std::printf("     DIFFERENT: %s\n", qUtf8Printable(p));
	ok(problems.isEmpty(), "every lane and every clip is exactly as it was saved");

	std::printf("\n-- and again --\n");
	{
		const QString proj2 = media + QStringLiteral("/roundtrip2.harpiaproj");
		w2.saveProjectTo(proj2, /*quiet=*/true);
		QFile f1(proj), f2(proj2);
		f1.open(QIODevice::ReadOnly);
		f2.open(QIODevice::ReadOnly);
		QJsonObject j1 = QJsonDocument::fromJson(f1.readAll()).object();
		QJsonObject j2 = QJsonDocument::fromJson(f2.readAll()).object();
		for (const char *k : {"saved", "created", "sourceName"}) {
			j1.remove(QLatin1String(k));
			j2.remove(QLatin1String(k));
		}
		// Source ids are the editor's own numbering: compare by file, then drop them.
		const auto files = [](const QJsonObject &j) {
			QStringList out;
			for (const QJsonValue &s : j.value(QStringLiteral("sources")).toArray())
				out << QFileInfo(s.toObject().value(QStringLiteral("path")).toString()).fileName();
			out.sort();
			return out;
		};
		ok(files(j1) == files(j2), "the reopened project lists the same media files");
		std::printf("     top-level keys that differ: %s\n",
			    qUtf8Printable(diffKeys(j1, j2).join(QStringLiteral(", "))));
		QStringList d = diffKeys(j1, j2);
		d.removeAll(QStringLiteral("sources"));
		d.removeAll(QStringLiteral("tracks"));     // source ids renumbered (compared above)
		d.removeAll(QStringLiteral("soundRules")); // likewise
		ok(d.isEmpty(), "and saving it again writes the same project");
		QFile::remove(proj2);
	}

	std::printf("\n-- the rest of a project: format, trim, Multi-Cut, voiceovers --\n");
	{
		QFile f1(proj.isEmpty() ? QString() : proj);
		// Start from the file editor one saved, and add everything the
		// timeline does not cover.
		w1.saveProjectTo(proj, /*quiet=*/true);
		f1.open(QIODevice::ReadOnly);
		QJsonObject j = QJsonDocument::fromJson(f1.readAll()).object();
		f1.close();
		int vidPid = -1;
		for (const QJsonValue &sv : j.value(QStringLiteral("sources")).toArray())
			if (QFileInfo(sv.toObject().value(QStringLiteral("path")).toString()).fileName() ==
			    QStringLiteral("av_red.mp4"))
				vidPid = sv.toObject().value(QStringLiteral("id")).toInt();
		j[QStringLiteral("canvasW")] = 1080;
		j[QStringLiteral("canvasH")] = 1920;
		j[QStringLiteral("fps")] = 25.0;
		j[QStringLiteral("author")] = QStringLiteral("Bruno");
		j[QStringLiteral("trimStart")] = 300.0;
		j[QStringLiteral("trimEnd")] = 3200.0;
		j[QStringLiteral("speed")] = 1.5;
		j[QStringLiteral("crop")] = QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("x"), 10},
						       {QStringLiteral("y"), 20}, {QStringLiteral("w"), 200},
						       {QStringLiteral("h"), 100}};
		j[QStringLiteral("segments")] = QJsonArray{
			QJsonObject{{QStringLiteral("srcStart"), 100.0}, {QStringLiteral("srcEnd"), 900.0},
				    {QStringLiteral("speed"), 2.0}, {QStringLiteral("source"), vidPid}},
			QJsonObject{{QStringLiteral("srcStart"), 1500.0}, {QStringLiteral("srcEnd"), 2500.0},
				    {QStringLiteral("speed"), 0.5}, {QStringLiteral("source"), vidPid}}};
		QFile::copy(wav, media + QStringLiteral("/vo_take.wav"));
		j[QStringLiteral("voiceovers")] = QJsonArray{QJsonObject{
			{QStringLiteral("file"), QStringLiteral("vo_take.wav")}, {QStringLiteral("outStart"), 700.0},
			{QStringLiteral("duration"), 1200.0}, {QStringLiteral("srcStart"), 250.0},
			{QStringLiteral("srcTotal"), 2000.0}, {QStringLiteral("volume"), 1.4},
			{QStringLiteral("fadeIn"), 60}, {QStringLiteral("fadeOut"), 90}}};
		const QString rich = media + QStringLiteral("/rich.harpiaproj");
		{
			QFile o(rich);
			o.open(QIODevice::WriteOnly);
			o.write(QJsonDocument(j).toJson());
		}
		VideoEditorWindow w3{QString()};
		w3.resize(1200, 800);
		w3.show();
		settle(300);
		ok(w3.openProjectAt(rich), "a project with all of it opens");
		settle(1000);
		ok(w3.timelineCanvasSize() == QSize(1080, 1920) && std::abs(w3.timelineFps() - 25.0) < 1e-9,
		   "its format (1080x1920, 25 fps) is the editor's format");
		const QString again = media + QStringLiteral("/rich2.harpiaproj");
		w3.saveProjectTo(again, /*quiet=*/true);
		QFile o2(again);
		o2.open(QIODevice::ReadOnly);
		const QJsonObject k = QJsonDocument::fromJson(o2.readAll()).object();
		QStringList differ;
		for (const char *key : {"canvasW", "canvasH", "fps", "author", "trimStart", "trimEnd", "speed", "crop"})
			if (j.value(QLatin1String(key)) != k.value(QLatin1String(key)))
				differ << QString::fromLatin1(key);
		const auto segs = [](const QJsonObject &o) {
			QJsonArray a = o.value(QStringLiteral("segments")).toArray();
			for (int i = 0; i < a.size(); ++i) {
				QJsonObject s = a[i].toObject();
				s.remove(QStringLiteral("source")); // renumbered; checked by file below
				a[i] = s;
			}
			return a;
		};
		if (segs(j) != segs(k))
			differ << QStringLiteral("segments");
		const auto vos = [](const QJsonObject &o) {
			QJsonArray a = o.value(QStringLiteral("voiceovers")).toArray();
			for (int i = 0; i < a.size(); ++i) {
				QJsonObject s = a[i].toObject();
				s.remove(QStringLiteral("file")); // copied into the project's assets folder
				a[i] = s;
			}
			return a;
		};
		if (vos(j) != vos(k))
			differ << QStringLiteral("voiceovers");
		const QJsonArray kv = k.value(QStringLiteral("voiceovers")).toArray();
		const bool voFile = !kv.isEmpty() &&
				    QFile::exists(media + QLatin1Char('/') + kv[0].toObject().value(QStringLiteral("file")).toString());
		if (differ.contains(QStringLiteral("crop")))
			std::printf("     crop saved %s, came back %s\n",
				    QJsonDocument(j.value(QStringLiteral("crop")).toObject()).toJson(QJsonDocument::Compact).constData(),
				    QJsonDocument(k.value(QStringLiteral("crop")).toObject()).toJson(QJsonDocument::Compact).constData());
		std::printf("     settings that changed on the way round: %s\n",
			    differ.isEmpty() ? "none" : qUtf8Printable(differ.join(QStringLiteral(", "))));
		ok(differ.isEmpty(), "format, author, trim, speed, crop, Multi-Cut cuts and voiceovers all come back");
		ok(voFile, "and the voiceover audio travels with the project (copied to its assets folder)");
		int segSrcOk = 0;
		for (const QJsonValue &sv : k.value(QStringLiteral("segments")).toArray()) {
			const int pid = sv.toObject().value(QStringLiteral("source")).toInt();
			for (const QJsonValue &src : k.value(QStringLiteral("sources")).toArray())
				if (src.toObject().value(QStringLiteral("id")).toInt() == pid &&
				    QFileInfo(src.toObject().value(QStringLiteral("path")).toString()).fileName() ==
					    QStringLiteral("av_red.mp4"))
					++segSrcOk;
		}
		ok(segSrcOk == 2, "each cut still points at its video");
		QFile::remove(rich);
		QFile::remove(again);
	}

	QFile::remove(proj);
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
