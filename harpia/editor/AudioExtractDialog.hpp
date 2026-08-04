#pragma once

#include "AudioExtract.hpp"

#include <QDialog>
#include <QVector>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace harpia {

class AudioPreview;

// The waveform strip: peaks, two drag handles for the trim, a playhead, and
// shading over the parts that will be discarded. Its own widget so the maths
// that maps milliseconds to pixels lives in one place rather than being
// scattered through the dialog's paint and mouse handlers.
class AudioWaveform : public QWidget {
	Q_OBJECT
public:
	explicit AudioWaveform(QWidget *parent = nullptr);

	void setPeaks(const QVector<float> &peaks, qint64 durationMs);
	void setTrim(qint64 startMs, qint64 endMs);
	void setPlayhead(qint64 ms); // < 0 hides it
	qint64 trimStartMs() const { return startMs_; }
	qint64 trimEndMs() const { return endMs_; }

	// Pixel <-> time, exposed so a test can drive a drag without a mouse.
	qint64 msAtX(int x) const;
	int xForMs(qint64 ms) const;

signals:
	void trimChanged(qint64 startMs, qint64 endMs);
	void scrubbed(qint64 ms); // clicked outside a handle: move the playhead

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;

private:
	enum class Grab { None, Start, End };
	Grab grabAt(int x) const;

	QVector<float> peaks_;
	qint64 durationMs_ = 0;
	qint64 startMs_ = 0;
	qint64 endMs_ = 0;
	qint64 playMs_ = -1;
	Grab dragging_ = Grab::None;
	Grab hover_ = Grab::None;
};

// "Extract Audio Only": pull the soundtrack out of a recording, trim it, tidy
// it up and write it somewhere.
//
// Deliberately not the video editor. This opens on one file, does four things
// to it -- trim, fade, level, speed -- and exports. Anything more belongs in
// Full Editing, which already exists.
//
// The audio is decoded once, up front, into a float buffer that both the
// waveform and the preview player share. That costs a moment on a long
// recording and buys instant scrubbing, instant re-trimming and a preview that
// is exactly what will be exported, because it is the same samples through the
// same edit chain.
class AudioExtractDialog : public QDialog {
	Q_OBJECT
public:
	// Returns false (with the reason shown) when the file has no audio track,
	// so the caller can avoid opening an empty window.
	static bool fileHasAudio(const QString &path, QString *why);

	explicit AudioExtractDialog(const QString &sourcePath, QWidget *parent = nullptr);
	~AudioExtractDialog() override;

	// The file that was written, or empty if the user closed without exporting.
	QString exportedPath() const { return exportedPath_; }

private slots:
	void onPlayPause();
	void onExport();
	void onNormalise();

private:
	AudioEdit currentEdit() const;
	void refreshSummary();      // duration, size estimate, button states
	void rebuildPreviewBuffer(); // re-apply the edit chain for playback

	QString sourcePath_;
	QString exportedPath_;
	std::vector<float> pcm_; // decoded once: the source of truth for everything
	int rate_ = 48000;
	int channels_ = 2;
	qint64 durationMs_ = 0;

	AudioWaveform *wave_ = nullptr;
	AudioPreview *preview_ = nullptr;
	QTimer *playTimer_ = nullptr;
	QPushButton *playBtn_ = nullptr;
	QPushButton *exportBtn_ = nullptr;
	QSpinBox *fadeInSpin_ = nullptr;
	QSpinBox *fadeOutSpin_ = nullptr;
	QSlider *gainSlider_ = nullptr;
	QLabel *gainLabel_ = nullptr;
	QDoubleSpinBox *speedSpin_ = nullptr;
	QComboBox *formatCombo_ = nullptr;
	QComboBox *qualityCombo_ = nullptr;
	QLabel *summary_ = nullptr;
	// The preview buffer lags the controls by a beat: rebuilding it on every
	// slider tick would stutter a long recording. Coalesced through this.
	QTimer *rebuildTimer_ = nullptr;
};

} // namespace harpia
