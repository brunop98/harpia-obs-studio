#include "ComponentRegistry.hpp"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace harpia {

ComponentRegistry &ComponentRegistry::instance()
{
	static ComponentRegistry r;
	return r;
}

int ComponentRegistry::indexOf(const QString &id) const
{
	for (int i = 0; i < types_.size(); ++i)
		if (types_[i].id == id)
			return i;
	return -1;
}

bool ComponentRegistry::add(const ComponentType &type)
{
	if (type.id.isEmpty() || !type.make)
		return false;
	const int i = indexOf(type.id);
	if (i >= 0)
		types_[i] = type; // re-registering is how hot reload lands
	else
		types_.append(type);
	return true;
}

void ComponentRegistry::remove(const QString &id)
{
	const int i = indexOf(id);
	if (i >= 0)
		types_.remove(i);
}

const ComponentType *ComponentRegistry::find(const QString &id) const
{
	const int i = indexOf(id);
	return i >= 0 ? &types_[i] : nullptr;
}

QVector<ComponentType> ComponentRegistry::all() const
{
	QVector<ComponentType> out = types_;
	std::sort(out.begin(), out.end(), [](const ComponentType &a, const ComponentType &b) {
		if (a.category != b.category)
			return a.category < b.category;
		return a.displayName < b.displayName;
	});
	return out;
}

QStringList ComponentRegistry::categories() const
{
	QStringList out;
	for (const ComponentType &t : types_)
		if (!out.contains(t.category))
			out.append(t.category);
	std::sort(out.begin(), out.end());
	return out;
}

std::unique_ptr<IComponent> ComponentRegistry::make(const QString &typeId) const
{
	const ComponentType *t = find(typeId);
	return t && t->make ? t->make() : nullptr;
}

void ComponentRegistry::clear()
{
	types_.clear();
}

QVector<int> resolveOrder(const QVector<ComponentInstance> &list, const ComponentRegistry &reg,
			  QStringList *warnings)
{
	// Bucket by stage first. An unknown type has no stage to sort into; it is
	// not going to run either way, so it keeps its place and is reported.
	QVector<QVector<int>> byStage(kStageCount);
	QVector<int> unknown;
	for (int i = 0; i < list.size(); ++i) {
		const ComponentType *t = reg.find(list[i].typeId);
		if (!t) {
			unknown.append(i);
			if (warnings)
				warnings->append(
					QStringLiteral("Missing component \"%1\" — its settings are "
						       "preserved but it will not render.")
						.arg(list[i].typeId));
			continue;
		}
		byStage[int(t->stage)].append(i);
	}

	QVector<int> out;
	out.reserve(list.size());

	for (int s = 0; s < kStageCount; ++s) {
		const QVector<int> &bucket = byStage[s];
		if (bucket.isEmpty())
			continue;

		// Which of THIS stage's members provide each type id. A requirement on
		// something in an earlier stage is already satisfied by the stage order;
		// one on a later stage is unsatisfiable and says so.
		QHash<QString, int> providedHere;
		for (int idx : bucket)
			providedHere.insert(list[idx].typeId, idx);

		// Kahn's algorithm, but taking ready nodes in the user's order so a list
		// with no dependencies at all comes out exactly as it was arranged.
		QHash<int, int> pending;   // index -> unmet requirements
		QHash<int, QVector<int>> unblocks; // index -> who is waiting on it
		for (int idx : bucket) {
			const ComponentType *t = reg.find(list[idx].typeId);
			int n = 0;
			for (const QString &need : t->requiresIds) {
				const auto it = providedHere.find(need);
				if (it == providedHere.end())
					continue; // earlier stage, or absent -- checked below
				if (*it == idx)
					continue; // requiring your own type is a no-op
				++n;
				unblocks[*it].append(idx);
			}
			pending.insert(idx, n);
		}

		QVector<int> ready;
		for (int idx : bucket)
			if (pending.value(idx) == 0)
				ready.append(idx);

		QVector<int> done;
		while (!ready.isEmpty()) {
			const int idx = ready.takeFirst();
			done.append(idx);
			for (int waiter : unblocks.value(idx))
				if (--pending[waiter] == 0) {
					// Re-insert keeping the user's order among the newly
					// freed, so the result is stable run to run.
					int at = 0;
					while (at < ready.size() && ready[at] < waiter)
						++at;
					ready.insert(at, waiter);
				}
		}

		if (done.size() != bucket.size()) {
			// A cycle. Everything still runs — refusing to render because two
			// plugins disagree is worse than rendering in list order and
			// saying so.
			for (int idx : bucket)
				if (!done.contains(idx)) {
					done.append(idx);
					if (warnings)
						warnings->append(
							QStringLiteral("Circular requirement involving "
								       "\"%1\" — running in list order.")
								.arg(list[idx].typeId));
				}
		}
		out += done;
	}

	// Unknown types keep a place in the list so the Inspector can show them.
	out += unknown;
	return out;
}

QStringList findConflicts(const QVector<ComponentInstance> &list, const ComponentRegistry &reg)
{
	QSet<QString> present;
	for (const ComponentInstance &c : list)
		if (c.enabled)
			present.insert(c.typeId);

	QStringList out;
	for (const ComponentInstance &c : list) {
		if (!c.enabled)
			continue;
		const ComponentType *t = reg.find(c.typeId);
		if (!t)
			continue;
		for (const QString &bad : t->conflictsIds)
			if (present.contains(bad)) {
				const QString msg =
					QStringLiteral("\"%1\" and \"%2\" do not work together.")
						.arg(t->displayName, bad);
				if (!out.contains(msg))
					out.append(msg);
			}
	}
	return out;
}

bool allPure(const QVector<ComponentInstance> &list, const ComponentRegistry &reg)
{
	for (const ComponentInstance &c : list) {
		if (!c.enabled)
			continue;
		const ComponentType *t = reg.find(c.typeId);
		// An unknown component does not render, so it cannot make the clip
		// impure. It is the missing plugin's absence, not its behaviour.
		if (t && !t->pure)
			return false;
	}
	return true;
}

} // namespace harpia
