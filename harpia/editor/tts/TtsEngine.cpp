#include "../../core/RecorderSettings.hpp"
#include "TtsEngine.hpp"

#include "TtsAudio.hpp"
#include "TtsSynthesizer.hpp"

#include "../../ui/EditorLog.hpp"
#include "../VoiceoverMixer.hpp"
#include "../subtitles/AudioForSpeech.hpp"
#include "../subtitles/SecretStore.hpp"
#include "../subtitles/SpeechTranscriber.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace harpia {

namespace {
const QString kWhere = QStringLiteral("Text to speech");
const QString kGoogleKey = QStringLiteral("tts/apiKey_google");

QSettings settings()
{
	return harpia::recorderSettings();
}

// The services that give audio only are timed by transcribing it back.
SpeechProvider transcriberFor(TtsProvider p)
{
	switch (p) {
	case TtsProvider::Deepgram: return SpeechProvider::Deepgram;
	case TtsProvider::ElevenLabs: return SpeechProvider::ElevenLabs; // a model without character times
	default: return SpeechProvider::OpenAI;
	}
}
} // namespace

QString TtsEngine::loadKey(TtsProvider p)
{
	switch (p) {
	case TtsProvider::OpenAI: return SecretStore::loadApiKey(SpeechProvider::OpenAI);
	case TtsProvider::ElevenLabs: return SecretStore::loadApiKey(SpeechProvider::ElevenLabs);
	case TtsProvider::Deepgram: return SecretStore::loadApiKey(SpeechProvider::Deepgram);
	case TtsProvider::Google: return SecretStore::loadNamedKey(kGoogleKey, QStringLiteral("Google Cloud"));
	}
	return QString();
}

bool TtsEngine::saveKey(TtsProvider p, const QString &key)
{
	switch (p) {
	case TtsProvider::OpenAI: return SecretStore::saveApiKey(SpeechProvider::OpenAI, key);
	case TtsProvider::ElevenLabs: return SecretStore::saveApiKey(SpeechProvider::ElevenLabs, key);
	case TtsProvider::Deepgram: return SecretStore::saveApiKey(SpeechProvider::Deepgram, key);
	case TtsProvider::Google: return SecretStore::saveNamedKey(kGoogleKey, QStringLiteral("Google Cloud"), key);
	}
	return false;
}

QString TtsEngine::pronunciationText()
{
	return settings().value(QStringLiteral("tts/pronunciations")).toString();
}

void TtsEngine::setPronunciationText(const QString &t)
{
	QSettings s = settings();
	s.setValue(QStringLiteral("tts/pronunciations"), t);
}

QVector<PronunciationRule> TtsEngine::pronunciations()
{
	return parsePronunciations(pronunciationText());
}

GroupRule TtsEngine::captionRule()
{
	QSettings s = settings();
	GroupRule r;
	r.maxWords = s.value(QStringLiteral("subtitles/maxWords"), 3).toInt();
	r.maxDurationMs = qint64(s.value(QStringLiteral("subtitles/maxSeconds"), 2.5).toDouble() * 1000.0);
	r.pauseMs = s.value(QStringLiteral("subtitles/pauseMs"), 400).toInt();
	r.maxChars = s.value(QStringLiteral("subtitles/maxChars"), 32).toInt();
	return r;
}

double TtsEngine::fitTempo(qint64 naturalMs, qint64 targetMs)
{
	if (naturalMs <= 0 || targetMs <= 0)
		return 1.0;
	return std::clamp(double(naturalMs) / double(targetMs), kMinTempo, kMaxTempo);
}

TtsEngine::TtsEngine(QObject *parent) : QObject(parent)
{
	synth_ = new TtsSynthesizer(this);
	transcriber_ = new SpeechTranscriber(this);
	connect(synth_, &TtsSynthesizer::synthesized, this, &TtsEngine::onSynthesized);
	connect(synth_, &TtsSynthesizer::failed, this, [this](const QString &why) {
		if (running_)
			fail(why);
	});
	connect(transcriber_, &SpeechTranscriber::finished, this, &TtsEngine::onTranscribed);
	connect(transcriber_, &SpeechTranscriber::failed, this, [this](const QString &why) {
		if (running_)
			fail(QStringLiteral("timing the words: %1").arg(why));
	});
	connect(transcriber_, &SpeechTranscriber::progress, this, [this](const QString &s) {
		emit progress(QStringLiteral("Timing the words of part %1 of %2: %3").arg(idx_ + 1).arg(pieces_.size()).arg(s));
	});
}

TtsEngine::~TtsEngine()
{
	cleanUp();
}

void TtsEngine::cleanUp()
{
	if (!workDir_.isEmpty())
		QDir(workDir_).removeRecursively();
}

void TtsEngine::start(const TtsJob &job)
{
	if (running_) {
		emit failed(QStringLiteral("already making a narration"));
		return;
	}
	job_ = job;
	pieces_.clear();
	wavs_.clear();
	words_.clear();
	idx_ = 0;
	// Pauses, paces and sentences into requests; the marks in each service's
	// own form (TtsPlan.hpp).
	const TtsProviderInfo &info = ttsProviderInfo(job.spec.provider);
	for (const TtsPlanned &pl : ttsPlan(job.text, job.spec.provider, job.spec.model, job.rules, info.maxChars,
					      job.spec.style))
		pieces_.append({pl, ttsRender(job.spec.provider, job.spec.model, pl, job.spec.style), pl.gapBeforeMs});
	if (pieces_.isEmpty()) {
		emit failed(QStringLiteral("there is nothing to say in the text"));
		return;
	}
	workDir_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
		   QStringLiteral("/harpia-tts-%1-%2")
			   .arg(QCoreApplication::applicationPid())
			   .arg(QDateTime::currentMSecsSinceEpoch());
	QDir().mkpath(workDir_);
	running_ = true;
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("%1 part(s), %2, voice %3, language %4%5")
			  .arg(pieces_.size())
			  .arg(ttsProviderName(job.spec.provider), job.spec.voice, job.spec.language)
			  .arg(job.rules.isEmpty() ? QString() : QStringLiteral(", %1 pronunciation rule(s)").arg(job.rules.size())));
	nextPiece();
}

void TtsEngine::cancel()
{
	if (!running_)
		return;
	synth_->cancel();
	transcriber_->cancel();
	fail(QStringLiteral("cancelled"));
}

void TtsEngine::nextPiece()
{
	if (!running_)
		return;
	if (idx_ >= pieces_.size()) {
		assemble();
		return;
	}
	TtsRequest r;
	r.provider = job_.spec.provider;
	r.apiKey = job_.apiKey;
	const TtsRendered &rd = pieces_[idx_].rendered;
	r.text = rd.text;
	r.timingText = rd.timingText;
	r.model = job_.spec.model;
	r.voice = job_.spec.voice;
	r.language = job_.spec.language;
	// OpenAI: the style and what the marks ask, as instructions.
	r.style = job_.spec.provider == TtsProvider::OpenAI ? rd.instructions : job_.spec.style;
	// The pace: the service's own rate, times this passage's where the pace
	// is per request (ElevenLabs v2: 0.7-1.2; Deepgram: its own).
	r.speed = job_.spec.provider == TtsProvider::Deepgram ? rd.speed
		  : job_.spec.provider == TtsProvider::ElevenLabs ? std::clamp(job_.spec.speed * rd.speed, 0.7, 1.2)
								  : job_.spec.speed;
	if (idx_ > 0)
		r.previousText = pieces_[idx_ - 1].st().spoken;
	if (idx_ + 1 < pieces_.size())
		r.nextText = pieces_[idx_ + 1].st().spoken;
	emit progress(QStringLiteral("Speaking part %1 of %2 with %3…")
			      .arg(idx_ + 1)
			      .arg(pieces_.size())
			      .arg(ttsProviderName(job_.spec.provider)));
	synth_->synthesize(r);
}

void TtsEngine::onSynthesized(const TtsReply &reply)
{
	if (!running_)
		return;
	// The piece as it came, then as the editor plays audio: 48 kHz stereo.
	const QString raw = workDir_ + QStringLiteral("/piece_%1.%2").arg(idx_).arg(reply.ext);
	const QString wav = workDir_ + QStringLiteral("/piece_%1_48k.wav").arg(idx_);
	QFile f(raw);
	if (!f.open(QIODevice::WriteOnly) || f.write(reply.audio) != reply.audio.size()) {
		fail(QStringLiteral("could not write the audio to %1").arg(QDir::toNativeSeparators(workDir_)));
		return;
	}
	f.close();
	tts_audio::Pcm pcm;
	if (!VoiceoverMixer::decodeToWav(raw, wav) || !tts_audio::readWav16(wav, pcm)) {
		fail(QStringLiteral("the audio %1 sent could not be read").arg(ttsProviderName(job_.spec.provider)));
		return;
	}
	wavs_.append(wav);
	pieceMs_ = pcm.durationMs();
	if (reply.timed) {
		pieceDone(reply.words);
		return;
	}
	// Audio only: hear it back to find where each word is.
	QString err;
	const QVector<SpeechAudioChunk> chunks = AudioForSpeech::prepare(wav, 0, pieceMs_, workDir_, &err);
	if (chunks.isEmpty()) {
		fail(QStringLiteral("preparing the audio for word timing: %1").arg(err));
		return;
	}
	SpeechTranscriber::Job job;
	job.provider = transcriberFor(job_.spec.provider);
	job.wavPath = chunks.first().wavPath;
	job.apiKey = job_.apiKey;
	job.language = job_.spec.language;
	emit progress(QStringLiteral("Timing the words of part %1 of %2…").arg(idx_ + 1).arg(pieces_.size()));
	transcriber_->transcribe(job);
}

void TtsEngine::onTranscribed(const Transcript &t)
{
	if (!running_)
		return;
	QVector<bool> matched;
	QVector<ClipWordTime> spoken = alignToTranscript(ttsWords(pieces_[idx_].st().spoken), t.words, &matched);
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("part %1: %2 of %3 words matched to what was heard")
			  .arg(idx_ + 1)
			  .arg(std::count(matched.begin(), matched.end(), true))
			  .arg(spoken.size()));
	fillUnmatched(spoken, matched, pieceMs_);
	pieceDone(spoken);
}

void TtsEngine::pieceDone(const QVector<ClipWordTime> &spokenTimes)
{
	// Back from what was said to what is written: the captions' words.
	words_.append(ttsWrittenTimes(pieces_[idx_].st(), spokenTimes));
	++idx_;
	nextPiece();
}

void TtsEngine::assemble()
{
	QVector<tts_audio::Pcm> pcms;
	QVector<qint64> gaps;
	for (int i = 0; i < wavs_.size(); ++i) {
		tts_audio::Pcm p;
		if (!tts_audio::readWav16(wavs_[i], p)) {
			fail(QStringLiteral("a piece of the audio could not be read back"));
			return;
		}
		pcms.append(p);
		gaps.append(pieces_[i].gapBeforeMs);
	}
	QVector<qint64> offsets;
	const tts_audio::Pcm all = tts_audio::join(pcms, gaps, &offsets);
	QVector<ClipWordTime> words;
	for (int i = 0; i < words_.size() && i < offsets.size(); ++i)
		for (ClipWordTime w : words_[i]) {
			w.startMs += offsets[i];
			w.endMs += offsets[i];
			words.append(w);
		}

	TtsResult r;
	r.spec = job_.spec;
	r.wavPath = job_.outPath;
	r.spec.tempo = job_.fitToMs > 0 ? fitTempo(all.durationMs(), job_.fitToMs) : job_.spec.tempo;
	QDir().mkpath(QFileInfo(job_.outPath).absolutePath());
	if (std::abs(r.spec.tempo - 1.0) > 0.001) {
		// Stretched with the pitch kept (FFmpeg's atempo, as for clip speed).
		const QString joined = workDir_ + QStringLiteral("/joined.wav");
		tts_audio::Pcm out;
		if (!tts_audio::writeWav16(joined, all) || !VoiceoverMixer::decodeToWav(joined, job_.outPath, r.spec.tempo) ||
		    !tts_audio::readWav16(job_.outPath, out)) {
			fail(QStringLiteral("could not change the narration's pace"));
			return;
		}
		for (ClipWordTime &w : words) {
			w.startMs = qint64(std::llround(w.startMs / r.spec.tempo));
			w.endMs = qint64(std::llround(w.endMs / r.spec.tempo));
		}
		r.durationMs = out.durationMs();
	} else {
		if (!tts_audio::writeWav16(job_.outPath, all)) {
			fail(QStringLiteral("could not save the narration in %1")
				     .arg(QDir::toNativeSeparators(QFileInfo(job_.outPath).absolutePath())));
			return;
		}
		r.durationMs = all.durationMs();
	}
	r.words = words;
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("narration saved: %1 (%2 s, %3 words%4)")
			  .arg(QDir::toNativeSeparators(r.wavPath))
			  .arg(r.durationMs / 1000.0, 0, 'f', 1)
			  .arg(words.size())
			  .arg(std::abs(r.spec.tempo - 1.0) > 0.001 ? QStringLiteral(", pace ×%1").arg(r.spec.tempo, 0, 'f', 2)
								    : QString()));
	running_ = false;
	cleanUp();
	emit finished(r);
}

void TtsEngine::fail(const QString &why)
{
	if (!running_)
		return;
	running_ = false;
	cleanUp();
	editorLog(EditorLogLevel::Error, kWhere,
		  QStringLiteral("stopped at part %1 of %2: %3").arg(idx_ + 1).arg(pieces_.size()).arg(why));
	emit failed(why);
}

} // namespace harpia
