#pragma once

// The Inspector's component list — the Unity shape, for a clip.
//
// One ordered list of components, each a header row (enable, name, stage, move,
// remove) over its properties, and Add Component at the bottom. A property's
// control is built from its PropDef, so a component declares a property once
// and gets a slider, a keyframe track and a serialised field without anyone
// writing a third thing.
//
// It owns no clip. It is handed a list and a clip time, and reports edits back;
// what the list belongs to and how the edit becomes an undo step is the
// window's business. That keeps the panel testable without the editor, which
// matters because everything else in this Inspector needs libobs to exist.
//
// Rebuild vs refresh is deliberate. Rebuilding the widgets on every keystroke
// would take the focus out from under whoever is typing, so the panel only
// rebuilds when the list's SHAPE changes (different components, or a different
// order) and otherwise just pushes values into the controls that are already
// there.

#include "../timeline/TlTransform.hpp"
#include "Component.hpp"

#include <QMap>
#include <QVector>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace harpia {

class ComponentRegistry;

class ComponentPanel : public QWidget {
	Q_OBJECT
public:
	// `reg` must outlive the panel.
	explicit ComponentPanel(const ComponentRegistry &reg, QWidget *parent = nullptr);

	// Show `list`, with property values resolved at `clipTimeMs` so an animated
	// property reads honestly rather than showing its resting value.
	void setComponents(const QVector<ComponentInstance> &list, qint64 clipTimeMs);

	// Errors from loading the user's components folder, shown above the list.
	void setLoadErrors(const QStringList &errors);

	// A clip's ESSENTIAL properties: the ones it has by being a clip rather than
	// by having something added to it. Its pose, and how fast it plays. They are
	// shown as pinned rows above the list, with no enable box, no move arrows
	// and no remove button — Unity's Transform, which works the same way and for
	// the same reason. Offering to delete a clip's position would be offering
	// nonsense.
	//
	// Their values live in the clip's own fields, not in a ComponentInstance.
	// The rows are still rendered from the registered type's PropDefs, so one
	// declaration of the labels and ranges serves the pinned rows and the
	// ordinary ones alike.
	struct PinnedRow {
		QString typeId;               // the registered type whose props to show
		QMap<QString, double> values; // by property key
		QStringList driven;           // props a script is computing
		QString drivenTip;
	};
	void setPinned(const QVector<PinnedRow> &rows);

signals:
	// The list changed and should be written back to the clip.
	void componentsEdited(const QVector<ComponentInstance> &list);
	// A pinned property changed. Separate from componentsEdited because these do
	// not live in the list; the window knows which clip field each one is.
	void pinnedEdited(const QString &typeId, const QString &key, double value);

private:
	void rebuild();
	void buildPinnedRows();
	void pushPinnedValues();
	void pushValues();
	void emitEdit();
	// True when the widgets on screen no longer match `list_`'s shape.
	bool shapeChanged(const QVector<ComponentInstance> &next) const;
	void addComponentMenu();

	struct Row; // one component's widgets

	const ComponentRegistry &reg_;
	QVector<ComponentInstance> list_;
	qint64 timeMs_ = 0;
	QVector<Row *> rows_;
	QVector<Row *> pinnedRows_;
	QVector<PinnedRow> pinned_;
	QVBoxLayout *pinnedLayout_ = nullptr;
	QVBoxLayout *listLayout_ = nullptr;
	QLabel *empty_ = nullptr;
	QLabel *warnings_ = nullptr;
	QLabel *loadErrors_ = nullptr;
	bool syncing_ = false;
};

} // namespace harpia
