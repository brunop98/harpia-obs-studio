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
#include "TimelineModel.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace harpia {

inline QJsonObject clipToJson(const TlClip &c)
{
	QJsonObject co;
	co[QStringLiteral("type")] = c.type == TlClip::Type::Text    ? QStringLiteral("text")
				     : c.type == TlClip::Type::Image ? QStringLiteral("image")
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
			ko[QStringLiteral("ease")] = k.ease == TlKeyframe::Ease::Linear ? 0 : 1;
			keyArr.append(ko);
		}
		co[QStringLiteral("keys")] = keyArr;
	}
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
	}
	// `peaks` is deliberately absent: it is a waveform cache derived from the
	// source, rebuilt on load rather than stored.
	return co;
}

inline TlClip clipFromJson(const QJsonObject &co)
{
	TlClip c;
	const QString ct = co.value(QStringLiteral("type")).toString();
	c.type = (ct == QLatin1String("text"))    ? TlClip::Type::Text
		 : (ct == QLatin1String("image")) ? TlClip::Type::Image
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
		k.ease = ko.value(QStringLiteral("ease")).toInt(1) == 0 ? TlKeyframe::Ease::Linear
									: TlKeyframe::Ease::EaseInOut;
		c.keys.append(k);
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
	if (c.type == TlClip::Type::Text) {
		const QJsonObject tx = co.value(QStringLiteral("textStyle")).toObject();
		applyTextStyleFromJson(tx, c.text);
		c.text.text = tx.value(QStringLiteral("text")).toString();
	} else {
		c.volume = co.value(QStringLiteral("volume")).toDouble(1.0);
		c.fadeInMs = co.value(QStringLiteral("fadeIn")).toInt(15);
		c.fadeOutMs = co.value(QStringLiteral("fadeOut")).toInt(15);
	}
	return c;
}

inline QJsonObject trackToJson(const TlTrack &t)
{
	QJsonObject to;
	to[QStringLiteral("kind")] = t.kind == TlTrack::Kind::Video ? QStringLiteral("video")
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
	t.kind = to.value(QStringLiteral("kind")).toString() == QLatin1String("audio")
			 ? TlTrack::Kind::Audio
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
