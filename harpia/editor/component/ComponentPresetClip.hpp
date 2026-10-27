#pragma once

// Transform and Speed are components in the Inspector, but their state lives
// on the clip itself. A preset must use those fields rather than attach an
// invisible second Transform/Speed to the component stack.

#include "../timeline/TimelineJson.hpp"

#include <cmath>
#include <limits>
#include <utility>

namespace harpia::component_presets {

inline bool isPinnedComponentType(const QString &typeId)
{
	return typeId == QLatin1String("harpia.transform") || typeId == QLatin1String("harpia.speed");
}

inline ComponentInstance snapshotPinnedComponent(const TlClip &clip, const QString &typeId)
{
	ComponentInstance snapshot;
	if (!isPinnedComponentType(typeId))
		return snapshot;
	snapshot.typeId = typeId;
	if (typeId == QLatin1String("harpia.speed")) {
		snapshot.props.insert(QStringLiteral("factor"), clip.speed);
		return snapshot;
	}
	snapshot.props = {{QStringLiteral("posX"), clip.posX}, {QStringLiteral("posY"), clip.posY},
			  {QStringLiteral("scale"), clip.scale}, {QStringLiteral("rotation"), clip.rotation},
			  {QStringLiteral("opacity"), clip.opacity}};
	// Reuse the project keyframe format for easing/channel/path compatibility.
	// Only the keys are retained; source IDs, content and timeline position never
	// become part of a Transform preset.
	TlClip holder;
	holder.keys = clip.keys;
	QJsonArray keys = clipToJson(holder).value(QStringLiteral("keys")).toArray();
	for (qsizetype i = 0; i < keys.size(); ++i) {
		QJsonObject key = keys[i].toObject();
		const TlKeyframe &original = clip.keys[i];
		// Keep dormant manual handles too: switching back to manual after using
		// a preset should restore the handle positions the author tuned.
		key.insert(QStringLiteral("manual"), original.handlesManual);
		key.insert(QStringLiteral("broken"), original.handlesBroken);
		key.insert(QStringLiteral("hand"), QJsonArray{original.inX, original.inY, original.outX, original.outY});
		keys[i] = key;
	}
	snapshot.unknown.insert(QStringLiteral("presetTransformVersion"), 1);
	snapshot.unknown.insert(QStringLiteral("presetTransformKeys"), keys);
	return snapshot;
}

namespace clip_detail {
inline double finite(double value, double fallback)
{
	return std::isfinite(value) ? value : fallback;
}
inline double property(const ComponentInstance &preset, const char *key, double fallback)
{
	bool valid = false;
	const double value = preset.props.value(QLatin1String(key)).toDouble(&valid);
	return valid ? finite(value, fallback) : fallback;
}
inline void sanitizeTransform(TlTransform &pose)
{
	pose.posX = finite(pose.posX, 0.5);
	pose.posY = finite(pose.posY, 0.5);
	pose.scale = finite(pose.scale, 1.0);
	if (pose.scale <= 0.0)
		pose.scale = 1.0;
	pose.rotation = finite(pose.rotation, 0.0);
	pose.opacity = std::clamp(finite(pose.opacity, 1.0), 0.0, 1.0);
}
} // namespace clip_detail

// Returns whether this is a supported pinned preset. It intentionally does
// not emit edits or ripple: the caller applies it to the selection in one
// TimelineView edit, which owns locking, ripple and undo.
inline bool applyPinnedComponentPreset(TlClip &clip, const ComponentInstance &preset)
{
	if (preset.typeId == QLatin1String("harpia.speed")) {
		if (clip.freeDuration())
			return false;
		clip.speed = std::clamp(clip_detail::property(preset, "factor", 1.0), 0.1, 20.0);
		return true;
	}
	if (preset.typeId != QLatin1String("harpia.transform"))
		return false;
	const QJsonValue version = preset.unknown.value(QStringLiteral("presetTransformVersion"));
	const QJsonValue rawKeys = preset.unknown.value(QStringLiteral("presetTransformKeys"));
	if ((!version.isUndefined() && version != QJsonValue(1)) ||
	    (!rawKeys.isUndefined() && !rawKeys.isArray()))
		return false;
	QJsonArray sanitized;
	for (const QJsonValue &value : rawKeys.toArray()) {
		if (!value.isObject())
			return false;
		QJsonObject key = value.toObject();
		// clipFromJson reads milliseconds as qint64. Check before its cast so
		// edited/corrupt preset files cannot trigger an out-of-range conversion.
		const double time = key.value(QStringLiteral("t")).toDouble();
		constexpr double maxTime = double(std::numeric_limits<qint64>::max() / 4);
		if (!std::isfinite(time) || time < 0.0 || time > maxTime)
			return false;
		key.insert(QStringLiteral("t"), time);
		sanitized.append(key);
	}
	QJsonObject holder;
	holder.insert(QStringLiteral("keys"), sanitized);
	QVector<TlKeyframe> keys = clipFromJson(holder).keys;
	for (qsizetype i = 0; i < keys.size(); ++i) {
		TlKeyframe &key = keys[i];
		const QJsonObject serialized = sanitized[i].toObject();
		key.handlesManual = serialized.value(QStringLiteral("manual")).toBool(key.handlesManual);
		key.handlesBroken = serialized.value(QStringLiteral("broken")).toBool(key.handlesBroken);
		clip_detail::sanitizeTransform(key.tf);
		key.inX = clip_detail::finite(key.inX, 0.0);
		key.inY = clip_detail::finite(key.inY, 0.0);
		key.outX = clip_detail::finite(key.outX, 0.0);
		key.outY = clip_detail::finite(key.outY, 0.0);
	}
	std::stable_sort(keys.begin(), keys.end(), [](const TlKeyframe &a, const TlKeyframe &b) { return a.tMs < b.tMs; });
	TlTransform pose{clip_detail::property(preset, "posX", 0.5),
			 clip_detail::property(preset, "posY", 0.5),
			 clip_detail::property(preset, "scale", 1.0),
			 clip_detail::property(preset, "rotation", 0.0),
			 clip_detail::property(preset, "opacity", 1.0)};
	clip_detail::sanitizeTransform(pose);
	clip.setBaseTransform(pose);
	clip.keys = std::move(keys);
	return true;
}

} // namespace harpia::component_presets
