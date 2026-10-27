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

#include <algorithm>

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

inline QVector<PropKey> propKeysFromJson(const QJsonArray &array)
{
	QVector<PropKey> keys;
	for (const QJsonValue &value : array)
		if (value.isObject())
			keys.append(propKeyFromJson(value.toObject()));
	std::stable_sort(keys.begin(), keys.end(),
			 [](const PropKey &a, const PropKey &b) { return a.tMs < b.tMs; });
	return keys;
}

// Retain a missing component's extra key fields when a common numeric value
// is edited. Match unchanged keys first, then the same timestamp; a moved key
// can keep its extra fields by index when the key count has not changed.
inline QJsonArray propKeysToJson(const QVector<PropKey> &keys, const QJsonArray &original = {})
{
	QVector<int> match(keys.size(), -1);
	QVector<bool> used(original.size(), false);
	for (int pass = 0; pass < 2; ++pass)
		for (int i = 0; i < keys.size(); ++i) {
			if (match[i] >= 0)
				continue;
			for (int j = 0; j < original.size(); ++j) {
				if (used[j] || !original[j].isObject())
					continue;
				const PropKey old = propKeyFromJson(original[j].toObject());
				if (pass == 0 ? old == keys[i] : old.tMs == keys[i].tMs) {
					match[i] = j;
					used[j] = true;
					break;
				}
			}
		}
	if (keys.size() == original.size())
		for (int i = 0; i < keys.size(); ++i)
			if (match[i] < 0 && !used[i] && original[i].isObject()) {
				match[i] = i;
				used[i] = true;
			}
	QJsonArray array;
	for (int i = 0; i < keys.size(); ++i) {
		QJsonObject object = match[i] >= 0 ? original[match[i]].toObject() : QJsonObject();
		if (match[i] < 0 || !(propKeyFromJson(object) == keys[i])) {
			for (const char *field : {"t", "v", "ease", "b1", "b2"})
				object.remove(QLatin1String(field));
			const QJsonObject known = propKeyToJson(keys[i]);
			for (auto it = known.begin(); it != known.end(); ++it)
				object.insert(it.key(), it.value());
		}
		array.append(object);
	}
	return array;
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
	// A component loaded while unavailable still carries its original schema.
	// If the plugin appears later, keep that version until a known load can
	// migrate its payload rather than claiming it was already migrated.
	if (const ComponentType *t = reg.find(c.typeId); t && !o.contains(QStringLiteral("version")))
		o[QStringLiteral("version")] = t->version;

	QJsonObject props;
	for (auto it = c.props.cbegin(); it != c.props.cend(); ++it)
		props[it.key()] = QJsonValue::fromVariant(it.value());
	if (!props.isEmpty())
		o[QStringLiteral("props")] = props;
	else
		o.remove(QStringLiteral("props"));

	const QJsonValue rawKeys = o.value(QStringLiteral("keys"));
	QJsonObject keys = rawKeys.toObject();
	for (auto it = keys.begin(); it != keys.end();) {
		const QVector<PropKey> live = c.keys.value(it.key());
		if (live == propKeysFromJson(it.value().toArray())) {
			++it; // unchanged: even a future key shape survives verbatim
		} else if (live.isEmpty()) {
			it = keys.erase(it); // an explicit key deletion must stay deleted
		} else {
			it.value() = propKeysToJson(live, it.value().toArray());
			++it;
		}
	}
	for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it) {
		if (it->isEmpty())
			continue;
		if (!keys.contains(it.key()))
			keys[it.key()] = propKeysToJson(*it);
	}
	if (!keys.isEmpty() || (rawKeys.isObject() && rawKeys.toObject().isEmpty()))
		o[QStringLiteral("keys")] = keys;
	else if (rawKeys.isUndefined() || rawKeys.isObject())
		o.remove(QStringLiteral("keys"));
	return o;
}

inline ComponentInstance componentFromJson(const QJsonObject &in, const ComponentRegistry &reg)
{
	QJsonObject o = in;

	// Give the type a chance to bring an older shape forward before anything is
	// read out of it.
	const ComponentType *t = reg.find(o.value(QStringLiteral("type")).toString());
	const int from = o.value(QStringLiteral("version")).toInt(1);
	if (t && t->migrate && from < t->version)
		t->migrate(from, o);

	ComponentInstance c;
	c.typeId = o.value(QStringLiteral("type")).toString();
	c.instanceId = o.value(QStringLiteral("id")).toString();
	c.enabled = o.value(QStringLiteral("enabled")).toBool(true);
	// Absent means 0 means "no envelope": an older project opens rendering
	// exactly as it always did.
	c.inMs = qint64(o.value(QStringLiteral("inMs")).toDouble(0));
	c.outMs = qint64(o.value(QStringLiteral("outMs")).toDouble(0));

	const QJsonObject props = o.value(QStringLiteral("props")).toObject();
	for (auto it = props.begin(); it != props.end(); ++it)
		c.props.insert(it.key(), it->toVariant());
	const QJsonObject keys = o.value(QStringLiteral("keys")).toObject();
	for (auto it = keys.begin(); it != keys.end(); ++it) {
		const QVector<PropKey> ks = propKeysFromJson(it->toArray());
		if (!ks.isEmpty())
			c.keys.insert(it.key(), ks);
	}

	// Everything we just consumed comes out, so `unknown` holds only what this
	// build genuinely did not understand. Keeping the whole object here instead
	// would mean writing stale copies of props back out beside the live ones.
	for (const char *known : {"type", "id", "enabled", "inMs", "outMs", "props"})
		o.remove(QLatin1String(known));
	// A missing plugin owns its schema version. Keep it with the unknown
	// fields so saving here cannot make that plugin apply the wrong migration
	// when the project is reopened on a machine that has it installed.
	if (t) {
		o.remove(QStringLiteral("version"));
		o.remove(QStringLiteral("keys"));
	} else if (!keys.isEmpty()) {
		// Canonical keys made by this build need no second copy. This also
		// keeps an in-memory missing component equal after its first save.
		QJsonObject canonical;
		for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it)
			canonical[it.key()] = propKeysToJson(*it);
		if (o.value(QStringLiteral("keys")) == canonical)
			o.remove(QStringLiteral("keys"));
	}
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
