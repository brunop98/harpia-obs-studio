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

#include <QDialog>
#include <QVector>
#include <QWidget>

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
class KeyframeLane : public QWidget {
	Q_OBJECT
public:
	explicit KeyframeLane(int lane, QWidget *parent = nullptr);

	void setClip(const TlClip &c);
	const TlClip &clip() const { return clip_; }
	void setPlayhead(qint64 clipMs); // time INSIDE the clip
	qint64 playhead() const { return playheadMs_; }

	int selectedKey() const { return sel_; }   // index into clip().keys, or -1
	void selectKey(int keyIndex);
	void zoomToFit();

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
	int keyAtX(int x) const; // index into keys_, or -1
	void clampView();

	int lane_ = 0;
	TlClip clip_;
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

signals:
	// The clip was edited here: the window should push it back onto the
	// timeline, refresh the preview and record one undo step.
	void clipChanged(const TlClip &c);
	// The user scrubbed inside the editor; `outMs` is an output-time position.
	void scrubRequested(qint64 outMs);

private:
	void rebuildLanes();
	void syncKeyPanel();       // fill the property panel from the selection
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
};

} // namespace harpia
