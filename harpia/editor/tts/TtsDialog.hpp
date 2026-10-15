#pragma once

// The Text to speech window: paste a text (or open a .txt), pick the service,
// the language and the voice, and Generate. The text becomes a narration and
// captions that say it word by word as it is spoken; the editor puts them on
// the timeline linked, so they move as one.
//
// The dialog owns the job -- cutting the text into pieces the service
// accepts, one request per piece, transcribing the audio back where the
// service gives no word times, joining the audio -- and hands back one WAV and
// the words' times through speechReady(). How the captions are grouped and
// look is shared with the Subtitles window (the same saved settings), so both
// make captions alike.

#include "../subtitles/Transcript.hpp"
#include "TtsProviders.hpp"

#include <QDialog>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace harpia {

class SpeechTranscriber;
class TtsSynthesizer;
struct Transcript;

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

	// Where finished narrations are kept: <Movies>/Harpia/Speech (beside the
	// recordings and the projects), and the name one gets from its text.
	static QString speechFolder();
	static QString fileNameFor(const QString &text, const QDateTime &when);
	// A text file's words: plain text as it is; an .srt without its numbers
	// and times. Exposed for the test.
	static QString readTextFile(const QString &path, QString *err);

signals:
	// The narration (a WAV in speechFolder()), its length, and every word of
	// the text with the time it is said, from the start of that WAV.
	void speechReady(const QString &wavPath, qint64 durationMs, const QVector<ClipWordTime> &words,
			 const GroupRule &rule, const SubtitleLook &look);

private:
	void buildUi();
	void loadSettings();
	void saveSettings();
	void refreshProvider();
	void refreshCount();
	void openTextFile();
	void loadVoices();
	void setVoices(const QVector<TtsVoice> &voices, const QString &select);
	QString currentKey() const;
	bool storeTypedKey(TtsProvider p);
	void setBusy(bool on);

	void startJob();
	void nextPiece();
	void onSynthesized(const TtsReply &reply);
	void onTranscribed(const Transcript &t);
	void pieceDone(const QVector<ClipWordTime> &words);
	void assemble();
	void finishJob(const QString &error);

	TtsSynthesizer *synth_ = nullptr;
	SpeechTranscriber *transcriber_ = nullptr;

	// Job state.
	QStringList pieces_;
	int pieceIdx_ = 0;
	QString workDir_;
	QVector<QString> pieceWavs_;                 // each piece, decoded to 48 kHz stereo
	QVector<QVector<ClipWordTime>> pieceWords_;  // each piece's words, from its own start
	qint64 pieceMs_ = 0;                         // the current piece's length
	bool running_ = false;

	// Controls.
	QPlainTextEdit *text_ = nullptr;
	QLabel *count_ = nullptr;
	QComboBox *provider_ = nullptr;
	TtsProvider keyFor_ = TtsProvider::OpenAI;
	QLabel *about_ = nullptr;
	QComboBox *language_ = nullptr;
	QComboBox *voice_ = nullptr;
	QPushButton *loadVoices_ = nullptr;
	QLineEdit *style_ = nullptr;
	QLabel *styleLabel_ = nullptr;
	QDoubleSpinBox *speed_ = nullptr;
	QLabel *speedLabel_ = nullptr;
	QLineEdit *apiKey_ = nullptr;
	QLabel *getKey_ = nullptr;
	QLabel *keyState_ = nullptr;
	QSpinBox *maxWords_ = nullptr;
	QComboBox *mode_ = nullptr;
	QComboBox *position_ = nullptr;
	QSpinBox *fontPx_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *go_ = nullptr;
	QPushButton *cancel_ = nullptr;
};

} // namespace harpia
