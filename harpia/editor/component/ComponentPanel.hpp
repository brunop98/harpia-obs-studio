#pragma once

// The Inspector's component list — the Unity shape, for one clip or for many.
//
// One ordered list of components, each a header row (enable, name, stage, move,
// remove) over its properties, and Add Component at the bottom. A property's
// control is built from its PropDef, so a component declares a property once
// and gets a slider, a keyframe track and a serialised field without anyone
// writing a third thing.
//
// MULTI-SELECT IS NOT A SEPARATE MODE. With several clips selected the panel
// shows the components they ALL have, and a property whose value differs
// between them reads as an em dash rather than as one clip's number pretending
// to speak for the rest. Editing it gives every selected clip that value.
// Because one clip is simply the case where N is 1, there is no single-clip
// path to keep in step with a multi-clip one — which is the bug that shape
// invites.
//
// Edits leave here as OPERATIONS ("set this property", "remove this component")
// rather than as a finished list, for the same reason: with five clips selected
// there is no one list to hand back. The window applies each operation to every
// selected clip, in a single undo step.
//
// It owns no clip. It is handed what to show and reports what was asked for;
// which clips those are, and how an edit becomes an undo entry, is the window's
// business. That keeps the panel testable without the editor, which matters
// because everything else in this Inspector needs libobs to exist.
//
// Rebuild vs refresh is deliberate. Rebuilding the widgets on every keystroke
// would take the focus out from under whoever is typing, so the panel only
// rebuilds when the SHAPE changes — which components are shared, which
// properties are mixed — and otherwise pushes values into the controls that are
// already there.

#include "../timeline/TlTransform.hpp"
#include "Component.hpp"

#include <QMap>
#include <QVariant>
#include <QVector>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace harpia {

class ComponentRegistry;

class ComponentPanel : public QWidget {
	Q_OBJECT
public:
	explicit ComponentPanel(const ComponentRegistry &reg, QWidget *parent = nullptr);

	// One value across the whole selection: either everyone agrees, or they do
	// not. Deliberately a QVariant rather than a double — every property type
	// the system grows gets mixed-value handling out of this without another
	// line of code, which is the only way "and any future type" can be true
	// rather than aspirational.
	struct Mixed {
		QVariant value; // meaningless when `mixed`
		bool mixed = false;
	};

	// A property every clip has by BEING a clip: its pose, its rate. Pinned
	// above the list, not removable — a clip without a position is not a thing.
	struct PinnedRow {
		QString typeId; // the registered type whose PropDefs to render from
		QMap<QString, Mixed> values;
		QStringList driven; // props a transform script is computing
		QString drivenTip;
	};

	// One component the whole selection shares. `ordinal` tells the second Blur
	// from the first when a clip carries two.
	struct SharedComponent {
		QString typeId;
		int ordinal = 0;
		Mixed enabled; // mixed when some of them have it switched off
		// In / Out ramp times, ms. Mixed when the selection disagrees. Both 0
		// (the default) means the component is simply on for the whole clip.
		Mixed inMs;
		Mixed outMs;
		QMap<QString, Mixed> values;
		QStringList keyedHere; // props with a key at the playhead on every clip
	};

	// Everything the panel draws, handed over whole so it can decide in one
	// place whether the shape changed and a rebuild is needed.
	struct View {
		int clipCount = 0;
		QVector<PinnedRow> pinned;
		QVector<SharedComponent> shared;
		QStringList notShared; // on some of the selection but not all
		QStringList warnings;
		QStringList loadErrors;
	};
	void setView(const View &v);

signals:
	// Every one of these means "do this to every selected clip".
	void propertyEdited(const QString &typeId, int ordinal, const QString &key,
			    const QVariant &value);
	void pinnedEdited(const QString &typeId, const QString &key, double value);
	void componentAdded(const QString &typeId);
	void componentRemoved(const QString &typeId, int ordinal);
	void componentEnableChanged(const QString &typeId, int ordinal, bool on);
	// An In or Out ramp time was edited. -1 for the one that did not change,
	// so a single signal serves both boxes without the panel having to know
	// the other's current value across a mixed selection.
	void componentTimingChanged(const QString &typeId, int ordinal, qint64 inMs, qint64 outMs);
	void componentMoved(const QString &typeId, int ordinal, int delta);
	void componentReset(const QString &typeId, int ordinal);
	void componentDuplicated(const QString &typeId, int ordinal);
	void componentCopied(const QString &typeId, int ordinal);
	void componentPasted(const QString &typeId, int ordinal);
	void keyframeToggled(const QString &typeId, int ordinal, const QString &key);
	// One of the component's own buttons. The panel does not know what it does;
	// the window looks the action up on the type and runs it on every selected
	// clip, as one undo step, like any other edit from here.
	void actionInvoked(const QString &typeId, int ordinal, const QString &actionId);

public:
	// Which clip kinds the selection is made of (a bitmask of ClipKind). The
	// Add Component menu offers only what applies to them -- a Typing effect is
	// meaningless on a video, and finding that out by adding it and watching
	// nothing happen is not a discovery worth making.
	void setAllowedKinds(unsigned kinds);

	// Show values as a READ-ONLY look at another instant -- the frame under the
	// pointer while the timeline is hovered -- rather than as the clip's own
	// editable state.
	//
	// Read-only because the value under the cursor is about to be replaced by
	// the next mouse-move: a field you can type into while its content is being
	// rewritten thirty times a second is a field that will eat an edit. Dimmed
	// and italic because "0.42" that came from interpolating two keys is not
	// the same fact as "0.42" that is stored on the clip, and the panel would
	// otherwise present them identically.
	void setPreviewing(bool on);
	bool previewing() const { return previewing_; }

private:
	struct Row;
	void applyPreviewStyle();
	bool previewing_ = false;
	unsigned allowedKinds_ = ClipKindAll;

	void rebuild();
	void pushValues();
	bool shapeChanged(const View &next) const;
	void addComponentMenu();
	void componentMenu(const QString &typeId, int ordinal);
	// `pinnedIndex >= 0` marks a pinned row: no enable box, no arrows, no
	// remove, and no keyframe diamonds.
	Row *makeRow(const QString &typeId, int ordinal, const QMap<QString, Mixed> &values,
		     const Mixed *enabled, const QStringList &driven, const QString &drivenTip,
		     const QStringList &keyedHere, bool pinned, bool canUp, bool canDown,
		     const Mixed *inMs = nullptr, const Mixed *outMs = nullptr);

	const ComponentRegistry &reg_;
	View view_;
	QVector<Row *> rows_; // pinned first, then shared, in draw order
	QVBoxLayout *listLayout_ = nullptr;
	QLabel *header_ = nullptr;
	QLabel *empty_ = nullptr;
	QLabel *warnings_ = nullptr;
	QLabel *loadErrors_ = nullptr;
	QLabel *notShared_ = nullptr;
	bool syncing_ = false;
};

} // namespace harpia
