// Save a project, open it again: everything comes back.
//
// A whole timeline is built with EVERY field of every kind of thing set away
// from its default -- clips of each type, their keyframes, words, tags, sound
// rule, transition and fades; components with keyframes, In/Out ramps and
// fields this build does not know; text in every style; effect and spotlight
// clips; tracks with every switch; the project's tags, sound rules and
// markers. It goes through timelineToProject -> a JSON file on disk ->
// timelineFromProject, the same functions Save and Open use, and every field is
// compared BY NAME, so a failure says exactly what was lost rather than just
// "the models differ" (and a field an operator== forgets cannot hide).
//
// Deliberately not compared, because not saving them is correct:
//   peaks     -- waveform cache, rebuilt from the audio on open;
//   autoLane  -- "a lane Harpia made"; after a reload every lane is yours
//                (TimelineModel.hpp says why).
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static int checked = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// One field: counted, and reported by name only when it did not survive.
static QStringList lost;
template <typename T> static void field(const QString &where, const char *name, const T &a, const T &b)
{
	++checked;
	if (!(a == b))
		lost << QStringLiteral("%1.%2").arg(where, QLatin1String(name));
}
#define F(where, a, b, name) field(where, #name, (a).name, (b).name)

static void compareText(const QString &w, const TlText &a, const TlText &b)
{
	F(w, a, b, text); F(w, a, b, fontFamily); F(w, a, b, fontPx); F(w, a, b, bold); F(w, a, b, italic);
	F(w, a, b, color); F(w, a, b, outlineWidth); F(w, a, b, outlineColor); F(w, a, b, boxEnabled);
	F(w, a, b, boxColor); F(w, a, b, boxOpacity); F(w, a, b, boxPadX); F(w, a, b, boxPadY);
	F(w, a, b, boxRadius); F(w, a, b, boxShape); F(w, a, b, align); F(w, a, b, textCase); F(w, a, b, variations); F(w, a, b, random);
}

static void compareClip(const QString &w, const TlClip &a, const TlClip &b)
{
	F(w, a, b, type); F(w, a, b, sourceId); F(w, a, b, srcStartMs); F(w, a, b, srcEndMs); F(w, a, b, speed);
	F(w, a, b, outStartMs); F(w, a, b, posX); F(w, a, b, posY); F(w, a, b, scale); F(w, a, b, rotation);
	F(w, a, b, opacity); F(w, a, b, crop); F(w, a, b, words); F(w, a, b, scripts);
	F(w, a, b, soundRule); F(w, a, b, tags); F(w, a, b, volume); F(w, a, b, fadeInMs); F(w, a, b, fadeOutMs);
	F(w, a, b, fadeInCurve); F(w, a, b, fadeOutCurve);
	field(w, "keys.count", a.keys.size(), b.keys.size());
	for (int i = 0; i < std::min(a.keys.size(), b.keys.size()); ++i) {
		const QString k = QStringLiteral("%1.keys[%2]").arg(w).arg(i);
		F(k, a.keys[i], b.keys[i], tMs);
		F(k, a.keys[i].tf, b.keys[i].tf, posX); F(k, a.keys[i].tf, b.keys[i].tf, posY);
		F(k, a.keys[i].tf, b.keys[i].tf, scale); F(k, a.keys[i].tf, b.keys[i].tf, rotation);
		F(k, a.keys[i].tf, b.keys[i].tf, opacity);
		for (int l = 0; l < kTlLaneCount; ++l) {
			const QString c = QStringLiteral("%1.channel%2").arg(k).arg(l);
			F(c, a.keys[i].channel(l), b.keys[i].channel(l), on);
			F(c, a.keys[i].channel(l), b.keys[i].channel(l), ease);
			F(c, a.keys[i].channel(l), b.keys[i].channel(l), bez1);
			F(c, a.keys[i].channel(l), b.keys[i].channel(l), bez2);
		}
		F(k, a.keys[i], b.keys[i], curvedPath); F(k, a.keys[i], b.keys[i], handlesManual);
		F(k, a.keys[i], b.keys[i], handlesBroken); F(k, a.keys[i], b.keys[i], inX);
		F(k, a.keys[i], b.keys[i], inY); F(k, a.keys[i], b.keys[i], outX); F(k, a.keys[i], b.keys[i], outY);
	}
	if (a.type == TlClip::Type::Text)
		compareText(w + QStringLiteral(".text"), a.text, b.text);
	const QString t = w + QStringLiteral(".transition");
	F(t, a.transition, b.transition, type); F(t, a.transition, b.transition, enabled);
	F(t, a.transition, b.transition, easeOut); F(t, a.transition, b.transition, easeIn);
	F(t, a.transition, b.transition, reverse); F(t, a.transition, b.transition, softness);
	field(w, "components.count", a.components.size(), b.components.size());
	for (int i = 0; i < std::min(a.components.size(), b.components.size()); ++i) {
		const QString c = QStringLiteral("%1.components[%2]").arg(w).arg(i);
		const ComponentInstance &x = a.components[i], &y = b.components[i];
		F(c, x, y, typeId); F(c, x, y, instanceId); F(c, x, y, enabled); F(c, x, y, inMs); F(c, x, y, outMs);
		F(c, x, y, props); F(c, x, y, keys); F(c, x, y, unknown);
	}
	if (a.type == TlClip::Type::Effect) {
		const QString f = w + QStringLiteral(".fx");
		F(f, a.fx, b.fx, type); F(f, a.fx, b.fx, name); F(f, a.fx, b.fx, enabled);
		F(f, a.fx, b.fx, params); F(f, a.fx, b.fx, keys); F(f, a.fx, b.fx, spot);
	}
	++checked;
	if (!(a == b) && lost.isEmpty())
		lost << w + QStringLiteral(" (operator== differs, no single field named)");
}

static TlKeyframe key(qint64 t, double x, double y, double s, double r, double o, TlEase e, double b1,
		      double b2, int lanes)
{
	TlKeyframe k;
	k.tMs = t;
	k.tf = TlTransform{x, y, s, r, o};
	for (int l = 0; l < kTlLaneCount; ++l) {
		k.channel(l).on = (lanes & (1 << l)) != 0;
		k.channel(l).ease = TlEase((int(e) + l) % 4);
		k.channel(l).bez1 = b1 + l * 0.01;
		k.channel(l).bez2 = b2 - l * 0.01;
	}
	return k;
}

static TimelineModel everything()
{
	TimelineModel m;
	m.tags = {TlTag{3, QStringLiteral("Intro"), QColor(10, 20, 30)}, TlTag{9, QStringLiteral("Punchline ★"), QColor(200, 100, 50)}};
	TlSoundRule r;
	r.id = 4;
	r.enabled = false;
	r.trigger = TlSoundRule::Trigger::Tagged;
	r.transitionType = 5;
	r.componentId = QStringLiteral("harpia.textType");
	r.tagId = 9;
	r.lane = 2;
	r.sourceId = 11;
	r.soundName = QStringLiteral("Whoosh.wav");
	r.volume = 1.35;
	r.offsetMs = -120;
	r.maxMs = 900;
	TlSoundRule r2 = r;
	r2.id = 7;
	r2.enabled = true;
	r2.trigger = TlSoundRule::Trigger::Component;
	m.soundRules = {r, r2};
	m.markers = {1500, 4200, 90000};

	// A video clip with everything.
	TlClip v;
	v.type = TlClip::Type::Video;
	v.sourceId = 2;
	v.srcStartMs = 1234;
	v.srcEndMs = 98765;
	v.speed = 1.75;
	v.outStartMs = 4321;
	v.posX = 0.31;
	v.posY = 0.72;
	v.scale = 1.44;
	v.rotation = 33.5;
	v.opacity = 0.66;
	v.crop = QRect(4, 8, 640, 360);
	v.keys = {key(0, 0.1, 0.2, 2.5, 45, 0.5, TlEase::EaseIn, 0.11, 0.91, 0xF),
		  key(750, 0.9, 0.8, 0.5, -30, 1.0, TlEase::Bezier, 0.2, 0.7, 0x5)};
	// A curved path with dragged, broken handles on the first key.
	v.keys[0].curvedPath = true;
	v.keys[0].handlesManual = true;
	v.keys[0].handlesBroken = true;
	v.keys[0].inX = -0.05;
	v.keys[0].inY = 0.125;
	v.keys[0].outX = 0.3;
	v.keys[0].outY = -0.0625;
	v.scripts = {TlScript{QStringLiteral("wobble"), {{QStringLiteral("amt"), 3.5}, {QStringLiteral("hz"), 2.0}}}};
	{
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.blur");
		ci.instanceId = QStringLiteral("b1");
		ci.enabled = false;
		ci.inMs = 250;
		ci.outMs = 400;
		ci.props.insert(QStringLiteral("radius"), 0.37);
		ci.keys.insert(QStringLiteral("radius"), QVector<PropKey>{PropKey{0, 0.1, TlEase::EaseOut, 0.3, 0.7}, PropKey{500, 0.9}});
		v.components.append(ci);
		ComponentInstance slide;
		slide.typeId = QStringLiteral("harpia.slide");
		slide.instanceId = QStringLiteral("s1");
		slide.props.insert(QStringLiteral("edge"), 3.0);
		slide.props.insert(QStringLiteral("curve"), 5.0);
		v.components.append(slide);
		ComponentInstance alien; // a component from a newer build or a plugin
		alien.typeId = QStringLiteral("acme.glitch");
		alien.instanceId = QStringLiteral("g1");
		alien.props.insert(QStringLiteral("amount"), 0.8);
		alien.unknown.insert(QStringLiteral("futureField"), QStringLiteral("keep me"));
		v.components.append(alien);
	}
	v.transition.type = TransitionType::WipeUp;
	v.transition.enabled = false;
	v.transition.easeOut = TlEase::EaseOut;
	v.transition.easeIn = TlEase::EaseIn;
	v.transition.reverse = true;
	v.transition.softness = 0.35;
	v.soundRule = 7;
	v.tags = {3, 9};
	v.volume = 0.55;
	v.fadeInMs = 123;
	v.fadeOutMs = 456;
	v.fadeInCurve = FadeCurve::Exponential;
	v.fadeOutCurve = FadeCurve::Logarithmic;

	// A still.
	TlClip img = v;
	img.type = TlClip::Type::Image;
	img.sourceId = 5;
	img.components.clear();
	img.scripts.clear();
	img.soundRule = kSoundFromComponent;

	// A caption in every style, with word timings (subtitles).
	TlClip txt;
	txt.type = TlClip::Type::Text;
	txt.outStartMs = 20000;
	txt.srcEndMs = 2500;
	txt.posX = 0.5;
	txt.posY = 0.88;
	txt.text.text = QStringLiteral("Era uma vez\numa galinha \"feliz\" – ção");
	txt.text.fontFamily = QStringLiteral("Georgia");
	txt.text.fontPx = 71;
	txt.text.bold = true;
	txt.text.italic = true;
	txt.text.color = QColor(1, 2, 3, 200);
	txt.text.outlineWidth = 4.5;
	txt.text.outlineColor = QColor(4, 5, 6);
	txt.text.boxEnabled = true;
	txt.text.boxColor = QColor(7, 8, 9);
	txt.text.boxOpacity = 0.42;
	txt.text.boxPadX = 21;
	txt.text.boxPadY = 13;
	txt.text.boxRadius = 17;
	txt.text.boxShape = TlTextBoxFitText;
	txt.text.align = 2;
	txt.text.textCase = 2;
	txt.text.random = true;
	txt.text.variations = {QStringLiteral("Era uma vez\num gato"), QStringLiteral("I like \"vultures\" – ção"),
			       QStringLiteral("")};
	txt.words = {ClipWordTime{QStringLiteral("Era"), 0, 300}, ClipWordTime{QStringLiteral("uma"), 310, 520},
		     ClipWordTime{QStringLiteral("vez"), 530, 900}};
	txt.tags = {9};
	{
		ComponentInstance sub;
		sub.typeId = QStringLiteral("harpia.subtitle");
		sub.instanceId = QStringLiteral("sub");
		sub.props.insert(QStringLiteral("mode"), 2.0);
		txt.components.append(sub);
	}

	// An effect clip, and a spotlight.
	TlClip fx;
	fx.type = TlClip::Type::Effect;
	fx.outStartMs = 3000;
	fx.srcEndMs = 6000;
	fx.fx.type = FxType::Vignette;
	fx.fx.name = QStringLiteral("myfx");
	fx.fx.enabled = false;
	fx.fx.params = fxDefaults(FxType::Vignette);
	fx.fx.params[QStringLiteral("amount")] = 0.77;
	fx.fx.keys.append(FxKey{120, {{QStringLiteral("amount"), 0.25}}, TlEase::EaseIn, 0.2, 0.8});
	// In the shape the app makes today: the effect as a component (the fx
	// block is kept alongside for older readers). Opening an old project
	// converts it into exactly this.
	fx = clipFromJson(clipToJson(fx));
	TlClip sp;
	sp.type = TlClip::Type::Effect;
	sp.outStartMs = 9000;
	sp.srcEndMs = 4000;
	sp.fx.type = FxType::InverseSelection;
	sp.fx.params = fxDefaults(FxType::InverseSelection);
	SpotMask mask;
	mask.shape = SpotShape::Ellipse;
	mask.pose = SpotPose{0.3, 0.4, 0.5, 0.6, 20.0, 0.25};
	mask.keys.append(SpotKey{80, SpotPose{0.1, 0.2, 0.3, 0.4, 5.0, 0.1}, TlEase::EaseOut, 0.15, 0.85});
	sp.fx.spot.masks.append(mask);

	// An audio clip on a music lane.
	TlClip a = v;
	a.components.clear();
	a.scripts.clear();
	a.keys.clear();
	a.sourceId = 11;

	TlTrack subs;
	subs.name = QStringLiteral("Subtitles");
	subs.color = QColor(0xd4, 0xa5, 0x3a);
	subs.clips = {txt};
	TlTrack fxLane;
	fxLane.kind = TlTrack::Kind::Effect;
	fxLane.name = QStringLiteral("Grade");
	fxLane.hidden = true;
	fxLane.clips = {fx, sp};
	TlTrack v1;
	v1.name = QStringLiteral("V1 renamed");
	v1.locked = true;
	v1.ripple = true;
	v1.rippleSpeed = true;
	v1.color = QColor(0x12, 0x34, 0x56);
	v1.clips = {v, img};
	TlTrack music;
	music.kind = TlTrack::Kind::Audio;
	music.name = QStringLiteral("Música");
	music.muted = true;
	music.solo = true;
	music.rippleSpeed = true;
	music.gain = 0.65;
	music.clips = {a};
	TlTrack sounds;
	sounds.kind = TlTrack::Kind::Audio;
	sounds.name = QStringLiteral("Sounds");
	sounds.autoSounds = true;
	sounds.locked = true;
	TlTrack emptyLane; // a lane with no clips keeps its name and colour
	emptyLane.name = QStringLiteral("Spare");
	emptyLane.color = QColor(0x99, 0x11, 0x77);
	m.tracks = {subs, fxLane, v1, music, sounds, emptyLane};
	return m;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	const TimelineModel before = everything();

	// Save, to a real file, as Save Project does; open it back.
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("everything.harpia"));
	{
		QJsonObject root;
		root[QStringLiteral("harpiaProject")] = 2;
		timelineToProject(before, root);
		QFile f(path);
		f.open(QIODevice::WriteOnly);
		f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	}
	QFile f(path);
	f.open(QIODevice::ReadOnly);
	const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	const TimelineModel after = timelineFromProject(root);

	std::printf("\n-- the project file --\n");
	ok(root.value(QStringLiteral("harpiaProject")).toInt() == 3, "a timeline project is marked v3");
	ok(after.tracks.size() == before.tracks.size(), "every track comes back, the empty one too");

	std::printf("\n-- every field, by name --\n");
	for (int ti = 0; ti < std::min(before.tracks.size(), after.tracks.size()); ++ti) {
		const TlTrack &x = before.tracks[ti], &y = after.tracks[ti];
		const QString w = QStringLiteral("track[%1]").arg(ti);
		F(w, x, y, kind); F(w, x, y, name); F(w, x, y, muted); F(w, x, y, hidden); F(w, x, y, locked);
		F(w, x, y, ripple); F(w, x, y, rippleSpeed); F(w, x, y, solo); F(w, x, y, gain); F(w, x, y, color); F(w, x, y, autoSounds);
		field(w, "clips.count", x.clips.size(), y.clips.size());
		for (int ci = 0; ci < std::min(x.clips.size(), y.clips.size()); ++ci)
			compareClip(QStringLiteral("%1.clip[%2]").arg(w).arg(ci), x.clips[ci], y.clips[ci]);
	}
	field(QStringLiteral("project"), "markers", before.markers, after.markers);
	field(QStringLiteral("project"), "tags", before.tags, after.tags);
	field(QStringLiteral("project"), "soundRules.count", before.soundRules.size(), after.soundRules.size());
	for (int i = 0; i < std::min(before.soundRules.size(), after.soundRules.size()); ++i) {
		const TlSoundRule &x = before.soundRules[i], &y = after.soundRules[i];
		const QString w = QStringLiteral("soundRule[%1]").arg(i);
		F(w, x, y, id); F(w, x, y, enabled); F(w, x, y, trigger); F(w, x, y, transitionType);
		F(w, x, y, componentId); F(w, x, y, tagId); F(w, x, y, lane); F(w, x, y, sourceId);
		F(w, x, y, soundName); F(w, x, y, volume); F(w, x, y, offsetMs); F(w, x, y, maxMs);
	}
	std::printf("     %d fields compared\n", checked);
	for (const QString &l : lost)
		std::printf("     LOST: %s\n", qUtf8Printable(l));
	ok(lost.isEmpty(), "nothing was lost");
	ok(after == before, "and the models compare equal");

	std::printf("\n-- saving twice changes nothing --\n");
	{
		QJsonObject r1, r2;
		timelineToProject(before, r1);
		timelineToProject(after, r2);
		ok(QJsonDocument(r1).toJson() == QJsonDocument(r2).toJson(),
		   "the reopened project saves byte-for-byte the same file");
	}

	std::printf("\n-- older and odd files --\n");
	{
		// An effect saved before effects were components: it is converted,
		// and an eased key keeps its easing.
		TlClip old;
		old.type = TlClip::Type::Effect;
		old.srcEndMs = 3000;
		old.fx.type = FxType::Brightness;
		old.fx.params = fxDefaults(FxType::Brightness);
		old.fx.keys = {FxKey{0, {{QStringLiteral("amount"), 0.1}}, TlEase::Bezier, 0.12, 0.88},
			       FxKey{900, {{QStringLiteral("amount"), 0.6}}, TlEase::EaseOut, 0.3, 0.7}};
		const TlClip conv = clipFromJson(clipToJson(old));
		const QVector<PropKey> ks = conv.components.isEmpty()
						    ? QVector<PropKey>()
						    : conv.components[0].keys.value(QStringLiteral("amount"));
		ok(conv.components.size() == 1 && conv.components[0].typeId == QStringLiteral("harpia.fx.brightness"),
		   "an old-style effect opens as its component");
		ok(ks.size() == 2 && ks[0].ease == TlEase::Bezier && ks[0].bez1 == 0.12 && ks[0].bez2 == 0.88 &&
			   ks[1].ease == TlEase::EaseOut,
		   "and its keyframes keep their easing and curve handles");
		ok(clipFromJson(clipToJson(conv)) == conv, "and it is not converted a second time");

		QJsonObject markersOnly;
		TimelineModel mk;
		mk.markers = {500, 1000};
		timelineToProject(mk, markersOnly);
		ok(timelineFromProject(markersOnly).markers == mk.markers,
		   "markers on a timeline with no clips yet are kept");
		QJsonObject none;
		timelineToProject(TimelineModel(), none);
		ok(!none.contains(QStringLiteral("tracks")) && timelineFromProject(none) == TimelineModel(),
		   "an empty timeline writes nothing, and reads back empty");
		QJsonObject bad = root;
		QJsonArray tags = bad.value(QStringLiteral("tags")).toArray();
		tags.append(QJsonObject{{QStringLiteral("id"), 0}, {QStringLiteral("name"), QStringLiteral("broken")}});
		tags.append(QJsonObject{{QStringLiteral("id"), 3}, {QStringLiteral("name"), QStringLiteral("duplicate")}});
		bad[QStringLiteral("tags")] = tags;
		ok(timelineFromProject(bad).tags == before.tags, "a tag with no id, or a repeated id, is dropped");
		QJsonObject unsorted = root;
		unsorted[QStringLiteral("markers")] = QJsonArray{9000, 100, 5000};
		ok(timelineFromProject(unsorted).markers == QVector<qint64>({100, 5000, 9000}),
		   "markers come back in time order");
	}

	std::printf("\n-- authored settings before clips are added --\n");
	{
		TimelineModel layout;
		TlTrack picture;
		picture.name = QStringLiteral("B-roll reserved");
		picture.color = QColor(16, 80, 160);
		picture.hidden = true;
		picture.locked = true;
		TlTrack audio;
		audio.kind = TlTrack::Kind::Audio;
		audio.name = QStringLiteral("Narration reserved");
		audio.muted = true;
		audio.solo = true;
		audio.gain = 0.65;
		layout.tracks = {picture, audio};
		QJsonObject saved;
		timelineToProject(layout, saved);
		const TimelineModel opened = timelineFromProject(
			QJsonDocument::fromJson(QJsonDocument(saved).toJson()).object());
		ok(opened == layout && saved.value(QStringLiteral("harpiaProject")).toInt() == 3,
		   "an authored empty lane layout keeps its names, order, switches and audio levels");
		QJsonObject reused = root;
		timelineToProject(layout, reused);
		ok(timelineFromProject(reused) == layout,
		   "rewriting a root removes old markers, tags and sound rules from the new layout");
		timelineToProject(TimelineModel(), reused);
		ok(!reused.contains(QStringLiteral("tracks")) && !reused.contains(QStringLiteral("tags")) &&
		   !reused.contains(QStringLiteral("soundRules")) && timelineFromProject(reused) == TimelineModel(),
		   "clearing the timeline cannot resurrect old tracks or metadata in a reused project root");
	}
	{
		TlClip plain;
		plain.type = TlClip::Type::Text;
		plain.srcEndMs = 2500;
		plain.text.text = QStringLiteral("<b>Hello</b> <color=#ff800080>world ★</color>\n<mark=#ffff0080>again</mark>");
		plain.text.variations = {QStringLiteral("Unused alternative")};
		plain.text.random = false;
		QJsonObject saved = clipToJson(plain);
		const TlClip opened = clipFromJson(
			QJsonDocument::fromJson(QJsonDocument(saved).toJson()).object());
		ok(opened == plain && !opened.text.random,
		   "plain text retains rich tags and alternatives without becoming Random Text on reopen");
		QJsonObject legacyText = saved.value(QStringLiteral("textStyle")).toObject();
		legacyText.remove(QStringLiteral("random"));
		saved[QStringLiteral("textStyle")] = legacyText;
		ok(clipFromJson(saved).text.random,
		   "legacy captions with alternatives and no kind flag still reopen as Random Text");
	}
	{
		ComponentRegistry &registry = ComponentRegistry::instance();
		const QJsonObject unknown = QJsonDocument::fromJson(R"({
			"type":"missing.projectEffect", "id":"effect1", "version":7,
			"enabled":false, "inMs":40, "outMs":80,
			"props":{"amount":0.37,"label":"My look","palette":[1,2,3],
			         "config":{"mode":"soft","enabled":true},"optional":null},
			"keys":{"amount":[{"t":0,"v":0.25,"ease":2,"futureCurve":{"shape":[1,2,3]}},
			                  {"t":100,"v":0.75,"ease":99,"futureHandle":"keep me"}],
			        "palette":[{"t":0,"v":[1,0,0,1],"futureBlend":"linear-light"}],
			        "futureTrack":{"samples":[1,2,3]}},
			"futureField":"keep this"
		})").object();
		const ComponentInstance missing = componentFromJson(unknown, registry);
		const QJsonObject reopened = QJsonDocument::fromJson(QJsonDocument(
			componentToJson(missing, registry)).toJson()).object();
		ok(reopened == unknown,
		   "an unavailable component retains its version, typed properties and future key payload exactly");
		ComponentInstance edited = missing;
		edited.enabled = true;
		edited.inMs = 0;
		edited.outMs = 150;
		edited.props[QStringLiteral("label")] = QStringLiteral("Renamed look");
		edited.keys[QStringLiteral("amount")][0].v = 0.5;
		const QJsonObject edits = componentToJson(edited, registry);
		const QJsonObject editedKeys = edits.value(QStringLiteral("keys")).toObject();
		const QJsonArray editedAmount = editedKeys.value(QStringLiteral("amount")).toArray();
		ok(!edits.contains(QStringLiteral("enabled")) && !edits.contains(QStringLiteral("inMs")) &&
		   edits.value(QStringLiteral("outMs")).toInt() == 150 &&
		   edits.value(QStringLiteral("props")).toObject().value(QStringLiteral("label")).toString() == QStringLiteral("Renamed look") &&
		   editedAmount[0].toObject().value(QStringLiteral("v")).toDouble() == 0.5 &&
		   editedAmount[0].toObject().value(QStringLiteral("futureCurve")) ==
		       unknown.value(QStringLiteral("keys")).toObject().value(QStringLiteral("amount")).toArray()[0].toObject().value(QStringLiteral("futureCurve")) &&
		   editedAmount[1] == unknown.value(QStringLiteral("keys")).toObject().value(QStringLiteral("amount")).toArray()[1] &&
		   editedKeys.value(QStringLiteral("palette")) == unknown.value(QStringLiteral("keys")).toObject().value(QStringLiteral("palette")) &&
		   editedKeys.value(QStringLiteral("futureTrack")) == unknown.value(QStringLiteral("keys")).toObject().value(QStringLiteral("futureTrack")),
		   "common edits apply to an unavailable component while opaque key data stays intact");
		edited.keys.remove(QStringLiteral("amount"));
		ok(!componentToJson(edited, registry).value(QStringLiteral("keys")).toObject().contains(QStringLiteral("amount")),
		   "deleting an unavailable component's numeric keys does not restore a stale raw copy");
		QJsonObject known = unknown;
		known[QStringLiteral("type")] = QStringLiteral("harpia.blur");
		const ComponentType *type = registry.find(QStringLiteral("harpia.blur"));
		const QJsonObject knownSaved = componentToJson(componentFromJson(known, registry), registry);
		ok(type && knownSaved.value(QStringLiteral("version")).toInt() == type->version,
		   "a registered component continues to save its current schema version");
		ComponentRegistry newlyInstalled;
		ComponentType plugin = *type;
		plugin.id = QStringLiteral("missing.projectEffect");
		plugin.version = 8;
		bool migrationSawOriginal = false;
		plugin.migrate = [&](int version, QJsonObject &payload) {
			migrationSawOriginal = version == 7 && payload.value(QStringLiteral("keys")) == unknown.value(QStringLiteral("keys"));
			payload[QStringLiteral("enabled")] = true;
			payload[QStringLiteral("inMs")] = 55;
			payload[QStringLiteral("outMs")] = 95;
			QJsonObject props = payload.value(QStringLiteral("props")).toObject();
			props[QStringLiteral("migrated")] = true;
			payload[QStringLiteral("props")] = props;
		};
		newlyInstalled.add(plugin);
		const QJsonObject beforeMigration = componentToJson(missing, newlyInstalled);
		const ComponentInstance migrated = componentFromJson(beforeMigration, newlyInstalled);
		ok(beforeMigration.value(QStringLiteral("version")).toInt() == 7 && migrationSawOriginal &&
		   migrated.enabled && migrated.inMs == 55 && migrated.outMs == 95 &&
		   migrated.props.value(QStringLiteral("migrated")).toBool() &&
		   componentToJson(migrated, newlyInstalled).value(QStringLiteral("version")).toInt() == 8,
		   "installing a missing plugin preserves the old schema until its migration reads the original payload");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
