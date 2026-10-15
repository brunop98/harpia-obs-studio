#pragma once

// Making a narration, start to finish, without any window: the Text to speech
// window runs one for a whole text, the editor runs one for a single sentence
// it is re-speaking.
//
//   1. The text is split at its pauses ([pause 1s], blank lines), each part
//      cut at sentences into pieces the service accepts, and each piece
//      turned into what is SAID (pronunciation rules) -- the captions keep
//      what is written.
//   2. One request per piece; its audio decoded to the editor's WAV.
//   3. Word times from the service, or (OpenAI, Deepgram) by transcribing the
//      audio back with the same key.
//   4. The pieces joined with their pauses; for Fit to selection the whole is
//      stretched (pitch kept) to the asked length.

#include "../subtitles/Transcript.hpp"
#include "TtsProviders.hpp"
#include "TtsText.hpp"

#include <QObject>
#include <QVector>

namespace harpia {

class SpeechTranscriber;
class TtsSynthesizer;
struct Transcript;

struct TtsJob {
	QString text;   // as written: [pause] markers and blank lines allowed
	TtsSpec spec;   // spec.tempo is applied as it is, unless fitToMs is set
	QString apiKey;
	QVector<PronunciationRule> rules;
	qint64 fitToMs = 0; // > 0: stretch the result to this length
	QString outPath;    // where the narration's WAV goes
};

struct TtsResult {
	QString wavPath;
	qint64 durationMs = 0;
	QVector<ClipWordTime> words; // every written word, from the start of the WAV
	TtsSpec spec;                // with the tempo that was used
};

class TtsEngine : public QObject {
	Q_OBJECT
public:
	explicit TtsEngine(QObject *parent = nullptr);
	~TtsEngine() override;

	void start(const TtsJob &job);
	void cancel();
	bool running() const { return running_; }

	// The keys: OpenAI's, ElevenLabs' and Deepgram's are the same account keys
	// the Subtitles window keeps; Google's is its own. Sealed (SecretStore).
	static QString loadKey(TtsProvider p);
	static bool saveKey(TtsProvider p, const QString &key);

	// The pronunciation rules, as typed (settings), and parsed.
	static QString pronunciationText();
	static void setPronunciationText(const QString &t);
	static QVector<PronunciationRule> pronunciations();

	// How captions are grouped: the Subtitles window's saved settings, which
	// the Text to speech window shares (and re-speaking a sentence uses).
	static GroupRule captionRule();

	// The stretch that makes `naturalMs` last `targetMs`, kept to what still
	// sounds like speech.
	static constexpr double kMinTempo = 0.6, kMaxTempo = 1.8;
	static double fitTempo(qint64 naturalMs, qint64 targetMs);

	// The silence between two pieces of one part of the text (a sentence end).
	static constexpr qint64 kPieceGapMs = 250;

signals:
	void progress(const QString &what);
	void finished(const harpia::TtsResult &result);
	void failed(const QString &why);

private:
	struct Piece {
		SpokenText st;
		qint64 gapBeforeMs = 0;
	};
	void nextPiece();
	void onSynthesized(const TtsReply &reply);
	void onTranscribed(const Transcript &t);
	void pieceDone(const QVector<ClipWordTime> &spokenTimes);
	void assemble();
	void fail(const QString &why);
	void cleanUp();

	TtsSynthesizer *synth_ = nullptr;
	SpeechTranscriber *transcriber_ = nullptr;
	TtsJob job_;
	QVector<Piece> pieces_;
	int idx_ = 0;
	QString workDir_;
	QVector<QString> wavs_;
	QVector<QVector<ClipWordTime>> words_; // written words, per piece
	qint64 pieceMs_ = 0;
	bool running_ = false;
};

} // namespace harpia
