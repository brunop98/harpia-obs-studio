// Saving a project and opening it must give back what you had.
//
// This is the shape of bug that hides best: everything looks right on screen,
// the file loads without complaint, and one setting has quietly reverted. It is
// only visible if something compares the whole model before and after -- which
// is what this does, with every field set to a non-default value so anything
// the serialiser skips shows up as a difference.
//
// It found one: the Bezier easing handles on a keyframe were written only while
// that key's ease WAS Bezier. Tuning a Bezier and then flipping the ease to
// something else to compare is an ordinary thing to do, and doing it before a
// save threw the tuning away -- the handles came back as the 0.42/0.58 default.
// Three separate key types had the same gate (transform channels, spotlight
// keys, effect-parameter keys); the component keys already did it correctly, by
// writing the handles whenever they are not the defaults.
//
// Two things this deliberately does NOT treat as failures, because they are the
// serialiser being right:
//   - a Video clip's text/fx fields are not written. They are meaningless for
//     that type and the reader never looks at them.
//   - an effect's parameters come back seeded from fxDefaults() and then
//     overlaid, so a parameter added to an effect after the project was saved
//     still gets a sane value rather than a zero.
#include "editor/timeline/TimelineJson.hpp"

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

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	std::printf("\n-- a video clip, every field off its default --\n");
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 7;
	c.srcStartMs = 1234;
	c.srcEndMs = 9876;
	c.speed = 1.75;
	c.outStartMs = 4321;
	c.posX = 0.31;
	c.posY = 0.72;
	c.scale = 1.44;
	c.rotation = 33.5;
	c.opacity = 0.66;
	c.crop = QRect(4, 8, 640, 360);
	{
		// The key that mattered: a NON-Bezier ease carrying tuned handles.
		TlKeyframe k;
		k.tMs = 250;
		k.tf = TlTransform{0.1, 0.2, 2.5, 45.0, 0.5};
		k.pos.on = true;
		k.pos.ease = TlEase::EaseIn;
		k.pos.bez1 = 0.11;
		k.pos.bez2 = 0.91;
		k.scale.on = false;
		k.rot.on = true;
		k.rot.ease = TlEase::Linear;
		k.opacity.on = false;
		c.keys.append(k);
	}
	{
		TlScript sc;
		sc.name = QStringLiteral("wobble");
		sc.params.insert(QStringLiteral("amt"), 3.5);
		c.scripts.append(sc);
	}
	{
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.blur");
		ci.instanceId = QStringLiteral("b1");
		ci.enabled = false;
		ci.props.insert(QStringLiteral("radius"), 12.5);
		ci.keys.insert(QStringLiteral("radius"),
			       QVector<PropKey>{PropKey{0, 1.0, TlEase::EaseOut, 0.3, 0.7},
						PropKey{500, 9.0}});
		c.components.append(ci);
	}
	c.transition.type = TransitionType::WipeUp;
	c.volume = 0.55;
	c.fadeInMs = 123;
	c.fadeOutMs = 456;
	c.fadeInCurve = FadeCurve::Exponential;
	c.fadeOutCurve = FadeCurve::Logarithmic;

	const TlClip back = clipFromJson(clipToJson(c));
	ok(back == c, "it comes back exactly as it went in");
	std::printf("     key ease %d, handles %.2f/%.2f -> %.2f/%.2f\n", int(c.keys[0].pos.ease),
		    c.keys[0].pos.bez1, c.keys[0].pos.bez2,
		    back.keys.isEmpty() ? -1.0 : back.keys[0].pos.bez1,
		    back.keys.isEmpty() ? -1.0 : back.keys[0].pos.bez2);
	ok(!back.keys.isEmpty() && back.keys[0].pos.bez1 == 0.11 && back.keys[0].pos.bez2 == 0.91,
	   "the Bezier handles survive even though the ease is not Bezier");

	std::printf("\n-- a text clip's styling --\n");
	{
		TlClip t;
		t.type = TlClip::Type::Text;
		t.srcEndMs = 2000;
		t.text.text = QStringLiteral("hello\nthere");
		t.text.fontFamily = QStringLiteral("Georgia");
		t.text.fontPx = 71;
		t.text.bold = true;
		t.text.italic = true;
		t.text.color = QColor(1, 2, 3);
		t.text.outlineWidth = 4.5;
		t.text.outlineColor = QColor(4, 5, 6);
		t.text.boxEnabled = true;
		t.text.boxColor = QColor(7, 8, 9);
		t.text.boxOpacity = 0.42;
		t.text.boxPadX = 21;
		t.text.boxPadY = 13;
		t.text.boxRadius = 17;
		t.text.align = 2;
		ok(clipFromJson(clipToJson(t)) == t, "every text field round-trips");
	}

	std::printf("\n-- an effect clip's parameters and keys --\n");
	{
		TlClip fx;
		fx.type = TlClip::Type::Effect;
		fx.srcEndMs = 3000;
		fx.fx.type = FxType::Vignette;
		fx.fx.name = QStringLiteral("myfx");
		fx.fx.enabled = false;
		// From the defaults, because that is what the reader rebuilds.
		fx.fx.params = fxDefaults(FxType::Vignette);
		fx.fx.params[QStringLiteral("amount")] = 0.77;
		fx.fx.keys.append(
			FxKey{120, {{QStringLiteral("amount"), 0.25}}, TlEase::EaseIn, 0.2, 0.8});
		const TlClip fb = clipFromJson(clipToJson(fx));
		ok(fb == fx, "an effect's parameters and keys round-trip");
		ok(!fb.fx.keys.isEmpty() && fb.fx.keys[0].bez1 == 0.2 && fb.fx.keys[0].bez2 == 0.8,
		   "including ITS handles under a non-Bezier ease");
	}

	std::printf("\n-- a spotlight's masks and keys --\n");
	{
		TlClip sp;
		sp.type = TlClip::Type::Effect;
		sp.srcEndMs = 3000;
		sp.fx.type = FxType::InverseSelection;
		sp.fx.params = fxDefaults(FxType::InverseSelection);
		SpotMask m;
		m.shape = SpotShape::Ellipse;
		m.pose = SpotPose{0.3, 0.4, 0.5, 0.6, 20.0, 0.25};
		m.keys.append(SpotKey{80, SpotPose{0.1, 0.2, 0.3, 0.4, 5.0, 0.1}, TlEase::EaseOut,
				      0.15, 0.85});
		sp.fx.spot.masks.append(m);
		const TlClip sb = clipFromJson(clipToJson(sp));
		ok(sb == sp, "a spotlight mask and its keys round-trip");
		ok(!sb.fx.spot.masks.isEmpty() && !sb.fx.spot.masks[0].keys.isEmpty() &&
			   sb.fx.spot.masks[0].keys[0].bez1 == 0.15,
		   "and so do THEIR handles, the third place with the same rule");
	}

	std::printf("\n-- a track, and the whole model --\n");
	{
		TlTrack t;
		t.kind = TlTrack::Kind::Audio;
		t.name = QStringLiteral("Narration");
		t.muted = true;
		t.hidden = true;
		t.locked = true;
		t.ripple = true;
		t.color = QColor(0x12, 0x34, 0x56);
		t.clips.append(c);
		ok(trackFromJson(trackToJson(t)) == t, "a track keeps its switches, colour and clips");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
