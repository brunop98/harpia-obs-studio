#pragma once

// Components, to and from the project file.
//
// The rule that matters more than the format: a component this build has never
// heard of must survive a load-and-save untouched. Someone opens a project on a
// laptop without a plugin installed, saves it, and sends it back — the plugin's
// settings have to still be there. Premiere and Resolve both drop unknown
// effects on the floor; the cost of not doing that is keeping the raw object
// and writing it back, which is what ComponentInstance::unknown is for.

#include "Component.hpp"
#include "ComponentRegistry.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace harpia {

inline QJsonObject propKeyToJson(const PropKey &k)
{
	QJsonObject o;
	o[QStringLiteral("t")] = double(k.tMs);
	o[QStringLiteral("v")] = k.v;
	o[QStringLiteral("ease")] = int(k.ease);
	if (k.bez1 != 0.42 || k.bez2 != 0.58) {
		o[QStringLiteral("b1")] = k.bez1;
		o[QStringLiteral("b2")] = k.bez2;
	}
	return o;
}

inline PropKey propKeyFromJson(const QJsonObject &o)
{
	PropKey k;
	k.tMs = qint64(o.value(QStringLiteral("t")).toDouble());
	k.v = o.value(QStringLiteral("v")).toDouble();
	k.ease = tlEaseFromInt(o.value(QStringLiteral("ease")).toInt(int(TlEase::EaseInOut)));
	k.bez1 = o.value(QStringLiteral("b1")).toDouble(0.42);
	k.bez2 = o.value(QStringLiteral("b2")).toDouble(0.58);
	return k;
}

inline QJsonObject componentToJson(const ComponentInstance &c, const ComponentRegistry &reg)
{
	// Start from whatever we could not understand, so unknown members survive
	// a round trip. The fields we DO own are written over the top, which means
	// a component that later becomes known cannot end up with two copies of a
	// value disagreeing.
	QJsonObject o = c.unknown;
	o[QStringLiteral("type")] = c.typeId;
	o[QStringLiteral("id")] = c.instanceId;
	if (!c.enabled)
		o[QStringLiteral("enabled")] = false; // omitted when true: the common case
	// Omitted when zero, which is the default and by far the common case --
	// so this feature adds nothing to the size of an existing project file.
	if (c.inMs > 0)
		o[QStringLiteral("inMs")] = double(c.inMs);
	if (c.outMs > 0)
		o[QStringLiteral("outMs")] = double(c.outMs);
	if (const ComponentType *t = reg.find(c.typeId))
		o[QStringLiteral("version")] = t->version;

	QJsonObject props;
	for (auto it = c.props.cbegin(); it != c.props.cend(); ++it) {
		if (it->typeId() == QMetaType::Bool)
			props[it.key()] = it->toBool();
		else if (it->typeId() == QMetaType::Int)
			props[it.key()] = it->toInt();
		else
			props[it.key()] = it->toDouble();
	}
	if (!props.isEmpty())
		o[QStringLiteral("props")] = props;
	else
		o.remove(QStringLiteral("props"));

	QJsonObject keys;
	for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it) {
		if (it->isEmpty())
			continue;
		QJsonArray arr;
		for (const PropKey &k : *it)
			arr.append(propKeyToJson(k));
		keys[it.key()] = arr;
	}
	if (!keys.isEmpty())
		o[QStringLiteral("keys")] = keys;
	else
		o.remove(QStringLiteral("keys"));
	return o;
}

inline ComponentInstance componentFromJson(const QJsonObject &in, const ComponentRegistry &reg)
{
	QJsonObject o = in;

	ComponentInstance c;
	c.typeId = o.value(QStringLiteral("type")).toString();
	c.instanceId = o.value(QStringLiteral("id")).toString();
	c.enabled = o.value(QStringLiteral("enabled")).toBool(true);
	// Absent means 0 means "no envelope": an older project opens rendering
	// exactly as it always did.
	c.inMs = qint64(o.value(QStringLiteral("inMs")).toDouble(0));
	c.outMs = qint64(o.value(QStringLiteral("outMs")).toDouble(0));

	// Give the type a chance to bring an older shape forward before anything is
	// read out of it.
	const ComponentType *t = reg.find(c.typeId);
	const int from = o.value(QStringLiteral("version")).toInt(1);
	if (t && t->migrate && from < t->version)
		t->migrate(from, o);

	const QJsonObject props = o.value(QStringLiteral("props")).toObject();
	for (auto it = props.begin(); it != props.end(); ++it) {
		if (it->isBool())
			c.props.insert(it.key(), it->toBool());
		else
			c.props.insert(it.key(), it->toDouble());
	}
	const QJsonObject keys = o.value(QStringLiteral("keys")).toObject();
	for (auto it = keys.begin(); it != keys.end(); ++it) {
		QVector<PropKey> ks;
		for (const QJsonValue &kv : it->toArray())
			ks.append(propKeyFromJson(kv.toObject()));
		std::sort(ks.begin(), ks.end(),
			  [](const PropKey &a, const PropKey &b) { return a.tMs < b.tMs; });
		if (!ks.isEmpty())
			c.keys.insert(it.key(), ks);
	}

	// Everything we just consumed comes out, so `unknown` holds only what this
	// build genuinely did not understand. Keeping the whole object here instead
	// would mean writing stale copies of props back out beside the live ones.
	for (const char *known : {"type", "id", "enabled", "inMs", "outMs", "version", "props", "keys"})
		o.remove(QLatin1String(known));
	c.unknown = o;
	return c;
}

inline QJsonArray componentsToJson(const QVector<ComponentInstance> &list,
				   const ComponentRegistry &reg)
{
	QJsonArray arr;
	for (const ComponentInstance &c : list)
		arr.append(componentToJson(c, reg));
	return arr;
}

inline QVector<ComponentInstance> componentsFromJson(const QJsonArray &arr,
						     const ComponentRegistry &reg)
{
	QVector<ComponentInstance> out;
	out.reserve(arr.size());
	for (const QJsonValue &v : arr)
		out.append(componentFromJson(v.toObject(), reg));
	return out;
}

} // namespace harpia
