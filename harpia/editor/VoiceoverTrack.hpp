#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

namespace harpia {

// Runtime-tweakable layout metrics for the Voiceover track (edited live from the
// editor's Developer Panel). Defaults match the values used when first built.
struct VoiceoverLayoutParams {
	int margin = 8;
	int captionH = 16;
	int trackH = 40;   // clip-strip height
	int minClipW = 6;  // narrowest a clip can draw
	int edgeZone = 7;  // px near a clip edge that starts a trim
};

// One narration take (recorded or imported), positioned on the output timeline.
// A clip can reference a sub-range of its source file (trimming/splitting):
// srcStartMs..srcStartMs+durationMs within a source of srcTotalMs.
struct VoiceoverClip {
	QString path;          // WAV on disk
	qint64 outStartMs = 0; // where it starts on the output timeline
	qint64 durationMs = 0; // played length (may be < the source after trimming)
	qint64 srcStartMs = 0; // offset into the source where playback begins
	qint64 srcTotalMs = 0; // full length of the source file
	double volume = 1.0;   // linear gain applied at export (0..2)
	int fadeInMs = 15;     // short default fades avoid clicks
	int fadeOutMs = 15;
	QVector<float> peaks;  // |amplitude| per bucket over the WHOLE source, 0..1

	// Compares the editable fields only (ignores the derived waveform peaks),
	// for undo/redo change detection.
	bool operator==(const VoiceoverClip &o) const
	{
		return path == o.path && outStartMs == o.outStartMs && durationMs == o.durationMs &&
		       srcStartMs == o.srcStartMs && volume == o.volume && fadeInMs == o.fadeInMs &&
		       fadeOutMs == o.fadeOutMs;
	}
};

// The Voiceover track shown under the editor's timelines: recorded narration
// clips laid out along the OUTPUT timeline. Clips can be selected, dragged to
// reposition, and deleted. The waveform of each take is drawn inside its block.
// (Trim handles and per-clip volume/fade UI arrive with the export stage.)
class VoiceoverTrack : public QWidget {
	Q_OBJECT
public:
	explicit VoiceoverTrack(QWidget *parent = nullptr);

	// Total output-timeline length the track maps across.
	void setOutputDuration(qint64 ms);

	// Add a take (computes its waveform peaks from the WAV if not supplied).
	// Returns the new clip's index.
	int addClip(VoiceoverClip clip);

	const QVector<VoiceoverClip> &clips() const { return clips_; }
	// Replace the whole clip list (undo/redo restore). Does NOT emit clipsChanged.
	void setClips(const QVector<VoiceoverClip> &clips);
	bool isEmpty() const { return clips_.isEmpty(); }
	int selectedIndex() const { return selected_; }
	void removeSelected();
	void clearAll();

	// Full length (ms) of a source WAV, from its header. 0 on failure.
	static qint64 wavDurationMs(const QString &path);

	void setPlayhead(qint64 outMs); // output-time marker during playback
	void clearPlayhead();

	// Developer Panel: tweak the layout live.
	const VoiceoverLayoutParams &layoutParams() const { return lp_; }
	void setLayoutParams(const VoiceoverLayoutParams &p);

	// Decode a 16-bit PCM WAV into `buckets` normalized peaks (0..1). Empty on
	// failure. Static so the recorder side can precompute off the GUI thread.
	static QVector<float> loadPeaks(const QString &path, int buckets);

signals:
	void clipsChanged();           // added / moved / removed
	void clipSelected(int index);  // -1 = none

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void keyPressEvent(QKeyEvent *) override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

private:
	QRect trackRect() const;
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;
	QVector<QRect> clipRects() const;
	int clipAt(const QPoint &p) const;
	void splitClip(int index, qint64 outMs); // split at an output-time position
	void showClipMenu(int index, const QPoint &globalPos, qint64 outMs);

	VoiceoverLayoutParams lp_;
	QVector<VoiceoverClip> clips_;
	qint64 outputMs_ = 0;
	qint64 playheadMs_ = -1;
	int selected_ = -1;

	enum class Mode { None, Moving, ResizingLeft, ResizingRight };
	Mode mode_ = Mode::None;
	QPoint pressPos_;
	qint64 dragOrigStart_ = 0;    // outStartMs at press
	qint64 dragOrigSrcStart_ = 0; // srcStartMs at press
	qint64 dragOrigDuration_ = 0; // durationMs at press
	bool dragMoved_ = false;
};

} // namespace harpia
