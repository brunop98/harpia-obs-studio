// The Sound Library and presets.
//
//   - the seven built-ins are always there, in order, whatever is stored; a
//     stored entry only sets the file behind one; custom slots follow;
//   - only what differs from the defaults is stored;
//   - resolve: your file when it exists, else the synthesised built-in,
//     else nothing (a custom slot with no file, or a file that went away);
//   - a preset carries tags by name and rules by tag name and sound slot or
//     file, never ids or lanes;
//   - merging adds missing tags, maps Tagged rules onto the project's tags,
//     skips a rule the project already has, counts missing sounds;
//   - the store round-trips and lists by name.
#include "editor/timeline/SoundLibrary.hpp"
#include "editor/timeline/SoundPresets.hpp"
#include "editor/timeline/SoundRules.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eqi(qint64 got, qint64 want, const char *w)
{
	const bool good = got == want;
	std::printf("  %s %s (got %lld, want %lld)\n", good ? "PASS" : "FAIL", w, (long long)got, (long long)want);
	if (!good)
		++failures;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	QTemporaryDir tmp;

	std::printf("\n-- the library --\n");
	{
		QVector<SoundSlot> s = SoundLibrary::fromJson(QByteArray());
		eqi(s.size(), SoundRules::builtinSounds().size(), "nothing stored: the seven built-ins");
		ok(s[0].name == QLatin1String("Whoosh") && s[0].builtin && s[0].file.isEmpty(), "Whoosh first, built in, no file");
		ok(SoundLibrary::toJson(s) == QByteArray("[]"), "defaults store nothing");

		// Your file behind Pop, and a custom slot.
		QFile mine(tmp.filePath(QStringLiteral("pop.wav")));
		mine.open(QIODevice::WriteOnly);
		mine.write("RIFF");
		mine.close();
		s[1].file = mine.fileName();
		SoundSlot riser;
		riser.name = QStringLiteral("Riser");
		riser.file = tmp.filePath(QStringLiteral("gone.wav")); // never created
		s.append(riser);
		SoundSlot empty;
		empty.name = QStringLiteral("Later");
		s.append(empty);
		const QVector<SoundSlot> back = SoundLibrary::fromJson(SoundLibrary::toJson(s));
		ok(back == s, "the library survives the JSON round trip");
		eqi(QJsonDocument::fromJson(SoundLibrary::toJson(s)).array().size(), 3,
		    "only Pop's file and the two custom slots are stored");

		// A stored entry cannot drop or reorder the built-ins.
		const QVector<SoundSlot> odd = SoundLibrary::fromJson(
			QByteArray(R"([{"name":"Custom","file":""},{"name":"whoosh","file":"x.wav"}])"));
		ok(odd[0].name == QLatin1String("Whoosh") && odd[0].file == QLatin1String("x.wav"),
		   "a stored built-in only sets its file (any case)");
		ok(odd.last().name == QLatin1String("Custom") && !odd.last().builtin, "custom slots come after");

		const QString cache = tmp.filePath(QStringLiteral("cache"));
		ok(SoundLibrary::resolve(s, QStringLiteral("Pop"), cache) == QFileInfo(mine.fileName()).absoluteFilePath(),
		   "a slot with your file plays your file");
		const QString wh = SoundLibrary::resolve(s, QStringLiteral("whoosh"), cache);
		ok(wh.endsWith(QLatin1String("whoosh.wav")) && QFileInfo(wh).size() > 44,
		   "a built-in without a file plays its synthesised sound");
		ok(SoundLibrary::resolve(s, QStringLiteral("Swoosh up"), cache).endsWith(QLatin1String("swoosh_up.wav")),
		   "spaces in a name make a safe file name");
		ok(SoundLibrary::resolve(s, QStringLiteral("Riser"), cache).isEmpty(), "a custom slot whose file is gone plays nothing");
		ok(SoundLibrary::resolve(s, QStringLiteral("Later"), cache).isEmpty(), "nor one with no file");
		ok(SoundLibrary::resolve(s, QStringLiteral("Nope"), cache).isEmpty(), "an unknown slot resolves to nothing");
		s[0].file = tmp.filePath(QStringLiteral("missing.wav"));
		ok(SoundLibrary::resolve(s, QStringLiteral("Whoosh"), cache) == wh,
		   "a built-in whose file went away falls back to its own sound");
	}

	std::printf("\n-- presets --\n");
	{
		// Project A: two tags, three rules (one Tagged, one on a lane).
		TimelineModel a;
		const int intro = a.ensureTag(QStringLiteral("intro"), QColor(Qt::red));
		a.ensureTag(QStringLiteral("callout"));
		TlSoundRule r1 = SoundRules::ruleForTag(intro);
		r1.id = 1;
		r1.sourceId = 10; // library: Whoosh
		r1.volume = 0.7;
		r1.offsetMs = -100;
		TlSoundRule r2;
		r2.id = 2;
		r2.trigger = TlSoundRule::Trigger::ImageAppears;
		r2.sourceId = 11; // a file
		r2.lane = 3;
		TlSoundRule r3;
		r3.id = 3;
		r3.trigger = TlSoundRule::Trigger::AnyTransition;
		r3.sourceId = 99; // nothing known: dropped
		a.soundRules << r1 << r2 << r3;
		auto refOf = [](int id) {
			SoundRef r;
			if (id == 10)
				r.slot = QStringLiteral("Whoosh");
			else if (id == 11)
				r.file = QStringLiteral("/sfx/click.wav");
			return r;
		};
		const QJsonObject p = SoundPresets::toJson(a, QStringLiteral("Tutorial"), refOf);
		eqi(p.value(QStringLiteral("tags")).toArray().size(), 2, "the preset carries both tags");
		const QJsonArray rules = p.value(QStringLiteral("rules")).toArray();
		eqi(rules.size(), 2, "and the two rules with a sound");
		const QJsonObject j1 = rules[0].toObject();
		ok(j1.value(QStringLiteral("tagName")).toString() == QLatin1String("intro") && !j1.contains(QStringLiteral("tag")),
		   "a Tagged rule names its tag, no id");
		ok(j1.value(QStringLiteral("slot")).toString() == QLatin1String("Whoosh") && !j1.contains(QStringLiteral("source")),
		   "a library sound is written as its slot");
		ok(rules[1].toObject().value(QStringLiteral("file")).toString() == QLatin1String("/sfx/click.wav"),
		   "another sound as its file");
		eqi(rules[1].toObject().value(QStringLiteral("lane")).toInt(), -1, "lanes are dropped");

		// Project B already has "Intro" (other case) and a matching image rule.
		TimelineModel b;
		const int bIntro = b.ensureTag(QStringLiteral("Intro"));
		TlSoundRule have;
		have.id = 1;
		have.trigger = TlSoundRule::Trigger::ImageAppears;
		have.sourceId = 51;
		b.soundRules << have;
		auto sourceFor = [](const SoundRef &r) {
			if (r.slot == QLatin1String("Whoosh"))
				return 50;
			if (r.file == QLatin1String("/sfx/click.wav"))
				return 51;
			return -1;
		};
		PresetMergeResult res = SoundPresets::merge(b, p, sourceFor);
		eqi(res.tagsAdded, 1, "one new tag (intro already there, by name)");
		eqi(b.tags.size(), 2, "B has two tags");
		eqi(res.rulesAdded, 1, "the tag rule is added");
		eqi(res.rulesSkipped, 1, "the image rule B already had is skipped");
		const TlSoundRule &added = b.soundRules.last();
		ok(added.trigger == TlSoundRule::Trigger::Tagged && added.tagId == bIntro, "mapped onto B's own Intro tag");
		ok(added.sourceId == 50 && added.id == 2 && std::abs(added.volume - 0.7) < 1e-9 && added.offsetMs == -100,
		   "with its sound, a fresh id, and its settings");
		ok(added.soundName == QLatin1String("Whoosh"), "named after its slot");
		res = SoundPresets::merge(b, p, sourceFor);
		ok(res.rulesAdded == 0 && res.tagsAdded == 0 && res.rulesSkipped == 2, "loading twice adds nothing");
		TimelineModel c;
		res = SoundPresets::merge(c, p, [](const SoundRef &) { return -1; });
		ok(res.soundsMissing == 2 && c.soundRules.isEmpty() && c.tags.size() == 2,
		   "sounds that cannot be found are counted, tags still come");

		// The store.
		SoundPresets::setDirForTesting(tmp.filePath(QStringLiteral("presets")));
		ok(SoundPresets::names().isEmpty(), "an empty store lists nothing");
		ok(SoundPresets::save(QStringLiteral("Tutorial"), p), "save");
		ok(SoundPresets::save(QStringLiteral("a/b: odd?"), p), "a name with odd characters saves too");
		ok(SoundPresets::fileFor(QStringLiteral("a/b: odd?")).endsWith(QLatin1String("a_b_ odd_.json")), "as a safe file name");
		const QStringList n = SoundPresets::names();
		ok(n.size() == 2 && n.contains(QStringLiteral("Tutorial")) && n.contains(QStringLiteral("a/b: odd?")),
		   "both listed by their real names");
		ok(SoundPresets::load(QStringLiteral("Tutorial")).value(QStringLiteral("rules")).toArray().size() == 2,
		   "load gives the rules back");
		ok(SoundPresets::remove(QStringLiteral("Tutorial")) && SoundPresets::names().size() == 1, "remove");
		ok(SoundPresets::load(QStringLiteral("Tutorial")).isEmpty(), "a removed preset loads as nothing");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
