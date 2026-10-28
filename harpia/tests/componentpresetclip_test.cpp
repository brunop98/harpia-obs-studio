#include "editor/component/ComponentPresetClip.hpp"

#include <QCoreApplication>
#include <QJsonDocument>

#include <cstdio>
#include <limits>

using namespace harpia;
using namespace harpia::component_presets;
namespace {
int failures = 0;
void check(bool value, const char *description)
{
	std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
	failures += !value;
}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	check(isPinnedComponentType(QStringLiteral("harpia.transform")) &&
	      isPinnedComponentType(QStringLiteral("harpia.speed")) &&
	      !isPinnedComponentType(QStringLiteral("harpia.blur")), "only native Transform and Speed are pinned preset types");
	TlClip source;
	source.posX = 2.5;
	source.posY = -1.25;
	source.scale = 20.0;
	source.rotation = 720;
	source.opacity = 0.65;
	source.speed = 2.75;
	TlKeyframe first;
	first.tMs = 0;
	first.tf = {0.2, 0.3, 3.0, -720, 0.3};
	first.pos.ease = TlEase::Bezier;
	first.pos.bez1 = 0.13;
	first.pos.bez2 = 0.93;
	first.scale.on = false;
	first.rot.ease = TlEase::Linear;
	first.opacity.on = false;
	first.curvedPath = true;
	first.handlesManual = true;
	first.handlesBroken = true;
	first.inX = -0.25;
	first.inY = 0.75;
	first.outX = 0.4;
	first.outY = -0.2;
	TlKeyframe last = first;
	last.tMs = 1250;
	last.tf = {3.0, -2.0, 12.0, 900, 0.9};
	last.handlesManual = false;
	last.scale.on = true;
	last.scale.ease = TlEase::EaseOut;
	last.scale.bez1 = 0.25;
	source.keys = {first, last};
	const auto transform = snapshotPinnedComponent(source, QStringLiteral("harpia.transform"));
	check(transform.typeId == QStringLiteral("harpia.transform") && transform.instanceId.isEmpty() &&
	      transform.props.value(QStringLiteral("posX")).toDouble() == 2.5 && transform.keys.isEmpty(),
	      "Transform snapshots base values rather than evaluated display values and carry no component identity");
	check(transform.unknown.value(QStringLiteral("presetTransformKeys")).toArray().size() == 2 &&
	      !transform.unknown.contains(QStringLiteral("source")), "Transform snapshot carries native pose keys without media references");
	TlClip target;
	target.type = TlClip::Type::Text;
	target.text.text = QStringLiteral("Keep these words");
	target.text.fontPx = 90;
	target.crop = QRect(3, 4, 50, 60);
	target.sourceId = 93;
	target.outStartMs = 8500;
	target.srcStartMs = 80;
	target.srcEndMs = 9000;
	target.speed = 1.25;
	ComponentInstance other;
	other.typeId = QStringLiteral("harpia.shader.crt");
	other.instanceId = QStringLiteral("keep-id");
	target.components = {other};
	const TlClip before = target;
	check(applyPinnedComponentPreset(target, transform), "Transform preset applies to a native clip pose");
	check(target.posX == source.posX && target.posY == source.posY && target.scale == source.scale &&
	      target.rotation == source.rotation && target.opacity == source.opacity,
	      "valid preview poses beyond the Inspector range are preserved exactly");
	check(target.keys == source.keys, "every native keyframe channel, easing, curve and dormant manual handle survives");
	check(target.text.text == before.text.text && target.text.fontPx == before.text.fontPx &&
	      target.crop == before.crop && target.sourceId == before.sourceId && target.outStartMs == before.outStartMs &&
	      target.srcStartMs == before.srcStartMs && target.srcEndMs == before.srcEndMs && target.speed == before.speed &&
	      target.components == before.components, "applying Transform preserves text, crop, media, timing, speed and other components");
	const auto encoded = QJsonDocument(componentToJson(transform, ComponentRegistry::instance())).toJson();
	const auto decoded = componentFromJson(QJsonDocument::fromJson(encoded).object(), ComponentRegistry::instance());
	TlClip reopened;
	check(applyPinnedComponentPreset(reopened, decoded) && reopened.keys == source.keys && reopened.scale == source.scale,
	      "native Transform animation survives component preset JSON serialization");
	TlClip still = target;
	ComponentInstance staticPose = transform;
	staticPose.unknown.remove(QStringLiteral("presetTransformKeys"));
	check(applyPinnedComponentPreset(still, staticPose) && still.keys.isEmpty(), "static Transform preset removes previous pose animation");
	ComponentInstance invalid = transform;
	invalid.unknown.insert(QStringLiteral("presetTransformVersion"), 2);
	TlClip protectedClip = target;
	check(!applyPinnedComponentPreset(protectedClip, invalid) && protectedClip.keys == target.keys &&
	      protectedClip.scale == target.scale, "unknown future native-preset versions leave the clip unchanged");
	invalid = transform;
	invalid.unknown.insert(QStringLiteral("presetTransformKeys"), QJsonArray{QJsonObject{{QStringLiteral("t"), 1e30}}});
	check(!applyPinnedComponentPreset(protectedClip, invalid) && protectedClip.keys == target.keys,
	      "out-of-range keyframe timestamps are rejected before integer conversion");
	invalid.unknown.insert(QStringLiteral("presetTransformKeys"), QJsonArray{QJsonObject{{QStringLiteral("t"), -1}}});
	check(!applyPinnedComponentPreset(protectedClip, invalid), "negative keyframe timestamps are rejected");
	invalid = transform;
	invalid.props[QStringLiteral("posX")] = std::numeric_limits<double>::infinity();
	invalid.props[QStringLiteral("posY")] = QStringLiteral("invalid");
	invalid.props[QStringLiteral("scale")] = -1;
	invalid.props[QStringLiteral("rotation")] = std::numeric_limits<double>::quiet_NaN();
	invalid.props[QStringLiteral("opacity")] = 5;
	check(applyPinnedComponentPreset(protectedClip, invalid) && protectedClip.posX == 0.5 && protectedClip.posY == 0.5 &&
	      protectedClip.scale == 1.0 && protectedClip.rotation == 0.0 && protectedClip.opacity == 1.0,
	      "invalid static transform values have finite safe fallbacks and bounded opacity");
	const auto speed = snapshotPinnedComponent(source, QStringLiteral("harpia.speed"));
	check(speed.props.value(QStringLiteral("factor")).toDouble() == source.speed && speed.unknown.isEmpty(),
	      "Speed snapshot contains only the playback factor");
	TlClip video = source;
	video.speed = 1;
	check(applyPinnedComponentPreset(video, speed) && video.speed == source.speed && video.keys == source.keys,
	      "Speed applies to the native factor without changing Transform animation");
	for (TlClip::Type kind : {TlClip::Type::Text, TlClip::Type::Image, TlClip::Type::Effect}) {
		TlClip free;
		free.type = kind;
		check(!applyPinnedComponentPreset(free, speed) && free.speed == 1.0, "Speed does not retime free-duration clips");
	}
	ComponentInstance speedLimit = speed;
	speedLimit.props[QStringLiteral("factor")] = 200;
	check(applyPinnedComponentPreset(video, speedLimit) && video.speed == 20, "Speed respects the native upper playback factor");
	speedLimit.props[QStringLiteral("factor")] = -20;
	check(applyPinnedComponentPreset(video, speedLimit) && video.speed == 0.1, "Speed respects the native lower playback factor");
	speedLimit.props[QStringLiteral("factor")] = std::numeric_limits<double>::infinity();
	check(applyPinnedComponentPreset(video, speedLimit) && video.speed == 1.0, "invalid playback factors use normal speed");
	const auto unknown = snapshotPinnedComponent(source, QStringLiteral("harpia.blur"));
	check(unknown.typeId.isEmpty() && !applyPinnedComponentPreset(video, other), "ordinary components never enter the pinned preset path");
	return failures ? 1 : 0;
}
