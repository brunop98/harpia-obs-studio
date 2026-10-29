#pragma once

// Slip, in a window of its own: one clip, its own preview, and a track bar of
// its whole source sliding under a fixed window -- the clip's place and length
// on the timeline stay as they are, and only the moment of footage it shows
// changes. Like CapCut's slip.
//
//   * The preview shows only this clip, alone, with its look (crop, colour,
//     effect components) but without its pose or fades, so the moment is what
//     you see -- never the rest of the timeline.
//   * Drag the strip: the footage follows the pointer (drag left, later
//     footage). The wheel and the arrow keys nudge it (a frame; Shift, a
//     second); Home and End go to either end of the source.
//   * Play loops the chosen moment with its own sound.
//   * OK applies it, as one undo step; Cancel or Esc leaves the clip alone.
//
// The window only CHOOSES the slip (deltaMs); the editor applies it, to the
// clip and the sound linked to it (TimelineView::applySlip).

#include "timeline/TimelineModel.hpp"

#include <QDialog>
#include <QImage>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <vector>

class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QPainter;

namespace harpia {

class SlipTrackBar;
class SlipPreview;

class SlipDialog : public QDialog {
public:
	struct Input {
		TlClip clip;              // as it is on the timeline now
		QVector<TlClip> partners; // linked, same file (its detached sound): heard with it
		bool soundOnly = false;   // a clip on an audio lane: no picture
		QString title;            // what the source is called
		QString decodePath;       // the file to read pictures from (the proxy, where there is one)
		QSize sourceSize;         // the ORIGINAL's pixel size (the crop is in its pixels)
		qint64 sourceMs = 0;      // the source's length; 0 = unknown
		qint64 loMs = 0;          // how far the footage can move: lo <= 0 <= hi
		qint64 hiMs = 0;
		QVector<QImage> thumbs;   // the source's filmstrip, start to end
		double fps = 30.0;        // the project's
		// The sound of a timeline as one stereo 48 kHz float buffer (empty =
		// silent). Called off the GUI thread, with a model the window builds:
		// this clip and its partners, from 0, for one pass of the moment.
		std::function<std::vector<float>(const TimelineModel &)> mixAudio;
	};

	explicit SlipDialog(const Input &in, QWidget *parent = nullptr);
	~SlipDialog() override;

	// The slip chosen: how far the footage moved, in source milliseconds.
	qint64 deltaMs() const { return delta_; }
	void setDeltaMs(qint64 d); // clamped to the range

	// For tests.
	SlipTrackBar *trackBar() const { return bar_; }
	SlipTrackBar *overviewBar() const { return overview_; }
	QImage previewImage() const;
	bool isPlaying() const { return playing_; }
	void setPlaying(bool on);
	qint64 playheadMs() const { return playMs_; } // into the moment, output time
	bool audioReady() const { return audioReady_; }
	QString readout() const;
	// A model for the moment as the window renders it (this clip alone, from 0).
	TimelineModel momentModel(bool withPartners) const;

	// Both stop playback first.
	void accept() override;
	void reject() override;

protected:
	void keyPressEvent(QKeyEvent *e) override;

private:
	void render(qint64 outMs);
	void tick();
	void restartAudio();
	void syncReadout();
	void stopAll();
	TlClip slipped(const TlClip &c) const;

	Input in_;
	qint64 delta_ = 0;
	qint64 outDurMs_ = 1;
	bool playing_ = false;
	qint64 playMs_ = 0;
	bool audioReady_ = false;
	int audioGen_ = 0;

	struct Impl;
	std::unique_ptr<Impl> d_;

	SlipPreview *preview_ = nullptr;
	SlipTrackBar *bar_ = nullptr;
	SlipTrackBar *overview_ = nullptr;
	QLabel *readout_ = nullptr;
	QDoubleSpinBox *sourceStart_ = nullptr;
	QPushButton *playBtn_ = nullptr;
};

// The whole source as a filmstrip (a waveform, for sound) under a fixed
// window the clip's length. Dragging the strip slips the clip.
class SlipTrackBar : public QWidget {
public:
	explicit SlipTrackBar(QWidget *parent = nullptr);

	void setSource(const QVector<QImage> &thumbs, const QVector<float> &peaks, qint64 sourceMs);
	void setWindow(qint64 baseStartMs, qint64 lenMs, qint64 loMs, qint64 hiMs);
	void setDelta(qint64 d);
	void setOverview(bool on);
	qint64 delta() const { return delta_; }
	// Where playback is within the window, 0..1; < 0 hides the playhead.
	void setPlayFraction(double f);

	// The window and how many pixels a millisecond of source is, as drawn.
	QRect windowRect() const;
	double pxPerMs() const;

	// Every change of the slip; `dragging` while the pointer is down.
	std::function<void(qint64 delta, bool dragging)> changed;

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	QSize sizeHint() const override { return QSize(640, 84); }

private:
	qint64 clampDelta(qint64 d) const;
	qint64 overviewDelta(double x) const;
	void paintOverview(QPainter &p);
	double xOf(qint64 srcMs) const; // a source time, on screen

	QVector<QImage> thumbs_;
	QVector<float> peaks_;
	qint64 sourceMs_ = 0;
	qint64 baseStart_ = 0, len_ = 1, lo_ = 0, hi_ = 0;
	qint64 delta_ = 0;
	double playFrac_ = -1.0;
	bool dragging_ = false;
	bool overview_ = false;
	int pressX_ = 0;
	qint64 pressDelta_ = 0;
};

} // namespace harpia
