#pragma once

// The Text to speech window: paste a text (or open a .txt), pick the service,
// the language and the voice, and Generate. The text becomes a narration and
// captions that say it word by word as it is spoken; the editor puts them on
// the timeline linked, so they move as one.
//
// The making itself is a TtsEngine (pauses, pronunciation, the requests, the
// word times, Fit to selection); this window is its controls. ▶ plays a
// sample of the chosen voice first. How the captions are grouped and look is
// shared with the Subtitles window (the same saved settings).

#include "../subtitles/Transcript.hpp"
#include "TtsProviders.hpp"

#include <QDialog>
#include <QHash>
#include <QStringList>
#include <QVector>

#include <vector>

class QCheckBox;
class QComboBox;
class QDateTime;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace harpia {

class AudioPreview;
class TtsEngine;
class TtsSynthesizer;
struct TtsResult;

class TtsDialog : public QDialog {
	Q_OBJECT
public:
	explicit TtsDialog(QWidget *parent = nullptr);
	~TtsDialog() override;

	void setText(const QString &text);
	QString text() const;
	TtsProvider provider() const;
	QString languageCode() const;
	QString voice() const;
	GroupRule groupRule() const;
	SubtitleLook look() const;
	TtsSpec spec() const;

	// What is selected on the timeline, for Fit to selection (the editor keeps
	// it current); toMs <= fromMs: nothing.
	void setFitTarget(qint64 fromMs, qint64 toMs);

	// Where finished narrations are kept: <Movies>/Harpia/Speech (beside the
	// recordings and the projects), and the name one gets from its text.
	static QString speechFolder();
	static QString fileNameFor(const QString &text, const QDateTime &when);
	// A text file's words: plain text as it is; an .srt without its numbers
	// and times. Exposed for the test.
	static QString readTextFile(const QString &path, QString *err);
	// What ▶ says: the text's first sentence (short), or a line in the language.
	static QString previewSentence(const QString &text, const QString &language);

signals:
	// The narration (a WAV in speechFolder()), its length, every word of the
	// text with the time it is said from the start of that WAV, how it was
	// made (TtsSpec JSON, for re-speaking), and where it goes: atMs, or the
	// playhead when atMs < 0.
	void speechReady(const QString &wavPath, qint64 durationMs, const QVector<ClipWordTime> &words,
			 const GroupRule &rule, const SubtitleLook &look, const QString &specJson, qint64 atMs);

private:
	void buildUi();
	void loadSettings();
	void saveSettings();
	void refreshProvider();
	void refreshCount();
	void refreshFit();
	void refreshPronunciationButton();
	void openTextFile();
	void editPronunciations();
	void loadVoices();
	void playPreview();
	void playPcm(const std::vector<float> &pcm);
	void setVoices(const QVector<TtsVoice> &voices, const QString &select);
	QString currentKey() const;
	bool storeTypedKey(TtsProvider p);
	void setBusy(bool on);
	void startJob();
	void onFinished(const TtsResult &r);

	TtsEngine *engine_ = nullptr;
	TtsSynthesizer *voices_ = nullptr;  // Load my voices
	TtsSynthesizer *sampler_ = nullptr; // ▶
	AudioPreview *player_ = nullptr;
	QHash<QString, std::vector<float>> samples_; // ▶ heard before: not paid for twice
	QString sampleKey_;
	qint64 fitFrom_ = -1, fitTo_ = -1;
	qint64 jobAtMs_ = -1;
	qint64 jobFitMs_ = 0;

	// Controls.
	QPlainTextEdit *text_ = nullptr;
	QLabel *count_ = nullptr;
	QComboBox *provider_ = nullptr;
	TtsProvider keyFor_ = TtsProvider::OpenAI;
	QLabel *about_ = nullptr;
	QComboBox *language_ = nullptr;
	QComboBox *voice_ = nullptr;
	QPushButton *preview_ = nullptr;
	QPushButton *loadVoices_ = nullptr;
	QLineEdit *style_ = nullptr;
	QLabel *styleLabel_ = nullptr;
	QDoubleSpinBox *speed_ = nullptr;
	QLabel *speedLabel_ = nullptr;
	QPushButton *pronounce_ = nullptr;
	QLineEdit *apiKey_ = nullptr;
	QLabel *getKey_ = nullptr;
	QLabel *keyState_ = nullptr;
	QCheckBox *fit_ = nullptr;
	QSpinBox *maxWords_ = nullptr;
	QComboBox *mode_ = nullptr;
	QComboBox *position_ = nullptr;
	QSpinBox *fontPx_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *go_ = nullptr;
	QPushButton *cancel_ = nullptr;
};

} // namespace harpia
