#pragma once

// Every keyboard shortcut in the editor, in one place.
//
// Shortcuts used to be a scatter of `new QShortcut(...)` calls, which meant a
// binding existed only as a line of code: nothing could list them, nothing
// could rebind them, and nothing could tell you two of them collided.
//
// A command is registered once, with an id, a label, a category, its default
// keys and what it does. The registry owns the QShortcut objects and rebuilds
// them whenever the bindings change, so a new binding takes effect immediately
// with no restart. Bindings persist in QSettings, and export/import moves a
// whole profile between machines as JSON.
//
// Some things are bound to the mouse rather than the keyboard (Ctrl+Wheel to
// zoom). Those are registered as DOCUMENTATION-ONLY commands: they appear in
// the list so the panel is a complete reference, but they carry no editable
// key, because the widget that handles them reads the mouse directly.

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QShortcut;
class QWidget;

namespace harpia {

struct ShortcutCommand {
	QString id;       // stable key for settings; never shown
	QString label;    // "Split clip"
	QString category; // "Timeline", "Playback", …
	QVector<QKeySequence> defaults;
	std::function<void()> run;
	// A gesture the mouse owns (Ctrl+Wheel). Listed for reference, not editable.
	bool mouseOnly = false;
	QString mouseText; // "Ctrl + Mouse wheel"
};

class ShortcutRegistry : public QObject {
	Q_OBJECT
public:
	// `host` is the widget the QShortcut objects live on; they use
	// Qt::WindowShortcut, so they fire wherever focus is inside that window.
	explicit ShortcutRegistry(QWidget *host, QObject *parent = nullptr);

	void addCommand(const ShortcutCommand &c);
	// Register a mouse gesture for the reference list.
	void addMouseGesture(const QString &id, const QString &label, const QString &category,
			     const QString &gesture);

	const QVector<ShortcutCommand> &commands() const { return cmds_; }
	QStringList categories() const; // sorted, with "All" NOT included

	QVector<QKeySequence> bindings(const QString &id) const;
	QVector<QKeySequence> defaults(const QString &id) const;
	const ShortcutCommand *command(const QString &id) const;

	// Which command currently owns `k`, or "" if it is free. `exceptId` lets a
	// command keep its own key without reporting a clash with itself.
	QString conflict(const QKeySequence &k, const QString &exceptId = {}) const;

	// Set the whole list for one command. Empty removes every binding.
	void setBindings(const QString &id, const QVector<QKeySequence> &keys);
	void addBinding(const QString &id, const QKeySequence &k);
	void removeBinding(const QString &id, const QKeySequence &k);
	void resetToDefault(const QString &id);
	void resetAllToDefaults();
	// Ids whose bindings differ from their defaults — the panel highlights them.
	bool isModified(const QString &id) const;

	// Human text for a command's keys, e.g. "Ctrl+Z, Ctrl+Shift+Z". A mouse
	// gesture returns its own text.
	QString displayText(const QString &id) const;

	// A whole profile as JSON, for sharing between machines.
	QByteArray exportProfile() const;
	bool importProfile(const QByteArray &json, QString *err = nullptr);

	void load(); // from QSettings
	void save() const;

signals:
	// Any binding changed: menus, tooltips and the panel refresh from this.
	void bindingsChanged();

private:
	void rebuild(); // recreate the QShortcut objects from the current bindings
	// Remove any key a previously-registered command already claims.
	void dropDuplicateBindings();

	QWidget *host_ = nullptr;
	QVector<ShortcutCommand> cmds_;
	QHash<QString, int> index_;                    // id -> position in cmds_
	QHash<QString, QVector<QKeySequence>> binds_;  // id -> current keys
	QVector<QShortcut *> live_;                    // owned by host_, cleared on rebuild
};

} // namespace harpia
