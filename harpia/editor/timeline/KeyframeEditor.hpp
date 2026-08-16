#pragma once

// The per-clip keyframe editor: a floating, resizable window with one tab per
// animatable channel (Position / Scale / Rotation / Opacity), each showing its
// own keyframes on its own time axis.
//
// It edits a COPY of the clip and emits clipChanged() after every edit. The
// window owns nothing: closing it throws away no keyframe data, because the
// data was never here — it lives on the clip, and every change has already been
// pushed back through the same path the Inspector uses (so the preview updates
// live and each edit is one undo step).

#include "TimelineModel.hpp"
#include "../component/Component.hpp"

#include <QDialog>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace harpia {

// One channel's keyframe strip: a ruler, the clip's span, and a diamond per key
// that pins this channel. Drag a diamond sideways to retime it, click to select,
// wheel to zoom, middle-drag (or shift-wheel) to pan.
// Runtime-tweakable geometry for the keyframe lanes (Dev-panel tunable, like
// TimelineViewParams next door). These were four constants in the .cpp, which
// meant the one part of the editor whose spacing is hardest to judge from a
// screenshot -- how big a diamond has to be before you can reliably grab it --
// could only be tuned by rebuilding.
struct KeyframeLayoutParams {
	int margin = 8;      // breathing room inside a lane
	int rulerH = 16;     // the per-lane time ruler
	int grab = 7;        // half-width of a key's clickable area
	int diamond = 5;     // half-width of the key marker as drawn
	int laneMinH = 120;  // a lane's minimum height
	double maxZoom = 64.0;
};

class KeyframeLane : public QWidget {
	Q_OBJECT
public:
	explicit KeyframeLane(int lane, QWidget *parent = nullptr);

	// Point this lane at a keyed COMPONENT PROPERTY instead of one of the
	// clip's four transform channels.
	//
	// A component property animates exactly like a transform channel does --
	// times inside the clip, a value, an ease -- it just lives in a different
	// store (ComponentInstance::keys). Rather than a second lane widget with a
	// second copy of the ruler, the zoom, the drag and the diamonds, the lane
	// reads its keys through a handful of accessors that know which store it is
	// looking at. Everything else here is the same code for both.
	//
	// `def` describes the property (label, type, range) so the lane can draw
	// the curve in the right shape -- a Bool steps, a Colour has no curve at
	// all -- and name itself when it is empty.
	void setComponentSource(int componentIndex, const QString &propKey, const PropDef &def);
	bool isComponentLane() const { return compIndex_ >= 0; }
	int componentIndex() const { return compIndex_; }
	QString propKey() const { return propKey_; }
	const PropDef &propDef() const { return def_; }
	// How many keys this lane shows, and where the i-th one is. Public because
	// the dialog's panel, buttons and clipboard all have to speak about the
	// lane's OWN index space, which for a transform lane is the clip's key
	// list and for a component lane is that property's key vector.
	int keyCount() const;
	qint64 keyTimeAt(int i) const;
	// Add / remove a key from the dialog's buttons, in this lane's own index
	// space. The lane owns the arithmetic (including the rule that a new key is
	// seeded from what the property is already doing); the dialog owns the undo
	// step, which is why these do not emit clipEdited themselves.
	int addKeyAtForOwner(qint64 tMs) { return addKeyAt(tMs); }
	void removeKeyAtForOwner(int i) { removeKeyAt(i); }

	void setClip(const TlClip &c);
	const TlClip &clip() const { return clip_; }
	void setPlayhead(qint64 clipMs); // time INSIDE the clip
	qint64 playhead() const { return playheadMs_; }

	int selectedKey() const { return sel_; }   // index into clip().keys, or -1
	void selectKey(int keyIndex);
	void zoomToFit();

	const KeyframeLayoutParams &layoutParams() const { return lp_; }
	void setLayoutParams(const KeyframeLayoutParams &p);

signals:
	void clipEdited();                  // a key moved/added/removed here
	void selectionChanged(int keyIndex);
	void scrubbed(qint64 clipMs);       // the user dragged the playhead

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;
	QSize minimumSizeHint() const override;

private:
	QRect contentRect() const;
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;
	// The size of the store `sel_`/`dragKey_` index into: the clip's whole key
	// list for a transform channel (a key can pin several channels, so the
	// index is a clip index, not a position in this lane), or this property's
	// own key vector.
	int keyStoreSize() const { return isComponentLane() ? keyCount() : clip_.keys.size(); }
	QVector<int> laneKeyIndices() const; // this lane's keys, in time order
	int keyAtX(int x) const; // index into this lane's key store, or -1
	void clampView();

	// Where this lane's keys live. compIndex_ < 0 = a transform channel
	// (lane_), otherwise clip_.components[compIndex_].keys[propKey_].
	int compIndex_ = -1;
	QString propKey_;
	PropDef def_;
	const QVector<PropKey> *propKeys() const;
	QVector<PropKey> *propKeys();
	double keyValueAt(int i) const;  // for the curve
	void removeKeyAt(int i);
	int addKeyAt(qint64 tMs);        // returns the new key's index
	QString channelName() const;     // "Opacity", "Blur · Radius"

	int lane_ = 0;
	TlClip clip_;
	KeyframeLayoutParams lp_;
	qint64 playheadMs_ = 0;
	int sel_ = -1;

	// Horizontal zoom/pan over the clip's own 0..duration span.
	double zoom_ = 1.0;
	qint64 viewStart_ = 0;

	enum class Drag { None, Key, Scrub, Pan };
	Drag drag_ = Drag::None;
	int dragKey_ = -1;
	qint64 dragGrabMs_ = 0;
	QPoint pressPos_;
	qint64 panStart_ = 0;
	bool moved_ = false;
};

class KeyframeEditor : public QDialog {
	Q_OBJECT
public:
	explicit KeyframeEditor(QWidget *parent = nullptr);

	// Point the editor at a clip. `label` names it in the title bar.
	void setClip(const TlClip &c, const QString &label);
	const TlClip &clip() const { return clip_; }
	// Follow the main timeline's playhead (given as an output-time position).
	void setPlayheadOut(qint64 outMs);

	// Dev-panel geometry, pushed down to every lane. One set for the dialog:
	// four lanes that measured their diamonds differently would be a bug, not
	// a feature.
	void setLayoutParams(const KeyframeLayoutParams &p);

	// The tab strip, for tests: which channels and properties this clip offers
	// is the whole claim, and a test that rebuilt the expected list itself
	// would be checking its own arithmetic rather than the dialog's.
	int tabCountForTest() const;
	QString tabTextForTest(int i) const;
	KeyframeLane *laneForTest(int i) const
	{
		return (i >= 0 && i < lanes_.size()) ? lanes_[i] : nullptr;
	}
	void showTabForTest(int i);

signals:
	// The clip was edited here: the window should push it back onto the
	// timeline, refresh the preview and record one undo step.
	void clipChanged(const TlClip &c);
	// The user scrubbed inside the editor; `outMs` is an output-time position.
	void scrubRequested(qint64 outMs);

private:
	KeyframeLayoutParams lp_;
	// One tab per transform channel, plus one per keyed component property.
	// Rebuilt when the clip changes, because which properties are keyed is a
	// property OF the clip -- a tab for something with no keys would be an
	// empty lane you cannot add to (the component owns its key list), and a
	// missing tab for something keyed is the bug this fixes.
	void rebuildTabs();
	void wireLane(KeyframeLane *lane);
	// Which (component, property) pairs have tabs right now, so rebuildTabs can
	// tell "nothing changed" from "a property was keyed or un-keyed" and leave
	// the current tab alone in the common case.
	QVector<QPair<int, QString>> compTabs_;
	void rebuildLanes();
	void syncKeyPanel();       // fill the property panel from the selection
	void syncCompPanel(KeyframeLane *lw); // ...when the tab is a component property
	PropKey *selectedCompKey();           // the component key being edited, or null
	void pushEdit();           // emit clipChanged + refresh the lanes
	int currentLane() const;
	KeyframeLane *currentLaneWidget() const;
	void addKeyHere();
	void deleteSelected();
	void copySelected();
	void pasteHere();

	TlClip clip_;
	QTabWidget *tabs_ = nullptr;
	QVector<KeyframeLane *> lanes_;
	bool syncing_ = false;

	// Property panel for the selected key.
	QFormLayout *form_ = nullptr; // rows are hidden whole, label included
	QLabel *keyInfo_ = nullptr;
	QSpinBox *timeSpin_ = nullptr;
	QVector<QDoubleSpinBox *> valueSpins_; // per lane, in lane order (pos = 2)
	QComboBox *easeCombo_ = nullptr;
	QDoubleSpinBox *bez1_ = nullptr;
	QDoubleSpinBox *bez2_ = nullptr;
	QWidget *bezRow_ = nullptr;
	QWidget *posRow_ = nullptr;
	QWidget *valueRow_ = nullptr;
	QLabel *valueLabel_ = nullptr;
	QPushButton *delBtn_ = nullptr;
	QPushButton *copyBtn_ = nullptr;
	QPushButton *pasteBtn_ = nullptr;

	// One key on the clipboard, plus which lane it came from.
	bool haveClip_ = false;
	TlKeyframe clipboard_;
	int clipboardLane_ = 0;
	// A copied COMPONENT key, tagged with the property it came from. Paste is
	// offered only into a lane for that same property: the value in a PropKey
	// is a bare double whose meaning is the property's (a radius, a flag, a
	// packed colour), so pasting a colour into a radius would type-check and be
	// nonsense.
	bool haveCompClip_ = false;
	PropKey compClipboard_;
	QString compClipboardKey_;

	// The component-property value editors. One row, three widgets, one shown:
	// a number, a checkbox or a colour field, per the property's type -- the
	// same three shapes the Inspector offers for the same properties.
	QWidget *compRow_ = nullptr;
	QLabel *compLabel_ = nullptr;
	QDoubleSpinBox *compSpin_ = nullptr;
	QCheckBox *compCheck_ = nullptr;
	QPushButton *compSwatch_ = nullptr;
	void applyCompValue(double v);
};

} // namespace harpia
