// The typewriter reveal, and the two model additions it needed.
//
// The reveal is pure arithmetic over a string, which is why it lives in its own
// header: the thing that can be wrong is WHERE the cut lands, and that is
// checkable without a canvas, a clip or a window.
//
// The claims:
//
//   - the caption's own text is never rebuilt, only cut. Its spacing, its blank
//     lines and its line breaks are how it was laid out; a reveal that
//     reassembled the words with single spaces would re-flow the block as it
//     typed, which is the bug this shape exists to avoid.
//   - progress 1 shows all of it, exactly, whatever the unit. A typing effect
//     that ends one character short is a caption with a letter missing for the
//     rest of the clip.
//   - a Choice HOLDS between keyframes, like a Bool: there is no half-way
//     between Words and Lines.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/TextTyping.hpp"

#include <QGuiApplication>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eqs(const QString &got, const QString &want, const char *w)
{
	const bool good = got == want;
	std::printf("  %s %s (got \"%s\")\n", good ? "PASS" : "FAIL", w,
		    qPrintable(QString(got).replace(QLatin1Char('\n'), QLatin1String("\\n"))));
	if (!good)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	const QString line = QStringLiteral("Hello there");

	std::printf("\n-- character by character --\n");
	{
		eqs(revealedText(line, 0.0, RevealUnit::Characters), QString(),
		    "nothing at the start");
		eqs(revealedText(line, 1.0, RevealUnit::Characters), line,
		    "and all of it at the end");
		// Rounded up: the first letter appears as soon as the reveal begins.
		ok(!revealedText(line, 0.01, RevealUnit::Characters).isEmpty(),
		   "a hair past zero already shows something");
		eqs(revealedText(line, 6.0 / 11.0, RevealUnit::Characters),
		    QStringLiteral("Hello "), "and it cuts where the progress says");
		// Every step is a prefix of the last: no letter may appear, vanish and
		// come back as the cut moves.
		bool monotonic = true;
		QString prev;
		for (int i = 0; i <= 100; ++i) {
			const QString now = revealedText(line, i / 100.0, RevealUnit::Characters);
			if (!now.startsWith(prev))
				monotonic = false;
			prev = now;
		}
		ok(monotonic, "and the text only ever grows");
	}

	std::printf("\n-- word by word, keeping the layout --\n");
	{
		const QString spaced = QStringLiteral("one  two\nthree");
		eqs(revealedText(spaced, 0.3, RevealUnit::Words), QStringLiteral("one"),
		    "the first word alone");
		eqs(revealedText(spaced, 0.6, RevealUnit::Words), QStringLiteral("one  two"),
		    "then the second, WITH the double space that was between them");
		eqs(revealedText(spaced, 1.0, RevealUnit::Words), spaced,
		    "and the newline survives to the end");
	}

	std::printf("\n-- line by line --\n");
	{
		const QString three = QStringLiteral("first\nsecond\nthird");
		eqs(revealedText(three, 0.3, RevealUnit::Lines), QStringLiteral("first\n"),
		    "one line, its break included so the next one starts below it");
		eqs(revealedText(three, 1.0, RevealUnit::Lines), three, "all three at the end");
		// The boundary, stated on purpose: with three stops, one line covers
		// progress up TO a third, and past it the second is already there.
		eqs(revealedText(three, 1.0 / 3.0, RevealUnit::Lines), QStringLiteral("first\n"),
		    "exactly a third is still the first line");
		eqs(revealedText(three, 0.34, RevealUnit::Lines), QStringLiteral("first\nsecond\n"),
		    "and a hair past it is the second");
	}

	std::printf("\n-- the edges --\n");
	{
		eqs(revealedText(QString(), 0.5, RevealUnit::Characters), QString(),
		    "an empty caption reveals nothing and does not crash");
		eqs(revealedText(line, 5.0, RevealUnit::Characters), line, "past 1 is still all of it");
		eqs(revealedText(line, -3.0, RevealUnit::Characters), QString(), "and below 0 is none");
		eqs(revealedText(QStringLiteral("   "), 1.0, RevealUnit::Words), QStringLiteral("   "),
		    "a caption of nothing but spaces has no words, and still ends complete");
	}

	std::printf("\n-- the caret --\n");
	{
		ok(caretVisibleAt(0.0, 2.0), "lit at the start");
		ok(!caretVisibleAt(0.3, 2.0), "dark half a cycle later");
		ok(caretVisibleAt(0.5, 2.0), "and lit again a full one later");
		ok(caretVisibleAt(123.4, 0.0), "a blink rate of 0 is a steady caret, not an absent one");
		ok(caretVisibleAt(-0.1, 2.0) || !caretVisibleAt(-0.1, 2.0),
		   "a negative time does not divide by zero or hang");
	}

	std::printf("\n-- the component, through the stack --\n");
	{
		ComponentRegistry reg;
		registerBuiltinComponents(reg);
		const ComponentType *t = reg.find(QStringLiteral("harpia.textType"));
		ok(t != nullptr, "Typing is registered");
		ok(t && t->stage == Stage::Source, "as a Source component: it decides what the clip SAYS");
		ok(t && t->clipKinds == unsigned(ClipKindText),
		   "and only for captions — the Add menu will not offer it on a video");

		QVector<ComponentInstance> list;
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.textType");
		ci.instanceId = QStringLiteral("t1");
		ci.props.insert(QStringLiteral("caret"), false); // the text alone, for this check
		list.append(ci);
		ComponentStack st(list, reg);

		EvalContext ctx;
		ctx.durMs = 1000;
		ctx.canvas = QSize(1920, 1080);
		const QString caption = QStringLiteral("abcd");

		ctx.tMs = 500;
		ClipState half = st.evaluatePose(ctx, TlTransform{}, &caption);
		eqs(half.text, QStringLiteral("ab"), "half way through the clip, half the caption");

		ctx.tMs = 1000;
		eqs(st.evaluatePose(ctx, TlTransform{}, &caption).text, caption,
		    "and all of it at the end");

		// The control: with no caption handed in, a text component must be a
		// no-op rather than inventing one.
		ctx.tMs = 500;
		ClipState novideo = st.evaluatePose(ctx, TlTransform{});
		ok(!novideo.textValid && novideo.text.isEmpty(),
		   "on a clip with no words it does nothing at all");
	}

	std::printf("\n-- a Choice holds between keys --\n");
	{
		PropDef d;
		d.key = QStringLiteral("unit");
		d.type = PropType::Choice;
		d.choices = {QStringLiteral("Characters"), QStringLiteral("Words"),
			     QStringLiteral("Lines")};
		const QVector<PropKey> keys{PropKey{0, 0.0, TlEase::Linear, 0, 0},
					    PropKey{1000, 2.0, TlEase::Linear, 0, 0}};
		ok(valueFromKeys(d, keys, 0).toInt() == 0, "the first option before the second key");
		ok(valueFromKeys(d, keys, 999).toInt() == 0,
		   "still the first a millisecond before it — no half-way between two options");
		ok(valueFromKeys(d, keys, 1000).toInt() == 2, "and the second exactly at it");
		// An index from a build with more options than this one must still land
		// on something real.
		ok(valueFromKeys(d, {PropKey{0, 9.0, TlEase::Linear, 0, 0}}, 0).toInt() == 2,
		   "an out-of-range index is clamped to an option that exists");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
