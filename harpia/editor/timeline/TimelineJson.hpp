#pragma once

// The Full-editing timeline as JSON — the project file's "tracks" array.
//
// Lives apart from the window so it can be tested directly: a dropped field
// here loses a user's work silently, and a round-trip check is the only way to
// know every one survives.
//
// Source ids are written and read VERBATIM. Remapping them onto the editor's
// own media pool is the caller's job, because only the caller knows how the
// project's `sources` array lines up with what is currently loaded.

#include "TextStyleJson.hpp"
#include "../component/BuiltinComponents.hpp"
#include "../component/ComponentJson.hpp"
#include "../component/TransformScriptComponent.hpp"
#include "TimelineModel.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace harpia {

// ---- Inverse Selection (Spotlight) -----------------------------------------
// Project-level, so it is written next to the tracks rather than on a clip.

inline QJsonObject spotPoseToJson(const SpotPose &p)
{
	QJsonObject o;
	o[QStringLiteral("cx")] = p.cx;
	o[QStringLiteral("cy")] = p.cy;
	o[QStringLiteral("w")] = p.w;
	o[QStringLiteral("h")] = p.h;
	o[QStringLiteral("rot")] = p.rotation;
	o[QStringLiteral("radius")] = p.radius;
	o[QStringLiteral("visible")] = p.visible;
	return o;
}

inline SpotPose spotPoseFromJson(const QJsonObject &o)
{
	SpotPose p;
	p.cx = o.value(QStringLiteral("cx")).toDouble(0.5);
	p.cy = o.value(QStringLiteral("cy")).toDouble(0.5);
	p.w = std::clamp(o.value(QStringLiteral("w")).toDouble(0.35), 0.001, 8.0);
	p.h = std::clamp(o.value(QStringLiteral("h")).toDouble(0.35), 0.001, 8.0);
	p.rotation = o.value(QStringLiteral("rot")).toDouble(0.0);
	p.radius = std::clamp(o.value(QStringLiteral("radius")).toDouble(0.08), 0.0, 0.5);
	p.visible = std::clamp(o.value(QStringLiteral("visible")).toDouble(1.0), 0.0, 1.0);
	return p;
}

inline QJsonObject spotlightToJson(const SpotlightSpec &s)
{
	QJsonObject o;
	o[QStringLiteral("enabled")] = s.enabled;
	o[QStringLiteral("invert")] = s.invert;
	o[QStringLiteral("dim")] = s.dimOpacity;
	o[QStringLiteral("color")] = s.dimColor.name(QColor::HexRgb);
	o[QStringLiteral("blur")] = s.blur;
	QJsonArray ms;
	for (const SpotMask &m : s.masks) {
		QJsonObject mo;
		mo[QStringLiteral("name")] = m.name;
		mo[QStringLiteral("shape")] = int(m.shape);
		mo[QStringLiteral("enabled")] = m.enabled;
		mo[QStringLiteral("pose")] = spotPoseToJson(m.pose);
		if (!m.keys.isEmpty()) {
			QJsonArray ks;
			for (const SpotKey &k : m.keys) {
				QJsonObject ko;
				ko[QStringLiteral("t")] = double(k.tMs);
				ko[QStringLiteral("pose")] = spotPoseToJson(k.pose);
				ko[QStringLiteral("ease")] = int(k.ease);
				if (k.ease == TlEase::Bezier) {
					ko[QStringLiteral("b1")] = k.bez1;
					ko[QStringLiteral("b2")] = k.bez2;
				}
				ks.append(ko);
			}
			mo[QStringLiteral("keys")] = ks;
		}
		ms.append(mo);
	}
	o[QStringLiteral("masks")] = ms;
	return o;
}

inline SpotlightSpec spotlightFromJson(const QJsonObject &o)
{
	SpotlightSpec s;
	s.enabled = o.value(QStringLiteral("enabled")).toBool(false);
	s.invert = o.value(QStringLiteral("invert")).toBool(false);
	s.dimOpacity = std::clamp(o.value(QStringLiteral("dim")).toDouble(0.65), 0.0, 1.0);
	const QColor c(o.value(QStringLiteral("color")).toString());
	if (c.isValid())
		s.dimColor = c;
	s.blur = std::clamp(o.value(QStringLiteral("blur")).toDouble(0.0), 0.0, 1.0);
	for (const QJsonValue &mv : o.value(QStringLiteral("masks")).toArray()) {
		const QJsonObject mo = mv.toObject();
		SpotMask m;
		m.name = mo.value(QStringLiteral("name")).toString();
		m.shape = spotShapeFromInt(mo.value(QStringLiteral("shape")).toInt(1));
		m.enabled = mo.value(QStringLiteral("enabled")).toBool(true);
		m.pose = spotPoseFromJson(mo.value(QStringLiteral("pose")).toObject());
		for (const QJsonValue &kv : mo.value(QStringLiteral("keys")).toArray()) {
			const QJsonObject ko = kv.toObject();
			SpotKey k;
			k.tMs = qint64(ko.value(QStringLiteral("t")).toDouble());
			k.pose = spotPoseFromJson(ko.value(QStringLiteral("pose")).toObject());
			k.ease = tlEaseFromInt(ko.value(QStringLiteral("ease")).toInt(3));
			k.bez1 = std::clamp(ko.value(QStringLiteral("b1")).toDouble(0.42), 0.0, 1.0);
			k.bez2 = std::clamp(ko.value(QStringLiteral("b2")).toDouble(0.58), 0.0, 1.0);
			m.keys.append(k);
		}
		std::stable_sort(m.keys.begin(), m.keys.end(),
				 [](const SpotKey &a, const SpotKey &b) { return a.tMs < b.tMs; });
		s.masks.append(m);
	}
	return s;
}



inline QJsonObject clipToJson(const TlClip &c)
{
	QJsonObject co;
	co[QStringLiteral("type")] = c.type == TlClip::Type::Text     ? QStringLiteral("text")
				     : c.type == TlClip::Type::Image  ? QStringLiteral("image")
				     : c.type == TlClip::Type::Effect ? QStringLiteral("effect")
								      : QStringLiteral("video");
	co[QStringLiteral("source")] = c.sourceId;
	co[QStringLiteral("srcStart")] = double(c.srcStartMs);
	co[QStringLiteral("srcEnd")] = double(c.srcEndMs);
	co[QStringLiteral("speed")] = c.speed;
	co[QStringLiteral("outStart")] = double(c.outStartMs);
	co[QStringLiteral("posX")] = c.posX;
	co[QStringLiteral("posY")] = c.posY;
	co[QStringLiteral("scale")] = c.scale;
	co[QStringLiteral("rotation")] = c.rotation;
	co[QStringLiteral("opacity")] = c.opacity;
	if (!c.crop.isNull()) {
		QJsonObject cr;
		cr[QStringLiteral("x")] = c.crop.x();
		cr[QStringLiteral("y")] = c.crop.y();
		cr[QStringLiteral("w")] = c.crop.width();
		cr[QStringLiteral("h")] = c.crop.height();
		co[QStringLiteral("crop")] = cr;
	}
	if (!c.keys.isEmpty()) {
		QJsonArray keyArr;
		for (const TlKeyframe &k : c.keys) {
			QJsonObject ko;
			ko[QStringLiteral("t")] = double(k.tMs);
			ko[QStringLiteral("posX")] = k.tf.posX;
			ko[QStringLiteral("posY")] = k.tf.posY;
			ko[QStringLiteral("scale")] = k.tf.scale;
			ko[QStringLiteral("rotation")] = k.tf.rotation;
			ko[QStringLiteral("opacity")] = k.tf.opacity;
			// Per-channel: which channels this key pins and how each leaves.
			// "ease" is still written as the old 0/1 flag so a project saved
			// here can still be opened by an older build.
			ko[QStringLiteral("ease")] =
				k.pos.ease == TlEase::Linear ? 0 : 1;
			QJsonArray chArr;
			for (int l = 0; l < kTlLaneCount; ++l) {
				const TlKeyChannel &ch = k.channel(l);
				QJsonObject cho;
				cho[QStringLiteral("on")] = ch.on;
				cho[QStringLiteral("ease")] = int(ch.ease);
				if (ch.ease == TlEase::Bezier) {
					cho[QStringLiteral("b1")] = ch.bez1;
					cho[QStringLiteral("b2")] = ch.bez2;
				}
				chArr.append(cho);
			}
			ko[QStringLiteral("chan")] = chArr;
			keyArr.append(ko);
		}
		co[QStringLiteral("keys")] = keyArr;
	}
	// Only written when it differs from the default, so a project full of plain
	// cuts stays clean.
	if (c.transition != TlTransition()) {
		QJsonObject tr;
		tr[QStringLiteral("type")] = int(c.transition.type);
		tr[QStringLiteral("enabled")] = c.transition.enabled;
		tr[QStringLiteral("easeOut")] = int(c.transition.easeOut);
		tr[QStringLiteral("easeIn")] = int(c.transition.easeIn);
		tr[QStringLiteral("reverse")] = c.transition.reverse;
		tr[QStringLiteral("softness")] = c.transition.softness;
		co[QStringLiteral("transition")] = tr;
	}
	if (c.type == TlClip::Type::Effect) {
		QJsonObject fo;
		fo[QStringLiteral("kind")] = int(c.fx.type);
		fo[QStringLiteral("name")] = c.fx.name;
		fo[QStringLiteral("enabled")] = c.fx.enabled;
		QJsonObject pv;
		for (auto it = c.fx.params.constBegin(); it != c.fx.params.constEnd(); ++it)
			pv[it.key()] = it.value();
		fo[QStringLiteral("params")] = pv;
		if (!c.fx.keys.isEmpty()) {
			QJsonArray ka;
			for (const FxKey &k : c.fx.keys) {
				QJsonObject ko;
				ko[QStringLiteral("t")] = double(k.tMs);
				QJsonObject kp;
				for (auto it = k.params.constBegin(); it != k.params.constEnd(); ++it)
					kp[it.key()] = it.value();
				ko[QStringLiteral("params")] = kp;
				ko[QStringLiteral("ease")] = int(k.ease);
				if (k.ease == TlEase::Bezier) {
					ko[QStringLiteral("b1")] = k.bez1;
					ko[QStringLiteral("b2")] = k.bez2;
				}
				ka.append(ko);
			}
			fo[QStringLiteral("keys")] = ka;
		}
		if (c.fx.type == FxType::InverseSelection)
			fo[QStringLiteral("spot")] = spotlightToJson(c.fx.spot);
		co[QStringLiteral("fx")] = fo;
	}
	if (!c.components.isEmpty())
		co[QStringLiteral("components")] =
			componentsToJson(c.components, ComponentRegistry::instance());
	if (!c.scripts.isEmpty()) {
		QJsonArray scArr;
		for (const TlScript &s : c.scripts) {
			QJsonObject sc;
			sc[QStringLiteral("name")] = s.name;
			QJsonObject sp;
			for (auto it = s.params.constBegin(); it != s.params.constEnd(); ++it)
				sp[it.key()] = it.value();
			sc[QStringLiteral("params")] = sp;
			scArr.append(sc);
		}
		co[QStringLiteral("scripts")] = scArr;
	}
	if (c.type == TlClip::Type::Text) {
		QJsonObject tx = textStyleToJson(c.text);
		tx[QStringLiteral("text")] = c.text.text; // the words travel with the clip
		co[QStringLiteral("textStyle")] = tx;
	} else {
		co[QStringLiteral("volume")] = c.volume;
		co[QStringLiteral("fadeIn")] = c.fadeInMs;
		co[QStringLiteral("fadeOut")] = c.fadeOutMs;
		co[QStringLiteral("fadeInCurve")] = int(c.fadeInCurve);
		co[QStringLiteral("fadeOutCurve")] = int(c.fadeOutCurve);
	}
	// `peaks` is deliberately absent: it is a waveform cache derived from the
	// source, rebuilt on load rather than stored.
	return co;
}

inline TlClip clipFromJson(const QJsonObject &co)
{
	TlClip c;
	const QString ct = co.value(QStringLiteral("type")).toString();
	c.type = (ct == QLatin1String("text"))     ? TlClip::Type::Text
		 : (ct == QLatin1String("image"))  ? TlClip::Type::Image
		 : (ct == QLatin1String("effect")) ? TlClip::Type::Effect
						   : TlClip::Type::Video;
	c.sourceId = co.value(QStringLiteral("source")).toInt(0);
	c.srcStartMs = qint64(co.value(QStringLiteral("srcStart")).toDouble());
	c.srcEndMs = qint64(co.value(QStringLiteral("srcEnd")).toDouble());
	c.speed = co.value(QStringLiteral("speed")).toDouble(1.0);
	c.outStartMs = qint64(co.value(QStringLiteral("outStart")).toDouble());
	c.posX = co.value(QStringLiteral("posX")).toDouble(0.5);
	c.posY = co.value(QStringLiteral("posY")).toDouble(0.5);
	c.scale = co.value(QStringLiteral("scale")).toDouble(1.0);
	c.rotation = co.value(QStringLiteral("rotation")).toDouble(0.0);
	c.opacity = co.value(QStringLiteral("opacity")).toDouble(1.0);
	if (co.contains(QStringLiteral("crop"))) {
		const QJsonObject cr = co.value(QStringLiteral("crop")).toObject();
		c.crop = QRect(cr.value(QStringLiteral("x")).toInt(),
			       cr.value(QStringLiteral("y")).toInt(),
			       cr.value(QStringLiteral("w")).toInt(),
			       cr.value(QStringLiteral("h")).toInt());
	}
	for (const QJsonValue &kv : co.value(QStringLiteral("keys")).toArray()) {
		const QJsonObject ko = kv.toObject();
		TlKeyframe k;
		k.tMs = qint64(ko.value(QStringLiteral("t")).toDouble());
		k.tf.posX = ko.value(QStringLiteral("posX")).toDouble(0.5);
		k.tf.posY = ko.value(QStringLiteral("posY")).toDouble(0.5);
		k.tf.scale = ko.value(QStringLiteral("scale")).toDouble(1.0);
		k.tf.rotation = ko.value(QStringLiteral("rotation")).toDouble(0.0);
		k.tf.opacity = ko.value(QStringLiteral("opacity")).toDouble(1.0);
		// A project written before per-channel keys has only the old 0/1 flag:
		// every channel is pinned by every key, which is exactly what it meant.
		const TlEase legacy = ko.value(QStringLiteral("ease")).toInt(1) == 0
					      ? TlEase::Linear
					      : TlEase::EaseInOut;
		for (int l = 0; l < kTlLaneCount; ++l) {
			k.channel(l).on = true;
			k.channel(l).ease = legacy;
		}
		const QJsonArray chArr = ko.value(QStringLiteral("chan")).toArray();
		for (int l = 0; l < kTlLaneCount && l < chArr.size(); ++l) {
			const QJsonObject cho = chArr[l].toObject();
			TlKeyChannel &ch = k.channel(l);
			ch.on = cho.value(QStringLiteral("on")).toBool(true);
			ch.ease = tlEaseFromInt(cho.value(QStringLiteral("ease")).toInt(int(legacy)));
			ch.bez1 = std::clamp(cho.value(QStringLiteral("b1")).toDouble(0.42), 0.0, 1.0);
			ch.bez2 = std::clamp(cho.value(QStringLiteral("b2")).toDouble(0.58), 0.0, 1.0);
		}
		c.keys.append(k);
	}
	if (co.contains(QStringLiteral("transition"))) {
		const QJsonObject tr = co.value(QStringLiteral("transition")).toObject();
		c.transition.type = transitionFromInt(tr.value(QStringLiteral("type")).toInt(0));
		c.transition.enabled = tr.value(QStringLiteral("enabled")).toBool(true);
		c.transition.easeOut = tlEaseFromInt(tr.value(QStringLiteral("easeOut")).toInt(0));
		c.transition.easeIn = tlEaseFromInt(tr.value(QStringLiteral("easeIn")).toInt(0));
		c.transition.reverse = tr.value(QStringLiteral("reverse")).toBool(false);
		c.transition.softness =
			std::clamp(tr.value(QStringLiteral("softness")).toDouble(0.0), 0.0, 1.0);
	}
	if (c.type == TlClip::Type::Effect) {
		const QJsonObject fo = co.value(QStringLiteral("fx")).toObject();
		c.fx.type = fxTypeFromInt(fo.value(QStringLiteral("kind")).toInt(0));
		c.fx.name = fo.value(QStringLiteral("name")).toString();
		c.fx.enabled = fo.value(QStringLiteral("enabled")).toBool(true);
		// Start from the defaults so a parameter added to an effect after this
		// project was saved still has a sane value.
		c.fx.params = fxDefaults(c.fx.type);
		const QJsonObject pv = fo.value(QStringLiteral("params")).toObject();
		for (auto it = pv.constBegin(); it != pv.constEnd(); ++it)
			c.fx.params[it.key()] = it.value().toDouble();
		for (const QJsonValue &kv : fo.value(QStringLiteral("keys")).toArray()) {
			const QJsonObject ko = kv.toObject();
			FxKey k;
			k.tMs = qint64(ko.value(QStringLiteral("t")).toDouble());
			const QJsonObject kp = ko.value(QStringLiteral("params")).toObject();
			for (auto it = kp.constBegin(); it != kp.constEnd(); ++it)
				k.params[it.key()] = it.value().toDouble();
			k.ease = tlEaseFromInt(ko.value(QStringLiteral("ease")).toInt(3));
			k.bez1 = std::clamp(ko.value(QStringLiteral("b1")).toDouble(0.42), 0.0, 1.0);
			k.bez2 = std::clamp(ko.value(QStringLiteral("b2")).toDouble(0.58), 0.0, 1.0);
			c.fx.keys.append(k);
		}
		std::stable_sort(c.fx.keys.begin(), c.fx.keys.end(),
				 [](const FxKey &a, const FxKey &b) { return a.tMs < b.tMs; });
		if (c.fx.type == FxType::InverseSelection)
			c.fx.spot = spotlightFromJson(fo.value(QStringLiteral("spot")).toObject());
	}
	c.components = componentsFromJson(co.value(QStringLiteral("components")).toArray(),
					  ComponentRegistry::instance());

	// Migration: effects used to be a single `fx` object on the clip. Turn one
	// into the component that replaced it, keyframes and all, so a project made
	// before the port opens with its grade intact and saves back in the new
	// shape. Only when there are no components yet — a project already migrated
	// still carries `fx` for the older readers, and converting it twice would
	// double the effect.
	// Effect clips only. Guarding on co.contains("fx") alone was wrong: the fx
	// block is not even PARSED for other clip types, so a stray key on a video
	// clip would have produced a spurious Brightness out of default values.
	if (c.type == TlClip::Type::Effect && c.components.isEmpty() &&
	    c.fx.type != FxType::InverseSelection && co.contains(QStringLiteral("fx"))) {
		ComponentInstance ci;
		ci.typeId = effectComponentId(c.fx.type);
		ci.instanceId = QStringLiteral("fx");
		ci.enabled = c.fx.enabled;
		for (auto it = c.fx.params.cbegin(); it != c.fx.params.cend(); ++it)
			ci.props.insert(it.key(), it.value());
		for (const FxKey &k : c.fx.keys)
			for (auto it = k.params.cbegin(); it != k.params.cend(); ++it) {
				PropKey pk;
				pk.tMs = k.tMs;
				pk.v = it.value();
				ci.keys[it.key()].append(pk);
			}
		for (auto it = ci.keys.begin(); it != ci.keys.end(); ++it)
			std::sort(it->begin(), it->end(),
				  [](const PropKey &a, const PropKey &b) { return a.tMs < b.tMs; });
		if (ComponentRegistry::instance().find(ci.typeId))
			c.components.append(ci);
	}

	// Scripts became a stack; projects written before that carry a single
	// "script" object, which reads as a one-entry stack.
	const auto readScript = [](const QJsonObject &sc) {
		TlScript s;
		s.name = sc.value(QStringLiteral("name")).toString();
		const QJsonObject sp = sc.value(QStringLiteral("params")).toObject();
		for (auto it = sp.constBegin(); it != sp.constEnd(); ++it)
			s.params[it.key()] = it.value().toDouble();
		return s;
	};
	if (co.contains(QStringLiteral("scripts"))) {
		for (const QJsonValue &sv : co.value(QStringLiteral("scripts")).toArray()) {
			const TlScript s = readScript(sv.toObject());
			if (!s.name.isEmpty())
				c.scripts.append(s);
		}
	} else if (co.contains(QStringLiteral("script"))) {
		const TlScript s = readScript(co.value(QStringLiteral("script")).toObject());
		if (!s.name.isEmpty())
			c.scripts.append(s);
	}

	// Migration: a transform script used to be an entry in the clip's own
	// `scripts` list. Each becomes the component that replaced it, in the same
	// order -- which is the order they ran in, and now the order they sit in the
	// component list. Only when there are no components yet, for the same reason
	// the fx migration above checks: a project already migrated still carries
	// `scripts` for the older readers, and converting it twice would apply every
	// script twice.
	//
	// A script whose file is gone registers no type, so it is skipped rather
	// than added as a component nothing can render. The `scripts` list keeps it
	// either way, so putting the file back and reopening restores it.
	if (c.components.isEmpty() && !c.scripts.isEmpty()) {
		int n = 0;
		for (const TlScript &sc : c.scripts) {
			ComponentInstance ci;
			ci.typeId = transformScriptComponentId(sc.name);
			if (!ComponentRegistry::instance().find(ci.typeId))
				continue;
			ci.instanceId = QStringLiteral("script%1").arg(n++);
			for (auto it = sc.params.cbegin(); it != sc.params.cend(); ++it)
				ci.props.insert(it.key(), it.value());
			c.components.append(ci);
		}
	}
	if (c.type == TlClip::Type::Text) {
		const QJsonObject tx = co.value(QStringLiteral("textStyle")).toObject();
		applyTextStyleFromJson(tx, c.text);
		c.text.text = tx.value(QStringLiteral("text")).toString();
	} else {
		c.volume = co.value(QStringLiteral("volume")).toDouble(1.0);
		c.fadeInMs = co.value(QStringLiteral("fadeIn")).toInt(15);
		c.fadeOutMs = co.value(QStringLiteral("fadeOut")).toInt(15);
		// Older projects have no curve, and an out-of-range one is not trusted;
		// both fall back to a straight line.
		c.fadeInCurve = fadeCurveFromInt(co.value(QStringLiteral("fadeInCurve")).toInt(0));
		c.fadeOutCurve = fadeCurveFromInt(co.value(QStringLiteral("fadeOutCurve")).toInt(0));
	}
	return c;
}

inline QJsonObject trackToJson(const TlTrack &t)
{
	QJsonObject to;
	to[QStringLiteral("kind")] = t.kind == TlTrack::Kind::Effect ? QStringLiteral("effect")
			     : t.kind == TlTrack::Kind::Video   ? QStringLiteral("video")
								    : QStringLiteral("audio");
	to[QStringLiteral("name")] = t.name;
	to[QStringLiteral("muted")] = t.muted;
	to[QStringLiteral("hidden")] = t.hidden;
	to[QStringLiteral("locked")] = t.locked;
	to[QStringLiteral("ripple")] = t.ripple;
	to[QStringLiteral("color")] = t.color.name(QColor::HexRgb);
	QJsonArray clipArr;
	for (const TlClip &c : t.clips)
		clipArr.append(clipToJson(c));
	to[QStringLiteral("clips")] = clipArr;
	return to;
}

inline TlTrack trackFromJson(const QJsonObject &to)
{
	TlTrack t;
	const QString kindStr = to.value(QStringLiteral("kind")).toString();
	t.kind = (kindStr == QLatin1String("audio"))    ? TlTrack::Kind::Audio
		 : (kindStr == QLatin1String("effect")) ? TlTrack::Kind::Effect
							: TlTrack::Kind::Video;
	t.name = to.value(QStringLiteral("name")).toString();
	t.muted = to.value(QStringLiteral("muted")).toBool(false);
	t.hidden = to.value(QStringLiteral("hidden")).toBool(false);
	t.locked = to.value(QStringLiteral("locked")).toBool(false);
	t.ripple = to.value(QStringLiteral("ripple")).toBool(false);
	if (const QColor tc(to.value(QStringLiteral("color")).toString()); tc.isValid())
		t.color = tc;
	for (const QJsonValue &cv : to.value(QStringLiteral("clips")).toArray())
		t.clips.append(clipFromJson(cv.toObject()));
	return t;
}


} // namespace harpia
