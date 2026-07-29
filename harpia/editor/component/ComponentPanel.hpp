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

	// The clip's own pose, shown as a pinned first row that cannot be removed or
	// reordered — Unity's Transform, which works the same way and for the same
	// reason. Its values still live in the clip's fields rather than in a
	// ComponentInstance; the row renders from the registered harpia.transform
	// type's PropDefs so it is built by the same code as every other row.
	// `drivenKeys` marks properties a transform script is computing, whose boxes
	// are therefore a starting point rather than the framing on screen.
	void setPinnedTransform(const TlTransform &xf, bool present, const QStringList &drivenKeys,
				const QString &drivenTip);

signals:
	// The list changed and should be written back to the clip.
	void componentsEdited(const QVector<ComponentInstance> &list);
	// The pinned pose changed. Separate because it does not live in the list.
	void transformEdited(const TlTransform &xf);

private:
	void rebuild();
	void buildPinnedRow();
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
	Row *pinned_ = nullptr; // the clip's pose; null when nothing is selected
	TlTransform pinnedXf_;
	bool pinnedPresent_ = false;
	QStringList drivenKeys_;
	QString drivenTip_;
	QVBoxLayout *pinnedLayout_ = nullptr;
	QVBoxLayout *listLayout_ = nullptr;
	QLabel *empty_ = nullptr;
	QLabel *warnings_ = nullptr;
	QLabel *loadErrors_ = nullptr;
	bool syncing_ = false;
};

} // namespace harpia
