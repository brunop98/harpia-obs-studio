#pragma once

// The component runtime's vocabulary.
//
// A clip is a bag of components, the way a GameObject is a bag of behaviours.
// Everything a clip does — how fast it plays, where it sits, what colour it is,
// what it blurs — is one component in an ordered list, and a user-written
// component is the same kind of thing as a built-in one.
//
// THE ONE RULE THAT MAKES THIS WORK: a component is a pure function of time.
// `evaluate(t)`, never `update(dt)`. Unity can assume monotonic time and let a
// behaviour accumulate state across frames; an editor cannot. The user scrubs
// to 00:47 without rendering the 1,400 frames before it, the exporter renders
// on another thread and possibly out of order, and preview and export have to
// agree to the pixel. "Always Rotate" is the trap in miniature: written as
// `angle += speed * dt` it is wrong in a way that only shows up when someone
// drags the playhead; written as `angle = speed * t` it is right everywhere.
//
// Purity is also what pays for the performance goals. A pure component hashes
// to its (type, version, props-at-t, upstream) and so can be cached per frame;
// a clip of only pure components can have its frames rendered on every core at
// once. Components that genuinely need history opt out by declaring themselves
// impure, which costs their clip both of those things — a price worth making
// visible rather than paying silently everywhere.

#include "../timeline/Ease.hpp"
#include "../timeline/TlTransform.hpp"

#include <QJsonObject>
#include <QMap>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>
#include <memory>

class QImage;

namespace harpia {

// Where in the frame's construction a component gets its turn. Named stages
// rather than Unity's free-integer execution order, which is a well-known
// source of "why does this run before that". The order here is the pipeline's
// actual order, so a Speed component CANNOT run after a Blur — time is resolved
// before pixels exist, and the type system says so.
enum class Stage {
	Time,      // choose which source instant this output frame samples
	Source,    // produce pixels (decode, caption, generator)
	Transform, // pose: position, scale, rotation, opacity
	Pixel,     // per-pixel work on the clip's own image
	Composite, // how it merges into what is below
	Audio,     // gain, pan, filters
};
inline constexpr int kStageCount = 6;
const char *stageName(Stage s);

// Choice is a fixed list of named options -- "reveal by Characters / Words /
// Lines". Stored as its index, like everything else, so persistence, keyframes
// and scripts need no new case; it HOLDS between keys exactly as a Bool does,
// because there is no half-way between two options.
enum class PropType { Float, Int, Bool, Color, Choice };

// One editable property. This single declaration drives the Inspector row, the
// keyframe track, the serialised key and the script binding — the same trick
// FxParamDef and the shaders' //@param already use, made universal so a new
// property cannot be added in one place and forgotten in three others.
struct PropDef {
	QString key;   // stable; the serialisation and script name
	QString label; // what the Inspector shows
	PropType type = PropType::Float;
	double min = 0.0, max = 1.0, def = 0.0;
	bool keyframeable = true;
	QString help; // tooltip
	// PropType::Choice only: the option names, in index order. The Inspector
	// draws a dropdown from these, so the number stored is never something the
	// user has to know.
	QStringList choices;

	bool operator==(const PropDef &o) const
	{
		return key == o.key && label == o.label && type == o.type && min == o.min &&
		       max == o.max && def == o.def && keyframeable == o.keyframeable;
	}
};

// A keyframe on one property. Times are CLIP-relative, like FxKey — so moving a
// clip carries its animation with it.
struct PropKey {
	qint64 tMs = 0;
	double v = 0.0;
	TlEase ease = TlEase::EaseInOut;
	double bez1 = 0.42, bez2 = 0.58;

	bool operator==(const PropKey &o) const
	{
		return tMs == o.tMs && v == o.v && ease == o.ease && bez1 == o.bez1 && bez2 == o.bez2;
	}
};

// Values resolved for one instant. Keyed by PropDef::key.
using PropBag = QMap<QString, QVariant>;

// One component attached to one clip: which type, and its settings.
struct ComponentInstance {
	QString typeId;     // "harpia.blur", "acme.glitch" — namespaced, stable
	QString instanceId; // unique on the clip; how other components refer to it
	bool enabled = true;

	// Ramp times, in milliseconds, both 0 by default -- which means "no
	// envelope", exactly the all-or-nothing behaviour every project written
	// before this had. Non-zero fades the component in over inMs and out over
	// outMs, so the common shape (arrive, hold, leave) needs no keyframes at
	// all. See ComponentEnvelope.hpp for what the weight then does, which
	// depends on the stage.
	qint64 inMs = 0;
	qint64 outMs = 0;

	PropBag props;                        // static values, by key
	QMap<QString, QVector<PropKey>> keys; // animated properties, by key

	// A project may carry components this build has never heard of — a plugin
	// the user has not installed, or one from a newer version. Their JSON is
	// kept here verbatim and written back out untouched, so opening and saving
	// a project cannot destroy work that this copy of the program cannot
	// render. Premiere and Resolve both silently drop unknown effects; the cost
	// of not doing that is this one field.
	QJsonObject unknown;

	bool operator==(const ComponentInstance &o) const
	{
		return typeId == o.typeId && instanceId == o.instanceId && enabled == o.enabled &&
		       inMs == o.inMs && outMs == o.outMs && props == o.props && keys == o.keys &&
		       unknown == o.unknown;
	}
	bool operator!=(const ComponentInstance &o) const { return !(*this == o); }
};

// What a component is told about the moment it is being evaluated at.
struct EvalContext {
	qint64 tMs = 0;   // into the clip; 0 is its first frame
	qint64 outMs = 0; // on the timeline
	qint64 durMs = 1; // the clip's length
	double fps = 30.0;
	QSize canvas;
	PropBag p; // this component's properties, already resolved at tMs

	double tSec() const { return double(tMs) / 1000.0; }
	double u() const { return durMs > 0 ? double(tMs) / double(durMs) : 0.0; } // 0..1
	double f(const char *key, double fallback = 0.0) const
	{
		const auto it = p.find(QString::fromLatin1(key));
		return it == p.end() ? fallback : it->toDouble();
	}
	bool b(const char *key, bool fallback = false) const
	{
		const auto it = p.find(QString::fromLatin1(key));
		return it == p.end() ? fallback : it->toBool();
	}
};

// What components read and write. Each stage touches its own part, and the
// parts are separate so a Transform component physically cannot reach the
// pixels — the capability is expressed by what it can see, not by a rule
// somebody has to remember.
struct ClipState {
	// Time stage. Multiplied, not assigned, so two time components compose
	// instead of the last one winning.
	double timeScale = 1.0;

	// Transform stage. Seeded from the clip's pose/keyframes, so a component
	// that offsets rather than overwrites composes with hand-set values — the
	// same contract ctx.base gives the existing transform scripts.
	TlTransform xf;

	// Pixel stage. Null in the stages that run before pixels exist.
	QImage *frame = nullptr;

	// Source stage, text clips only. Seeded from the clip's own caption; a
	// component may rewrite it -- a typing effect hands back a prefix -- and
	// the compositor draws THIS rather than the stored string. `caret`, if any,
	// is drawn immediately after it.
	//
	// The clip's own text is never touched: what is stored is what the user
	// typed, and what is drawn is what the components made of it. That is the
	// same contract the transform has, where `xf` is seeded from the pose and
	// the clip's own fields stay put.
	QString text;
	QString caret;
	bool textValid = false; // false on every clip that is not a caption
};

// The behaviour itself. Stateless: everything it needs arrives in EvalContext,
// which is what lets one instance serve every thread and every frame.
class IComponent {
public:
	virtual ~IComponent() = default;
	virtual void evaluate(const EvalContext &ctx, ClipState &io) const = 0;
};

// A button a component puts in its own box in the Inspector.
//
// Sliders cover "what is this value"; some things are a verb instead -- reset
// this, fit to the frame, bake the animation down. Those were previously
// buttons hand-placed in the Inspector next to whatever they acted on, which
// meant a new one was a change to the window rather than to the component that
// owns it, and a user-written component could not have one at all.
//
// `run` is the whole implementation, and it sees only the component's own
// properties. That is deliberate: an action that can reach the clip, the
// timeline or the window is an action whose effect depends on where it is
// invoked from, and it would be applied N times over a multi-clip selection
// with N different answers. Restricted to the property bag, "press it with five
// clips selected" has exactly one meaning -- do it to each of them -- which is
// how every other edit in this panel already behaves.
struct ComponentAction {
	QString id;    // stable; what the panel sends back
	QString label; // the button's text
	QString help;  // tooltip
	// Mutate the properties. Anything not written is left as it is, so an
	// action that resets one channel does not silently reset the others.
	std::function<void(PropBag &)> run;
};

// The registration record: everything the editor knows about a kind of
// component without having one.
// Which kinds of clip a component can be put on. A bitmask rather than the
// timeline's own enum, because this header is included BY the timeline model
// and cannot include it back.
enum ClipKind : unsigned {
	ClipKindVideo = 1u,
	ClipKindText = 2u,
	ClipKindImage = 4u,
	ClipKindEffect = 8u,
	ClipKindAll = ClipKindVideo | ClipKindText | ClipKindImage | ClipKindEffect,
};

struct ComponentType {
	QString id;
	QString displayName;
	QString category; // groups the Add Component menu
	int version = 1;  // bumped when props change shape; drives migrate()
	Stage stage = Stage::Pixel;
	QVector<PropDef> props;

	// Buttons this component shows under its properties. Empty for most.
	QVector<ComponentAction> actions;

	// Ordering within a stage. Not called `requires` — that is a keyword from
	// C++20 on, and this project will not want to rename a public field the day
	// it bumps the standard.
	QStringList requiresIds;
	QStringList conflictsIds;

	// False for a component every clip implicitly has, so it does not appear in
	// the Add Component menu. Unity's own Transform works this way: you cannot
	// add a second one and you cannot remove the one you have.
	bool addable = true;

	// Everything applies everywhere unless it says otherwise, so no existing
	// component changes behaviour. A typing effect is meaningless on a video
	// clip and simply does not appear in its Add Component menu.
	unsigned clipKinds = ClipKindAll;

	// True when an output pixel depends only on the INPUT PIXEL AT THE SAME
	// POSITION -- a brightness curve, a colour matrix, a hue rotation. Such a
	// component gives the same answer whether it is handed the whole frame or
	// any sub-rect of it, which is what lets an area-bounded effect grade only
	// the region it covers instead of the whole picture and throwing most of it
	// away. Measured: 0.48 ms against 2.24 ms for a quarter-area effect at 1080p.
	//
	// False by default, and false for anything that reads its NEIGHBOURS -- a
	// blur, a sharpen, a shader. Handing one of those a sub-rect would make it
	// sample the cut edge instead of the pixels that are really there, and the
	// area would get a visible seam.
	bool pointOp = false;

	// False only for components that genuinely need history. See the header
	// comment: an impure component costs its clip frame-parallel export and
	// correct scrubbing, so this is deliberately awkward to reach for.
	bool pure = true;

	QString help;

	std::function<std::unique_ptr<IComponent>()> make;

	// Optional: bring an older serialised form up to `version`.
	std::function<void(int fromVersion, QJsonObject &json)> migrate;
};

// Resolve one property at one instant: its keyframes if it has any, otherwise
// its static value, otherwise the type's default. Exposed because the Inspector
// needs the same answer the renderer gets, and computing it twice differently
// is how a panel comes to disagree with the picture.
// The declaration of one property by key, or null. Public because callers that
// key a value need its TYPE to know how to store it.
const PropDef *defFor(const ComponentType &type, const QString &key);

// A property value as a PropKey would store it.
//
// The inverse of what propAt does on the way out, and it exists because
// QVariant(QColor).toDouble() is silently 0 -- keying a colour by calling
// toDouble() on it would write black and look like the key did not take.
double propKeyValue(const PropDef &def, const QVariant &v);

// One property's keyframe track, resolved at `tMs`. Public because the keyframe
// editor draws a component property's curve with it -- the shape it draws has
// to be the shape the renderer produces, easing, held Bools, per-channel colour
// and all, and that is only guaranteed if it is the same function.
QVariant valueFromKeys(const PropDef &d, const QVector<PropKey> &keys, qint64 tMs);

QVariant propAt(const ComponentType &type, const ComponentInstance &inst, const QString &key,
		qint64 tMs);

// Every property of one component at one instant.
PropBag resolveProps(const ComponentType &type, const ComponentInstance &inst, qint64 tMs);

} // namespace harpia
