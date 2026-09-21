#include "EditConsole.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cmath>

#if HARPIA_HAVE_QJS
#include "quickjs.h"
#endif

namespace harpia {

namespace {

// Clips in console order: lane by lane from the top, each lane by start time.
struct Slot {
	int track;
	int clip;
};
QVector<Slot> slotsOf(const TimelineModel &m)
{
	QVector<Slot> out;
	for (int t = 0; t < m.tracks.size(); ++t) {
		QVector<int> idx(m.tracks[t].clips.size());
		for (int i = 0; i < idx.size(); ++i)
			idx[i] = i;
		std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
			return m.tracks[t].clips[a].outStartMs < m.tracks[t].clips[b].outStartMs;
		});
		for (int i : idx)
			out.append({t, i});
	}
	return out;
}

} // namespace

QString EditConsole::refName(const TimelineModel &m, int track, int clip)
{
	if (track < 0 || track >= m.tracks.size())
		return QString();
	const TlTrack &t = m.tracks[track];
	int slot = 0;
	for (int i = 0; i < t.clips.size(); ++i)
		if (i != clip && t.clips[i].outStartMs < t.clips[clip].outStartMs)
			++slot;
	return QStringLiteral("%1[%2]").arg(t.name).arg(slot);
}

QString EditConsole::helpText()
{
	return QStringLiteral(
		"clip            the selected clip (the primary one when several are)\n"
		"selection       every selected clip, as an array\n"
		"clips           every clip on the timeline, top lane first, by start\n"
		"tracks          lane name -> its clips, e.g. tracks.V1\n"
		"canvas          { w, h } in pixels        playhead   position, in ms\n"
		"\n"
		"On a clip (read and write):\n"
		"  position      [x, y] in canvas pixels     x, y      the same, one axis\n"
		"  scale         1 = fits the canvas          opacity   0..1\n"
		"On a clip (read only):\n"
		"  name  track  index  start  duration  end (ms)  speed  type  ref\n"
		"\n"
		"print(...)      write a line here           help()    this text\n"
		"run(\"name\")     run a saved template\n"
		"\n"
		"Every line is one undo step. A line that fails changes nothing.");
}

#if !HARPIA_HAVE_QJS

bool EditConsole::available()
{
	return false;
}

EditConsole::Result EditConsole::run(const QString &, const Input &in)
{
	Result r;
	r.model = in.model;
	r.error = QStringLiteral("This build has no scripting engine.");
	return r;
}

#else

namespace {

constexpr int kBudgetMs = 2000; // a runaway loop is stopped, not waited for

QString toQString(JSContext *ctx, JSValueConst v)
{
	const char *s = JS_ToCString(ctx, v);
	const QString out = s ? QString::fromUtf8(s) : QString();
	if (s)
		JS_FreeCString(ctx, s);
	return out;
}

QString takeError(JSContext *ctx)
{
	JSValue e = JS_GetException(ctx);
	QString msg = toQString(ctx, e);
	JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
	if (!JS_IsUndefined(stack)) {
		const QString st = toQString(ctx, stack).trimmed();
		if (!st.isEmpty())
			msg += QStringLiteral("\n") + st;
	}
	JS_FreeValue(ctx, stack);
	JS_FreeValue(ctx, e);
	return msg;
}

// Everything one run needs, reachable from the C callbacks through the
// context's opaque pointer.
struct RunState {
	const EditConsole::Input *in = nullptr;
	QStringList output;
	QElapsedTimer clock;
	int depth = 0; // run() nesting, so a template cannot run itself forever
};

int interrupted(JSRuntime *, void *opaque)
{
	auto *st = static_cast<RunState *>(opaque);
	return st->clock.elapsed() > kBudgetMs ? 1 : 0;
}

JSValue jsPrint(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
	auto *st = static_cast<RunState *>(JS_GetContextOpaque(ctx));
	QStringList parts;
	for (int i = 0; i < argc; ++i) {
		// Objects and arrays print as JSON, which is what you want to see for
		// `print(clip.position)`; strings print bare.
		if (JS_IsObject(argv[i])) {
			JSValue j = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
			parts << toQString(ctx, j);
			JS_FreeValue(ctx, j);
		} else {
			parts << toQString(ctx, argv[i]);
		}
	}
	st->output << parts.join(QLatin1Char(' '));
	return JS_UNDEFINED;
}

JSValue jsHelp(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
	auto *st = static_cast<RunState *>(JS_GetContextOpaque(ctx));
	st->output << EditConsole::helpText();
	return JS_UNDEFINED;
}

JSValue jsRun(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
	auto *st = static_cast<RunState *>(JS_GetContextOpaque(ctx));
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "run(name): which template?");
	const QString name = toQString(ctx, argv[0]);
	if (!st->in->loadTemplate)
		return JS_ThrowReferenceError(ctx, "no templates folder is available here");
	if (st->depth >= 8)
		return JS_ThrowRangeError(ctx, "templates nested too deep (does one run itself?)");
	const QString src = st->in->loadTemplate(name);
	if (src.isEmpty())
		return JS_ThrowReferenceError(ctx, "no template called \"%s\"", name.toUtf8().constData());
	++st->depth;
	const QByteArray utf8 = src.toUtf8();
	JSValue v = JS_Eval(ctx, utf8.constData(), size_t(utf8.size()), name.toUtf8().constData(),
			    JS_EVAL_TYPE_GLOBAL);
	--st->depth;
	return v; // an exception propagates as the caller's error
}

void setNum(JSContext *ctx, JSValue o, const char *k, double v)
{
	JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, v));
}
void setStr(JSContext *ctx, JSValue o, const char *k, const QString &v)
{
	JS_SetPropertyStr(ctx, o, k, JS_NewString(ctx, v.toUtf8().constData()));
}
double getNum(JSContext *ctx, JSValueConst o, const char *k, double fallback)
{
	JSValue v = JS_GetPropertyStr(ctx, o, k);
	double d = fallback;
	if (JS_IsNumber(v))
		JS_ToFloat64(ctx, &d, v);
	JS_FreeValue(ctx, v);
	return std::isfinite(d) ? d : fallback;
}

// The clip object's editable fields are plain data properties. `position` is
// kept consistent with x/y on the way BACK: whichever of them the snippet
// changed wins, position first because it is the documented one.
const char *kClipType(TlClip::Type t)
{
	switch (t) {
	case TlClip::Type::Video: return "video";
	case TlClip::Type::Image: return "image";
	case TlClip::Type::Text: return "text";
	case TlClip::Type::Effect: return "effect";
	}
	return "clip";
}

} // namespace

bool EditConsole::available()
{
	return true;
}

EditConsole::Result EditConsole::run(const QString &source, const Input &in)
{
	Result r;
	r.model = in.model;

	JSRuntime *rt = JS_NewRuntime();
	if (!rt) {
		r.error = QStringLiteral("could not start the scripting engine");
		return r;
	}
	JS_SetMemoryLimit(rt, 64 * 1024 * 1024);
	RunState st;
	st.in = &in;
	JS_SetInterruptHandler(rt, &interrupted, &st);
	JSContext *ctx = JS_NewContext(rt);
	if (!ctx) {
		JS_FreeRuntime(rt);
		r.error = QStringLiteral("could not start the scripting engine");
		return r;
	}
	JS_SetContextOpaque(ctx, &st);

	const double W = std::max(1, in.canvas.width());
	const double H = std::max(1, in.canvas.height());
	TimelineModel &m = r.model;
	JSValue g = JS_GetGlobalObject(ctx);

	// ---- the picture -------------------------------------------------------
	const QVector<Slot> slotList = slotsOf(m);
	JSValue clipsArr = JS_NewArray(ctx);
	JSValue tracksObj = JS_NewObject(ctx);
	QVector<JSValue> clipObjs; // one per slot; freed with the context
	QHash<QPair<int, int>, int> slotOf;
	{
		QHash<int, JSValue> laneArr;
		QHash<int, int> laneCount;
		for (int k = 0; k < slotList.size(); ++k) {
			const Slot s = slotList[k];
			const TlClip &c = m.tracks[s.track].clips[s.clip];
			const TlTransform tf = c.transformAt(in.playheadMs);
			JSValue o = JS_NewObject(ctx);
			JSValue pos = JS_NewArray(ctx);
			JS_SetPropertyUint32(ctx, pos, 0, JS_NewFloat64(ctx, tf.posX * W));
			JS_SetPropertyUint32(ctx, pos, 1, JS_NewFloat64(ctx, tf.posY * H));
			JS_SetPropertyStr(ctx, o, "position", pos);
			setNum(ctx, o, "x", tf.posX * W);
			setNum(ctx, o, "y", tf.posY * H);
			setNum(ctx, o, "scale", tf.scale);
			setNum(ctx, o, "opacity", tf.opacity);
			setNum(ctx, o, "start", double(c.outStartMs));
			setNum(ctx, o, "duration", double(c.outDurationMs()));
			setNum(ctx, o, "end", double(c.outEndMs()));
			setNum(ctx, o, "speed", c.speed);
			setStr(ctx, o, "type", QString::fromLatin1(kClipType(c.type)));
			setStr(ctx, o, "track", m.tracks[s.track].name);
			const int laneIndex = laneCount[s.track]++;
			setNum(ctx, o, "index", laneIndex);
			setStr(ctx, o, "ref", QStringLiteral("%1[%2]").arg(m.tracks[s.track].name).arg(laneIndex));
			QString name;
			if (c.type == TlClip::Type::Text)
				name = c.text.text.left(40);
			else if (in.sourceName)
				name = in.sourceName(c.sourceId);
			setStr(ctx, o, "name", name);
			// Bookkeeping the snippet cannot see (a Symbol would be cleaner; a
			// double-underscore name is enough here and prints in JSON, which
			// helps when debugging a template).
			setNum(ctx, o, "__slot", k);

			JS_SetPropertyUint32(ctx, clipsArr, uint32_t(k), JS_DupValue(ctx, o));
			if (!laneArr.contains(s.track)) {
				JSValue arr = JS_NewArray(ctx);
				laneArr.insert(s.track, arr);
				JS_SetPropertyStr(ctx, tracksObj, m.tracks[s.track].name.toUtf8().constData(),
						  JS_DupValue(ctx, arr));
			}
			JS_SetPropertyUint32(ctx, laneArr[s.track], uint32_t(laneIndex), JS_DupValue(ctx, o));
			clipObjs.append(o);
			slotOf.insert({s.track, s.clip}, k);
		}
		for (auto it = laneArr.begin(); it != laneArr.end(); ++it)
			JS_FreeValue(ctx, it.value());
	}
	JS_SetPropertyStr(ctx, g, "clips", clipsArr);
	JS_SetPropertyStr(ctx, g, "tracks", tracksObj);

	JSValue selArr = JS_NewArray(ctx);
	uint32_t ns = 0;
	for (const auto &p : in.selection) {
		const auto it = slotOf.constFind(p);
		if (it != slotOf.constEnd())
			JS_SetPropertyUint32(ctx, selArr, ns++, JS_DupValue(ctx, clipObjs[it.value()]));
	}
	JS_SetPropertyStr(ctx, g, "selection", selArr);
	if (!in.selection.isEmpty() && slotOf.contains(in.selection.first()))
		JS_SetPropertyStr(ctx, g, "clip", JS_DupValue(ctx, clipObjs[slotOf[in.selection.first()]]));
	else
		JS_SetPropertyStr(ctx, g, "clip", JS_NULL);

	JSValue canvasObj = JS_NewObject(ctx);
	setNum(ctx, canvasObj, "w", W);
	setNum(ctx, canvasObj, "h", H);
	JS_SetPropertyStr(ctx, g, "canvas", canvasObj);
	setNum(ctx, g, "playhead", double(in.playheadMs));
	JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, jsPrint, "print", 1));
	JS_SetPropertyStr(ctx, g, "log", JS_NewCFunction(ctx, jsPrint, "log", 1));
	JS_SetPropertyStr(ctx, g, "help", JS_NewCFunction(ctx, jsHelp, "help", 0));
	JS_SetPropertyStr(ctx, g, "run", JS_NewCFunction(ctx, jsRun, "run", 1));

	// ---- run ----------------------------------------------------------------
	st.clock.start();
	const QByteArray utf8 = source.toUtf8();
	JSValue v = JS_Eval(ctx, utf8.constData(), size_t(utf8.size()), "<console>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(v)) {
		r.error = takeError(ctx);
		if (st.clock.elapsed() > kBudgetMs)
			r.error = QStringLiteral("stopped after %1 s: the line did not finish (an endless loop?)")
					  .arg(kBudgetMs / 1000);
	} else {
		r.ok = true;
		// The value of the last expression, like any console -- so a bare
		// `clip.position` shows the position.
		if (!JS_IsUndefined(v)) {
			if (JS_IsObject(v)) {
				JSValue j = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
				st.output << toQString(ctx, j);
				JS_FreeValue(ctx, j);
			} else {
				st.output << toQString(ctx, v);
			}
		}
	}
	JS_FreeValue(ctx, v);

	// ---- read back ------------------------------------------------------------
	if (r.ok) {
		for (int k = 0; k < slotList.size(); ++k) {
			const Slot s = slotList[k];
			TlClip &c = m.tracks[s.track].clips[s.clip];
			if (m.tracks[s.track].locked)
				continue; // as everywhere else: a locked lane takes no edit
			const TlTransform was = c.transformAt(in.playheadMs);
			JSValue o = clipObjs[k];
			TlTransform tf = was;
			// position first, then x/y override it if they alone changed.
			JSValue pos = JS_GetPropertyStr(ctx, o, "position");
			if (JS_IsArray(pos)) {
				JSValue px = JS_GetPropertyUint32(ctx, pos, 0);
				JSValue py = JS_GetPropertyUint32(ctx, pos, 1);
				double x = was.posX * W, y = was.posY * H;
				if (JS_IsNumber(px))
					JS_ToFloat64(ctx, &x, px);
				if (JS_IsNumber(py))
					JS_ToFloat64(ctx, &y, py);
				JS_FreeValue(ctx, px);
				JS_FreeValue(ctx, py);
				if (std::isfinite(x))
					tf.posX = x / W;
				if (std::isfinite(y))
					tf.posY = y / H;
			}
			JS_FreeValue(ctx, pos);
			const double x = getNum(ctx, o, "x", was.posX * W);
			const double y = getNum(ctx, o, "y", was.posY * H);
			if (std::abs(x - was.posX * W) > 1e-9)
				tf.posX = x / W;
			if (std::abs(y - was.posY * H) > 1e-9)
				tf.posY = y / H;
			tf.scale = std::clamp(getNum(ctx, o, "scale", was.scale), 0.01, 50.0);
			tf.opacity = std::clamp(getNum(ctx, o, "opacity", was.opacity), 0.0, 1.0);

			const bool same = std::abs(tf.posX - was.posX) < 1e-12 &&
					  std::abs(tf.posY - was.posY) < 1e-12 &&
					  std::abs(tf.scale - was.scale) < 1e-12 &&
					  std::abs(tf.opacity - was.opacity) < 1e-12;
			if (!same) {
				c.setBaseTransform(tf); // the Inspector's semantics, see the header
				r.changed = true;
			}
		}
	}
	r.output = st.output.join(QLatin1Char('\n'));

	for (JSValue o : clipObjs)
		JS_FreeValue(ctx, o);
	JS_FreeValue(ctx, g);
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	if (!r.ok)
		r.model = in.model; // belt and braces: an error leaves nothing changed
	return r;
}

#endif // HARPIA_HAVE_QJS

} // namespace harpia
