#pragma once

// The Subtitles window: pick a language, tune how words are grouped and how
// the captions look, press Generate, and the selected clips' speech becomes
// caption clips on the timeline.
//
// The dialog owns the whole job -- audio preparation, one request per chunk,
// grouping, clip building -- and hands the finished clips back through
// captionsReady(). The editor window then puts them on a lane as one undo
// step. Splitting it this way keeps every network and file detail out of the
// editor window, which is already the largest file in the tree.

#include "../timeline/TimelineModel.hpp"
#include "AudioForSpeech.hpp"
#include "Transcript.hpp"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace harpia {

class OpenAiTranscriber;

class SubtitleDialog : public QDialog {
	Q_OBJECT
public:
	// One media clip whose speech is wanted, with the file its audio is in.
	struct Target {
		TlClip media;
		QString path;
		QString name; // for the progress line
	};

	explicit SubtitleDialog(QWidget *parent = nullptr);
	~SubtitleDialog() override;

	void setTargets(const QVector<Target> &targets);

	// What the controls say right now (also saved in settings on Generate).
	GroupRule groupRule() const;
	SubtitleLook look() const;
	QString languageCode() const; // "" = detect

	// The languages offered, as (label, ISO code). Exposed for the test.
	static QVector<QPair<QString, QString>> languages();

signals:
	// Caption clips, already aligned to the timeline, for every target in
	// order. Emitted once, after the last chunk of the last target.
	void captionsReady(const QVector<TlClip> &clips, const QString &summary);

private:
	void buildUi();
	void loadSettings();
	void saveSettings();
	void startJob();
	void nextChunk();
	void finishJob(const QString &error);
	void setBusy(bool on);

	QVector<Target> targets_;
	OpenAiTranscriber *transcriber_ = nullptr;

	// Job state.
	int targetIdx_ = -1;
	QVector<SpeechAudioChunk> chunks_;  // the current target's audio pieces
	int chunkIdx_ = 0;
	QVector<ClipWordTime> words_;  // the current target's words, source time
	QVector<TlClip> out_;
	QString workDir_;
	int wordsTotal_ = 0;

	// Controls.
	QComboBox *language_ = nullptr;
	QLineEdit *apiKey_ = nullptr;
	QLabel *keyState_ = nullptr;
	QSpinBox *maxWords_ = nullptr;
	QDoubleSpinBox *maxSeconds_ = nullptr;
	QSpinBox *pauseMs_ = nullptr;
	QSpinBox *maxChars_ = nullptr;
	QComboBox *position_ = nullptr;
	QSpinBox *fontPx_ = nullptr;
	QCheckBox *bold_ = nullptr;
	QCheckBox *box_ = nullptr;
	QComboBox *mode_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *go_ = nullptr;
	QPushButton *cancel_ = nullptr;
};

} // namespace harpia
