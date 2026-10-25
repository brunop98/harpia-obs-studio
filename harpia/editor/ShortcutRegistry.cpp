#include "../core/RecorderSettings.hpp"
#include "ShortcutRegistry.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QWidget>
#include <QVariant>

#include <algorithm>

namespace harpia {

namespace {
QSettings shortcutSettings()
{
	return harpia::recorderSettings();
}
constexpr const char *kGroup = "editorShortcuts";
} // namespace

ShortcutRegistry::ShortcutRegistry(QWidget *host, QObject *parent) : QObject(parent), host_(host)
{
	connect(this, &ShortcutRegistry::bindingsChanged, this, &ShortcutRegistry::refreshBoundHints);
}

void ShortcutRegistry::addCommand(const ShortcutCommand &c)
{
	if (c.id.isEmpty() || index_.contains(c.id))
		return; // ids are unique by construction; a duplicate is a bug, not data
	index_.insert(c.id, cmds_.size());
	cmds_.append(c);
	if (!binds_.contains(c.id))
		binds_.insert(c.id, c.defaults);
}

void ShortcutRegistry::addMouseGesture(const QString &id, const QString &label,
				       const QString &category, const QString &gesture)
{
	ShortcutCommand c;
	c.id = id;
	c.label = label;
	c.category = category;
	c.mouseOnly = true;
	c.mouseText = gesture;
	addCommand(c);
}

QStringList ShortcutRegistry::categories() const
{
	QStringList out;
	for (const ShortcutCommand &c : cmds_)
		if (!out.contains(c.category))
			out << c.category;
	std::sort(out.begin(), out.end());
	return out;
}

const ShortcutCommand *ShortcutRegistry::command(const QString &id) const
{
	const auto it = index_.constFind(id);
	return it == index_.constEnd() ? nullptr : &cmds_[it.value()];
}

QVector<QKeySequence> ShortcutRegistry::bindings(const QString &id) const
{
	return binds_.value(id);
}

QVector<QKeySequence> ShortcutRegistry::defaults(const QString &id) const
{
	const ShortcutCommand *c = command(id);
	return c ? c->defaults : QVector<QKeySequence>();
}

QString ShortcutRegistry::conflict(const QKeySequence &k, const QString &exceptId) const
{
	if (k.isEmpty())
		return {};
	for (auto it = binds_.constBegin(); it != binds_.constEnd(); ++it) {
		if (it.key() == exceptId)
			continue;
		for (const QKeySequence &existing : it.value())
			if (existing == k)
				return it.key();
	}
	return {};
}

bool ShortcutRegistry::isModified(const QString &id) const
{
	return bindings(id) != defaults(id);
}

void ShortcutRegistry::setBindings(const QString &id, const QVector<QKeySequence> &keys)
{
	if (!index_.contains(id))
		return;
	const ShortcutCommand *c = command(id);
	if (c && c->mouseOnly)
		return; // a mouse gesture has no key to set
	// Drop empties and duplicates: an empty binding would swallow every key,
	// and a duplicate would fire the command twice.
	QVector<QKeySequence> clean;
	for (const QKeySequence &k : keys)
		if (!k.isEmpty() && !clean.contains(k))
			clean.append(k);
	if (binds_.value(id) == clean)
		return;
	binds_[id] = clean;
	rebuild();
	save();
	emit bindingsChanged();
}

void ShortcutRegistry::addBinding(const QString &id, const QKeySequence &k)
{
	QVector<QKeySequence> v = bindings(id);
	v.append(k);
	setBindings(id, v);
}

void ShortcutRegistry::removeBinding(const QString &id, const QKeySequence &k)
{
	QVector<QKeySequence> v = bindings(id);
	v.removeAll(k);
	setBindings(id, v);
}

void ShortcutRegistry::resetToDefault(const QString &id)
{
	setBindings(id, defaults(id));
}

void ShortcutRegistry::resetAllToDefaults()
{
	for (const ShortcutCommand &c : cmds_)
		binds_[c.id] = c.defaults;
	rebuild();
	save();
	emit bindingsChanged();
}

QString ShortcutRegistry::displayText(const QString &id) const
{
	const ShortcutCommand *c = command(id);
	if (c && c->mouseOnly)
		return c->mouseText;
	QStringList parts;
	for (const QKeySequence &k : bindings(id))
		parts << k.toString(QKeySequence::NativeText);
	return parts.join(QStringLiteral(", "));
}

QString ShortcutRegistry::actionText(const QString &id, const QString &label) const
{
	const ShortcutCommand *c = command(id);
	const QString name = !label.isEmpty() ? label : c ? c->label : id;
	const QString keys = displayText(id);
	return keys.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, keys);
}

QString ShortcutRegistry::formatHint(const QString &pattern) const
{
	static const QRegularExpression token(QStringLiteral("\\{([^{}]+)\\}"));
	QString out;
	qsizetype offset = 0;
	auto matches = token.globalMatch(pattern);
	while (matches.hasNext()) {
		const auto match = matches.next();
		out += pattern.mid(offset, match.capturedStart() - offset);
		const QString id = match.captured(1);
		out += command(id) ? actionText(id) : match.captured();
		offset = match.capturedEnd();
	}
	return out + pattern.mid(offset);
}

void ShortcutRegistry::bindHint(QObject *target, const char *property, const QString &pattern)
{
	if (!target || !property)
		return;
	hints_.erase(std::remove_if(hints_.begin(), hints_.end(), [](const BoundHint &hint) {
		return hint.target.isNull();
	}), hints_.end());
	for (BoundHint &hint : hints_) {
		if (hint.target == target && hint.property == property) {
			hint.pattern = pattern;
			hint.rendered = formatHint(pattern);
			target->setProperty(property, hint.rendered);
			return;
		}
	}
	const QString rendered = formatHint(pattern);
	target->setProperty(property, rendered);
	hints_.append({target, QByteArray(property), pattern, rendered});
}

void ShortcutRegistry::refreshBoundHints()
{
	for (auto it = hints_.begin(); it != hints_.end();) {
		if (!it->target || it->target->property(it->property.constData()).toString() != it->rendered) {
			it = hints_.erase(it);
			continue;
		}
		it->rendered = formatHint(it->pattern);
		it->target->setProperty(it->property.constData(), it->rendered);
		++it;
	}
}

ShortcutRegistry *ShortcutRegistry::forWidget(const QWidget *widget)
{
	for (auto *current = widget; current; current = current->parentWidget())
		if (auto *registry = current->findChild<ShortcutRegistry *>(QString(), Qt::FindDirectChildrenOnly))
			return registry;
	return nullptr;
}

QString ShortcutRegistry::modifierText(Qt::KeyboardModifiers modifiers)
{
	QString text = QKeySequence(int(modifiers)).toString(QKeySequence::NativeText);
	// Qt renders a modifier-only sequence with a trailing '+' on platforms
	// that use textual modifier names; macOS uses its native modifier glyphs.
	while (text.endsWith(QLatin1Char('+')))
		text.chop(1);
	return text;
}

QString ShortcutRegistry::mouseGesture(Qt::KeyboardModifiers modifiers, const QString &gesture)
{
	const QString keys = modifierText(modifiers);
	return keys.isEmpty() ? gesture : QStringLiteral("%1 + %2").arg(keys, gesture);
}

void ShortcutRegistry::rebuild()
{
	// Delete and recreate rather than mutate: a QShortcut's key can be changed
	// in place, but the COUNT per command varies, and stale objects would keep
	// firing the old binding.
	for (QShortcut *s : live_)
		delete s;
	live_.clear();
	liveCmd_.clear();
	if (!host_)
		return;
	// At most one live shortcut per key. Two QShortcuts on the same key in the
	// same window is "ambiguous" to Qt, and an ambiguous shortcut fires
	// NEITHER command -- so a single duplicate silently kills two features. The
	// panel refuses to create one, but an imported profile or a hand-edited
	// settings file can, and this is the one place every route passes through.
	QSet<QKeySequence> taken;
	for (int ci = 0; ci < cmds_.size(); ++ci) {
		const ShortcutCommand &c = cmds_[ci];
		if (c.mouseOnly || !c.run)
			continue;
		for (const QKeySequence &k : binds_.value(c.id)) {
			if (k.isEmpty() || taken.contains(k))
				continue;
			taken.insert(k);
			auto *s = new QShortcut(k, host_);
			s->setContext(Qt::WindowShortcut);
			const std::function<void()> fn = c.run;
			QObject::connect(s, &QShortcut::activated, host_, [fn]() { fn(); });
			live_.append(s);
			liveCmd_.append(ci);
		}
	}
	refreshEnabled();
}

void ShortcutRegistry::refreshEnabled()
{
	for (int i = 0; i < live_.size(); ++i) {
		const ShortcutCommand &c = cmds_[liveCmd_[i]];
		live_[i]->setEnabled(!c.when || c.when());
	}
}

// Strip any key already claimed by an earlier command, so what the panel shows
// is what actually works. Registration order decides the winner, which is also
// the order the panel lists them in.
//
// Done on the way IN rather than only in rebuild(): dropping the shortcut but
// leaving the binding would show a command as bound to a key that does nothing.
void ShortcutRegistry::dropDuplicateBindings()
{
	QSet<QKeySequence> taken;
	for (const ShortcutCommand &c : cmds_) {
		if (c.mouseOnly)
			continue;
		QVector<QKeySequence> keep;
		for (const QKeySequence &k : binds_.value(c.id)) {
			if (k.isEmpty() || taken.contains(k))
				continue;
			taken.insert(k);
			keep.append(k);
		}
		binds_[c.id] = keep;
	}
}

void ShortcutRegistry::load()
{
	QSettings s = shortcutSettings();
	s.beginGroup(QLatin1String(kGroup));
	for (const ShortcutCommand &c : cmds_) {
		if (c.mouseOnly)
			continue;
		if (!s.contains(c.id)) {
			binds_[c.id] = c.defaults;
			continue;
		}
		QVector<QKeySequence> v;
		for (const QString &str : s.value(c.id).toStringList()) {
			const QKeySequence k(str, QKeySequence::PortableText);
			if (!k.isEmpty())
				v.append(k);
		}
		// A stored EMPTY list is meaningful: the user unbound the command.
		binds_[c.id] = v;
	}
	s.endGroup();
	// Settings are a file too, and a build that renamed a command can leave two
	// of them holding the same key.
	dropDuplicateBindings();
	rebuild();
	emit bindingsChanged();
}

void ShortcutRegistry::save() const
{
	QSettings s = shortcutSettings();
	s.beginGroup(QLatin1String(kGroup));
	for (const ShortcutCommand &c : cmds_) {
		if (c.mouseOnly)
			continue;
		// Only store what differs from the default, so a later change to a
		// shipped default reaches anyone who never touched that command.
		if (binds_.value(c.id) == c.defaults) {
			s.remove(c.id);
			continue;
		}
		QStringList out;
		for (const QKeySequence &k : binds_.value(c.id))
			out << k.toString(QKeySequence::PortableText);
		s.setValue(c.id, out);
	}
	s.endGroup();
}

QByteArray ShortcutRegistry::exportProfile() const
{
	QJsonObject root;
	root[QStringLiteral("format")] = QStringLiteral("harpia-shortcuts");
	root[QStringLiteral("version")] = 1;
	QJsonObject binds;
	for (const ShortcutCommand &c : cmds_) {
		if (c.mouseOnly)
			continue;
		QJsonArray arr;
		for (const QKeySequence &k : binds_.value(c.id))
			arr.append(k.toString(QKeySequence::PortableText));
		binds[c.id] = arr;
	}
	root[QStringLiteral("bindings")] = binds;
	return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool ShortcutRegistry::importProfile(const QByteArray &json, QString *err)
{
	QJsonParseError pe{};
	const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
	if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
		if (err)
			*err = QStringLiteral("Not a readable shortcut file (%1).").arg(pe.errorString());
		return false;
	}
	const QJsonObject root = doc.object();
	if (root.value(QStringLiteral("format")).toString() != QLatin1String("harpia-shortcuts")) {
		if (err)
			*err = QStringLiteral("That file is not a Harpia shortcut profile.");
		return false;
	}
	const QJsonObject binds = root.value(QStringLiteral("bindings")).toObject();
	// Commands the file does not mention keep what they have, so an older
	// profile does not silently unbind anything added since.
	for (auto it = binds.constBegin(); it != binds.constEnd(); ++it) {
		if (!index_.contains(it.key()))
			continue; // a command this build does not have
		QVector<QKeySequence> v;
		for (const QJsonValue &kv : it.value().toArray()) {
			const QKeySequence k(kv.toString(), QKeySequence::PortableText);
			if (!k.isEmpty() && !v.contains(k))
				v.append(k);
		}
		binds_[it.key()] = v;
	}
	// A profile is a file: nothing stops it naming the same key twice.
	dropDuplicateBindings();
	rebuild();
	save();
	emit bindingsChanged();
	return true;
}

} // namespace harpia
