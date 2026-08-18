// Bullets that arrive on cue, with the cue written into the caption:
//
//     [0]   Framing the shot
//     [3.5] Setting the exposure
//     [7]   Rolling
//
// Two things have to hold for this to be usable, and the second is the one that
// decides whether anybody keeps the component after trying it once:
//
//   - the timing. A line is on screen from its own second onwards, and "build
//     up" versus "one at a time" is the difference between a list and a caption
//     that changes.
//   - what happens to text that is NOT written in this syntax. An ordinary
//     caption, a line that opens with a bracketed word, a stray "[" -- all of
//     them must survive exactly as typed. A feature that quietly eats part of
//     your text is worse than one that does nothing.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/TextBullets.hpp"

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
static void shows(const QString &caption, qint64 tMs, bool accumulate, const char *want,
		  const char *what)
{
	const QString got = bulletsAt(caption, tMs, accumulate);
	const QString wantS = QString::fromUtf8(want);
	const bool good = got == wantS;
	std::printf("  %s %s\n", good ? "PASS" : "FAIL", what);
	if (!good) {
		std::printf("       at %lldms got %s\n", (long long)tMs,
			    got.isEmpty() ? "<nothing>"
					  : QString(got).replace(QLatin1Char('\n'),
								 QStringLiteral(" / "))
						    .toUtf8()
						    .constData());
		std::printf("            want %s\n",
			    wantS.isEmpty() ? "<nothing>"
					    : QString(wantS)
						      .replace(QLatin1Char('\n'), QStringLiteral(" / "))
						      .toUtf8()
						      .constData());
		++failures;
	}
}

static const QString kThree = QStringLiteral("[0] Framing the shot\n"
					     "[3.5] Setting the exposure\n"
					     "[7] Rolling");

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	std::printf("\n-- the cue is read, and then it is gone --\n");
	{
		const QVector<BulletLine> b = parseBulletLines(kThree);
		ok(b.size() == 3, "three lines in, three lines out");
		ok(b.size() == 3 && b[0].atMs == 0 && b[1].atMs == 3500 && b[2].atMs == 7000,
		   "and their times, with the fraction kept");
		ok(b.size() == 3 && b[1].text == QStringLiteral("Setting the exposure"),
		   "the marker and the space after it are not drawn");
		ok(b.size() == 3 && b[0].timed && b[2].timed, "every line carried its own cue");

		// Indented and padded, because that is how a list looks once you line
		// the words up under each other.
		const QVector<BulletLine> p =
			parseBulletLines(QStringLiteral("  [ 2.5 ]   Rolling"));
		ok(p.size() == 1 && p[0].atMs == 2500, "spaces inside and around the marker");
		ok(p.size() == 1 && p[0].text == QStringLiteral("Rolling"),
		   "and the padding between marker and words is not drawn");
		// Which is the whole point of eating ALL of it: a list is written with
		// the words lined up under each other, and the padding that does the
		// lining up belongs to the markers, not to the caption.
		const QVector<BulletLine> aligned =
			parseBulletLines(QStringLiteral("[0]   Framing\n[3.5] Exposure"));
		ok(aligned.size() == 2 && aligned[0].text == QStringLiteral("Framing") &&
			   aligned[1].text == QStringLiteral("Exposure"),
		   "so an aligned list draws with a straight left edge");
	}

	std::printf("\n-- build up: the list grows --\n");
	{
		shows(kThree, 0, true, "Framing the shot", "the first point at its own second");
		shows(kThree, 3000, true, "Framing the shot", "and still alone just before the next");
		shows(kThree, 3500, true, "Framing the shot\nSetting the exposure",
		      "the second joins it exactly on cue");
		shows(kThree, 20000, true,
		      "Framing the shot\nSetting the exposure\nRolling",
		      "and all three are there at the end");
	}

	std::printf("\n-- one at a time: the caption changes --\n");
	{
		shows(kThree, 3500, false, "Setting the exposure", "the newest point replaces the old");
		shows(kThree, 7000, false, "Rolling", "and again");
		// The whole point of the Choice: at the same instant the two modes
		// must disagree, or one of them is not doing anything.
		ok(bulletsAt(kThree, 7000, true) != bulletsAt(kThree, 7000, false),
		   "the two modes really do differ at the same moment");
	}

	std::printf("\n-- before the first cue, and a list that does not start at zero --\n");
	{
		const QString late = QStringLiteral("[2] Later\n[4] Even later");
		shows(late, 0, true, "", "nothing is on screen before the first point is due");
		shows(late, 1999, true, "", "not a millisecond early either");
		shows(late, 2000, true, "Later", "and then it is");
	}

	std::printf("\n-- a line with no cue rides with the one above it --\n");
	{
		// A bullet that wraps. Both halves must arrive together, in either
		// mode -- in one-at-a-time especially, where showing only the second
		// line of a sentence would be nonsense.
		const QString wrapped = QStringLiteral("[0] First\n"
						       "[5] A point long enough that it\n"
						       "wraps onto a second line");
		shows(wrapped, 4999, false, "First", "the wrapped point is not there yet");
		shows(wrapped, 5000, false, "A point long enough that it\nwraps onto a second line",
		      "and both of its lines arrive together");
		shows(wrapped, 5000, true,
		      "First\nA point long enough that it\nwraps onto a second line",
		      "building up keeps the earlier one too");
	}

	std::printf("\n-- text that is not this syntax is left completely alone --\n");
	{
		ok(!captionHasBulletCues(QStringLiteral("Hello there")), "a plain caption has no cues");
		ok(!captionHasBulletCues(QStringLiteral("[note] to self")),
		   "a bracketed WORD is not a time");
		ok(!captionHasBulletCues(QStringLiteral("[-2] backwards")),
		   "and neither is a negative one");
		ok(!captionHasBulletCues(QStringLiteral("[] empty")), "nor an empty bracket");
		ok(!captionHasBulletCues(QStringLiteral("[3 unclosed")), "nor an unclosed one");
		ok(captionHasBulletCues(kThree), "while a real list is recognised");

		// And the text itself survives intact. This is the check that stops
		// the parser half-eating a line it decided not to claim.
		for (const char *s : {"[note] to self", "[-2] backwards", "[] empty", "[3 unclosed",
				      "a [3] mid-line marker"}) {
			const QString in = QString::fromUtf8(s);
			const QVector<BulletLine> b = parseBulletLines(in);
			ok(b.size() == 1 && b[0].text == in && !b[0].timed,
			   "left exactly as typed");
		}
		// The one that matters most for the component: an ordinary caption
		// asked for at time zero comes back whole, not blank and not trimmed.
		const QString plain = QStringLiteral("Hello there\nsecond line");
		const QVector<BulletLine> pb = parseBulletLines(plain);
		ok(pb.size() == 2 && pb[0].atMs == 0 && pb[1].atMs == 0,
		   "an uncued caption is due from the start, all of it");
		shows(plain, 0, true, "Hello there\nsecond line",
		      "so it reads back exactly as it was written");
	}

	std::printf("\n-- times that do not march forwards --\n");
	{
		// Nothing stops someone writing the list out of order, or reusing a
		// second. "Newest" is the latest time that has ARRIVED, not the last
		// line down the page -- otherwise one-at-a-time shows whichever
		// bullet happens to be typed last.
		const QString jumbled = QStringLiteral("[6] Third\n[2] First\n[4] Second");
		shows(jumbled, 5000, false, "Second",
		      "at 5 s the newest point is the 4 s one, not the last line");
		shows(jumbled, 3000, false, "First", "and at 3 s the 2 s one");
		shows(jumbled, 7000, false, "Third", "the 6 s one only once it is due");
		shows(jumbled, 10000, true, "Third\nFirst\nSecond",
		      "building up keeps the caption's own order, not the clock's");

		// Two lines on the same cue are one group, and stay together.
		const QString pair = QStringLiteral("[0] A\n[4] B\n[4] C");
		shows(pair, 4000, false, "B\nC", "two points sharing a second arrive as one");
	}

	std::printf("\n-- the empty edges --\n");
	{
		shows(QString(), 0, true, "", "an empty caption stays empty");
		const QVector<BulletLine> b = parseBulletLines(QStringLiteral("[1]"));
		ok(b.size() == 1 && b[0].timed && b[0].text.isEmpty(),
		   "a cue with no words is a blank line, on cue");
		shows(QStringLiteral("[0] A\n[2]\n[4] C"), 2000, false, "",
		      "which can be used to clear the caption between points");
	}

	std::printf("\n-- the component, through the stack --\n");
	{
		ComponentRegistry reg;
		registerBuiltinComponents(reg);
		const ComponentType *t = reg.find(QStringLiteral("harpia.textBullets"));
		ok(t != nullptr, "Bullets is registered");
		ok(t && t->stage == Stage::Source, "as a Source component: it decides what the clip SAYS");
		ok(t && t->clipKinds == unsigned(ClipKindText),
		   "and only for captions -- the Add menu will not offer it on a video");
		ok(t && t->props.size() == 1 && t->props[0].type == PropType::Choice &&
			   t->props[0].choices.size() == 2,
		   "with one property, and it is the build-up choice");

		QVector<ComponentInstance> list;
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.textBullets");
		ci.instanceId = QStringLiteral("b1");
		list.append(ci);
		ComponentStack st(list, reg);

		EvalContext ctx;
		ctx.durMs = 10000;
		ctx.canvas = QSize(1920, 1080);

		ctx.tMs = 4000;
		ok(st.evaluatePose(ctx, TlTransform{}, &kThree).text ==
			   QStringLiteral("Framing the shot\nSetting the exposure"),
		   "four seconds in, two points are on screen");

		// The Choice, from the property rather than from the argument: this is
		// what proves the wiring reads "mode" at all.
		list[0].props.insert(QStringLiteral("mode"), 1);
		ComponentStack one(list, reg);
		ok(one.evaluatePose(ctx, TlTransform{}, &kThree).text ==
			   QStringLiteral("Setting the exposure"),
		   "and with One at a time, only the newest");

		// The two controls. A caption with no cues must come back untouched --
		// not blanked, not trimmed -- and a clip with no words at all must be
		// left alone entirely.
		const QString plain = QStringLiteral("Just a caption\nover two lines");
		ok(st.evaluatePose(ctx, TlTransform{}, &plain).text == plain,
		   "an ordinary caption is handed straight back");
		ClipState noText = st.evaluatePose(ctx, TlTransform{});
		ok(!noText.textValid && noText.text.isEmpty(),
		   "and on a clip with no words it does nothing at all");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
