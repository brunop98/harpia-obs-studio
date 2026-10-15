#include "TtsDialog.hpp"

#include "TtsAudio.hpp"
#include "TtsEngine.hpp"
#include "TtsSynthesizer.hpp"
#include "TtsMarkup.hpp"
#include "TtsText.hpp"

#include "../../ui/EditorLog.hpp"
#include "../../ui/InfoHint.hpp"
#include "../AudioPreview.hpp"
#include "../ProjectPaths.hpp"
#include "../VoiceoverMixer.hpp"
#include "../subtitles/SecretStore.hpp"

#include <QAction>
#include <QCheckBox>
#include <QMenu>
#include <QMessageBox>
#include <QSyntaxHighlighter>
#include <QTextCursor>
#include <QToolButton>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

// The marks in the text box, in colour: pauses blue, marks the chosen service
// does orange, marks it cannot do grey and struck through. Brackets that are
// not marks are left as text.
class TtsMarkHighlighter : public QSyntaxHighlighter {
public:
	explicit TtsMarkHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {}
	void setService(TtsProvider p, const QString &model)
	{
		provider_ = p;
		model_ = model;
		rehighlight();
	}

protected:
	void highlightBlock(const QString &text) override
	{
		static const QRegularExpression token(QStringLiteral("\\[([^\\[\\]\\n]{1,48})\\]"));
		static const QRegularExpression pause(QStringLiteral("^(?:pause|pausa)\\b"),
						      QRegularExpression::CaseInsensitiveOption);
		auto it = token.globalMatch(text);
		while (it.hasNext()) {
			const QRegularExpressionMatch m = it.next();
			const QString inside = m.captured(1).trimmed();
			const QString low = inside.toLower();
			const bool closing = low.startsWith(QLatin1Char('/'));
			const QString name = closing ? low.mid(1).trimmed() : low;
			QTextCharFormat f;
			f.setFontWeight(QFont::DemiBold);
			if (pause.match(inside).hasMatch()) {
				f.setForeground(QColor(0x6e, 0xa8, 0xfe));
			} else if (const TtsTag *t = ttsTagNamed(name)) {
				if (ttsSupport(provider_, model_, *t).level == TtsSupport::None) {
					f.setForeground(QColor(0x80, 0x84, 0x8c));
					f.setFontStrikeOut(true);
				} else {
					f.setForeground(QColor(0xe5, 0xa8, 0x4d));
				}
			} else if (!closing && ttsIsOwnTag(inside)) {
				const bool ok = provider_ == TtsProvider::ElevenLabs && elevenExpressive(model_);
				f.setForeground(ok ? QColor(0xc0, 0x8c, 0xf0) : QColor(0x80, 0x84, 0x8c));
				f.setFontStrikeOut(!ok);
			} else {
				continue; // not a mark: text
			}
			setFormat(int(m.capturedStart()), int(m.capturedLength()), f);
		}
	}

private:
	TtsProvider provider_ = TtsProvider::OpenAI;
	QString model_;
};

namespace {
QSettings settings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
const QString kGrp = QStringLiteral("tts/");
const QString kSubs = QStringLiteral("subtitles/"); // the caption settings, shared
constexpr int kRightWidth = 440; // the settings column

QString clock(qint64 ms)
{
	return QStringLiteral("%1:%2.%3")
		.arg(ms / 60000)
		.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
		.arg((ms / 100) % 10);
}
} // namespace

TtsDialog::TtsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Text to speech"));
	setModal(false);
	engine_ = new TtsEngine(this);
	voices_ = new TtsSynthesizer(this);
	sampler_ = new TtsSynthesizer(this);
	buildUi();
	loadSettings();

	connect(engine_, &TtsEngine::progress, this, [this](const QString &s) { status_->setText(s); });
	connect(engine_, &TtsEngine::finished, this, &TtsDialog::onFinished);
	connect(engine_, &TtsEngine::failed, this, [this](const QString &why) {
		setBusy(false);
		status_->setText(QStringLiteral("Stopped: %1").arg(why));
	});

	connect(voices_, &TtsSynthesizer::voicesListed, this, [this](const QVector<TtsVoice> &v) {
		loadVoices_->setEnabled(true);
		setVoices(v, voice());
		status_->setText(QStringLiteral("%1 voices from your %2 account.").arg(v.size()).arg(ttsProviderName(provider())));
	});
	connect(voices_, &TtsSynthesizer::failed, this, [this](const QString &why) {
		loadVoices_->setEnabled(true);
		status_->setText(QStringLiteral("Could not load voices: %1").arg(why));
	});

	connect(sampler_, &TtsSynthesizer::synthesized, this, [this](const TtsReply &reply) {
		preview_->setEnabled(true);
		// Decoded the way the narration will be, then played from memory.
		const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
		const QString raw = QDir(dir).filePath(QStringLiteral("harpia-tts-sample.%1").arg(reply.ext));
		const QString wav = QDir(dir).filePath(QStringLiteral("harpia-tts-sample-48k.wav"));
		bool ok = false;
		tts_audio::Pcm pcm;
		{
			QFile f(raw);
			ok = f.open(QIODevice::WriteOnly) && f.write(reply.audio) == reply.audio.size();
		}
		ok = ok && VoiceoverMixer::decodeToWav(raw, wav) && tts_audio::readWav16(wav, pcm);
		QFile::remove(raw);
		QFile::remove(wav);
		if (!ok || pcm.channels != 2) {
			status_->setText(QStringLiteral("The sample could not be played."));
			return;
		}
		std::vector<float> fl(size_t(pcm.samples.size()));
		for (qsizetype i = 0; i < pcm.samples.size(); ++i)
			fl[size_t(i)] = pcm.samples[i] / 32768.0f;
		samples_.insert(sampleKey_, fl);
		status_->setText(QStringLiteral("Playing a sample of %1.").arg(voice_->currentText()));
		playPcm(fl);
	});
	connect(sampler_, &TtsSynthesizer::failed, this, [this](const QString &why) {
		preview_->setEnabled(true);
		if (why != QLatin1String("cancelled"))
			status_->setText(QStringLiteral("No sample: %1").arg(why));
	});
}

TtsDialog::~TtsDialog() = default;

void TtsDialog::buildUi()
{
	auto *outer = new QHBoxLayout(this);
	outer->setContentsMargins(14, 12, 14, 12);
	outer->setSpacing(14);

	// ---- The text, on the left ----
	auto *left = new QVBoxLayout;
	left->setSpacing(6);
	auto *tTitle = new QLabel(QStringLiteral("Text"), this);
	tTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
	left->addWidget(tTitle);
	text_ = new QPlainTextEdit(this);
	text_->setPlaceholderText(QStringLiteral("Paste or type what should be said, or open a text file below.\n\n"
						 "Each word becomes part of a caption, shown while it is spoken."));
	text_->setMinimumSize(320, 300);
	highlighter_ = new TtsMarkHighlighter(text_->document());
	left->addLayout(buildMarksBar());
	left->addWidget(text_, 1);
	auto *pauses = new QLabel(QStringLiteral("Select words and use the buttons above, or type the marks: [pause 1s], "
						 "[slow]…[/slow], [whisper]…[/whisper], [laugh]. A blank line is a %1 s "
						 "pause. No mark is ever spoken or shown in the captions.")
					  .arg(kBlankLinePauseMs / 1000.0, 0, 'f', 1),
				  this);
	pauses->setWordWrap(true);
	pauses->setStyleSheet(QStringLiteral("color:#7f858e;"));
	left->addWidget(pauses);
	auto *tRow = new QHBoxLayout;
	auto *open = new QPushButton(QStringLiteral("Open text file…"), this);
	open->setToolTip(QStringLiteral("A .txt (or .srt: its numbers and times are left out). It replaces the text above."));
	connect(open, &QPushButton::clicked, this, &TtsDialog::openTextFile);
	tRow->addWidget(open);
	count_ = new QLabel(this);
	count_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	tRow->addWidget(count_, 1);
	left->addLayout(tRow);
	connect(text_, &QPlainTextEdit::textChanged, this, &TtsDialog::refreshCount);
	outer->addLayout(left, 1);

	// ---- Settings, on the right: a fixed width, so the wrapped lines in it
	// (what each service is, where its key comes from) have room to be read.
	auto *right = new QWidget(this);
	right->setFixedWidth(kRightWidth);
	auto *lay = new QVBoxLayout(right);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(10);
	outer->addWidget(right);
	lay->addWidget(infoHint(QStringLiteral(
				   "Turns the text into a narration and captions: an audio clip and caption clips, "
				   "linked so they move together. The audio is kept in Movies/Harpia/Speech. "
				   "Right-click a caption afterwards to re-speak just its sentence. Ctrl+Z removes "
				   "them."),
			       this),
		       0, Qt::AlignLeft);

	auto *svc = new QGroupBox(QStringLiteral("Voice"), this);
	auto *sf = new QFormLayout(svc);
	sf->setHorizontalSpacing(10);
	provider_ = new QComboBox(svc);
	for (const TtsProviderInfo &i : ttsProviders())
		provider_->addItem(QString::fromUtf8(i.name), QString::fromLatin1(i.id));
	provider_->setToolTip(QStringLiteral("Which service speaks the text. Each uses its own API key and bills its own account."));
	sf->addRow(QStringLiteral("Service"), provider_);
	about_ = new QLabel(svc);
	about_->setWordWrap(true);
	about_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	sf->addRow(QString(), about_);
	language_ = new QComboBox(svc);
	for (const auto &l : ttsLanguages())
		language_->addItem(l.first, l.second);
	language_->setToolTip(QStringLiteral("The language of the text: Google picks its voices by it, and the word "
					     "timing uses it as a hint."));
	sf->addRow(QStringLiteral("Language"), language_);
	auto *vRow = new QHBoxLayout;
	voice_ = new QComboBox(svc);
	voice_->setEditable(true);
	voice_->setInsertPolicy(QComboBox::NoInsert);
	voice_->setToolTip(QStringLiteral("Pick a voice, or type a voice id the service gave you."));
	vRow->addWidget(voice_, 1);
	preview_ = new QPushButton(QStringLiteral("▶"), svc);
	preview_->setFixedWidth(34);
	preview_->setToolTip(QStringLiteral("Hear this voice say the text's first sentence (or a sample line) before "
					    "generating. A sample already heard plays again for free."));
	connect(preview_, &QPushButton::clicked, this, &TtsDialog::playPreview);
	vRow->addWidget(preview_);
	loadVoices_ = new QPushButton(QStringLiteral("Load my voices"), svc);
	loadVoices_->setToolTip(QStringLiteral("Ask the service for every voice your account can use."));
	connect(loadVoices_, &QPushButton::clicked, this, &TtsDialog::loadVoices);
	vRow->addWidget(loadVoices_);
	sf->addRow(QStringLiteral("Voice"), vRow);
	model_ = new QComboBox(svc);
	model_->setToolTip(QStringLiteral(
		"Multilingual v2 is steady over long texts but takes only pace marks. v4 (and v3) take ElevenLabs' "
		"audio tags: moods, whispers, laughs, your own [tags]."));
	modelLabel_ = new QLabel(QStringLiteral("Model"), svc);
	sf->addRow(modelLabel_, model_);
	connect(model_, &QComboBox::currentIndexChanged, this, [this]() { refreshMarks(); });
	style_ = new QLineEdit(svc);
	style_->setPlaceholderText(QStringLiteral("e.g. Calm and warm, like a documentary"));
	style_->setToolTip(QStringLiteral("How the voice should sound: tone, pace, accent, emotion."));
	styleLabel_ = new QLabel(QStringLiteral("Style"), svc);
	sf->addRow(styleLabel_, style_);
	speed_ = new QDoubleSpinBox(svc);
	speed_->setDecimals(2);
	speed_->setSingleStep(0.05);
	speed_->setSuffix(QStringLiteral(" ×"));
	speedLabel_ = new QLabel(QStringLiteral("Speed"), svc);
	sf->addRow(speedLabel_, speed_);
	pronounce_ = new QPushButton(svc);
	pronounce_->setToolTip(QStringLiteral(
		"Words the voice should say differently from how they are written: \"Harpia = Árpia\". "
		"The captions keep the written word."));
	connect(pronounce_, &QPushButton::clicked, this, &TtsDialog::editPronunciations);
	sf->addRow(QStringLiteral("Pronunciation"), pronounce_);
	auto *keyRow = new QHBoxLayout;
	apiKey_ = new QLineEdit(svc);
	apiKey_->setEchoMode(QLineEdit::Password);
	keyRow->addWidget(apiKey_, 1);
	auto *saveKeyBtn = new QPushButton(QStringLiteral("Save key"), svc);
	connect(saveKeyBtn, &QPushButton::clicked, this, [this]() { storeTypedKey(provider()); });
	connect(apiKey_, &QLineEdit::editingFinished, this, [this]() { storeTypedKey(keyFor_); });
	keyRow->addWidget(saveKeyBtn);
	sf->addRow(QStringLiteral("API key"), keyRow);
	getKey_ = new QLabel(svc);
	getKey_->setTextFormat(Qt::RichText);
	getKey_->setOpenExternalLinks(true);
	getKey_->setTextInteractionFlags(Qt::TextBrowserInteraction);
	getKey_->setWordWrap(true);
	sf->addRow(QString(), getKey_);
	keyState_ = new QLabel(svc);
	keyState_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	keyState_->setWordWrap(true);
	sf->addRow(QString(), keyState_);
	connect(provider_, &QComboBox::currentIndexChanged, this, [this]() {
		storeTypedKey(keyFor_); // a key typed for the service being left is ITS key
		apiKey_->clear();
		refreshProvider();
	});
	connect(language_, &QComboBox::currentIndexChanged, this, [this]() {
		// Google's and Deepgram's voices are per language.
		if (provider() == TtsProvider::Google || provider() == TtsProvider::Deepgram)
			setVoices(ttsDefaultVoices(provider(), languageCode()), QString());
	});
	lay->addWidget(svc);

	// ---- Where it goes ----
	fit_ = new QCheckBox(this);
	fit_->setToolTip(QStringLiteral(
		"Place the narration at the start of the selected clips and stretch it (the pitch stays) to "
		"end with them -- between %1× and %2× its natural pace.")
				 .arg(TtsEngine::kMinTempo)
				 .arg(TtsEngine::kMaxTempo));
	lay->addWidget(fit_);

	// ---- Captions: the Subtitles window's settings ----
	auto *cap = new QGroupBox(QStringLiteral("Captions (shared with the Subtitles window)"), this);
	auto *cf = new QFormLayout(cap);
	cf->setHorizontalSpacing(10);
	maxWords_ = new QSpinBox(cap);
	maxWords_->setRange(1, 20);
	maxWords_->setToolTip(QStringLiteral("1 makes a caption per word. 3 to 5 reads like most social captions."));
	cf->addRow(QStringLiteral("At most words"), maxWords_);
	mode_ = new QComboBox(cap);
	mode_->addItem(QStringLiteral("Whole phrase"));
	mode_->addItem(QStringLiteral("One word at a time"));
	mode_->addItem(QStringLiteral("Build up word by word"));
	cf->addRow(QStringLiteral("Show"), mode_);
	position_ = new QComboBox(cap);
	position_->addItem(QStringLiteral("Bottom"));
	position_->addItem(QStringLiteral("Middle"));
	position_->addItem(QStringLiteral("Top"));
	cf->addRow(QStringLiteral("Position"), position_);
	fontPx_ = new QSpinBox(cap);
	fontPx_->setRange(16, 240);
	fontPx_->setSuffix(QStringLiteral(" px"));
	cf->addRow(QStringLiteral("Text size"), fontPx_);
	lay->addWidget(cap);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	lay->addWidget(status_);
	lay->addStretch(1);
	auto *btns = new QHBoxLayout;
	btns->addStretch(1);
	cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
	connect(cancel_, &QPushButton::clicked, this, [this]() {
		if (engine_->running())
			engine_->cancel();
		else
			reject();
	});
	btns->addWidget(cancel_);
	go_ = new QPushButton(QStringLiteral("Generate speech"), this);
	go_->setDefault(true);
	go_->setMinimumHeight(32);
	connect(go_, &QPushButton::clicked, this, &TtsDialog::startJob);
	btns->addWidget(go_);
	lay->addLayout(btns);
	setMinimumWidth(kRightWidth + 380);
	refreshFit();
	refreshPronunciationButton();
}

void TtsDialog::loadSettings()
{
	QSettings s = settings();
	const QString lang = s.value(kGrp + QStringLiteral("language"),
				     s.value(kSubs + QStringLiteral("language"), QStringLiteral("pt")))
				     .toString();
	const int li = language_->findData(lang);
	language_->setCurrentIndex(std::max(0, li));
	style_->setText(s.value(kGrp + QStringLiteral("style")).toString());
	maxWords_->setValue(s.value(kSubs + QStringLiteral("maxWords"), 3).toInt());
	mode_->setCurrentIndex(std::clamp(s.value(kSubs + QStringLiteral("mode"), 0).toInt(), 0, 2));
	position_->setCurrentIndex(std::clamp(s.value(kSubs + QStringLiteral("position"), 0).toInt(), 0, 2));
	fontPx_->setValue(s.value(kSubs + QStringLiteral("fontPx"), 64).toInt());
	fit_->setChecked(s.value(kGrp + QStringLiteral("fit"), false).toBool());
	const int pi = provider_->findData(s.value(kGrp + QStringLiteral("provider"), QStringLiteral("openai")).toString());
	const QSignalBlocker block(provider_);
	provider_->setCurrentIndex(std::max(0, pi));
	refreshProvider();
	refreshCount();
}

void TtsDialog::saveSettings()
{
	QSettings s = settings();
	s.setValue(kGrp + QStringLiteral("provider"), provider_->currentData().toString());
	s.setValue(kGrp + QStringLiteral("language"), languageCode());
	s.setValue(kGrp + QStringLiteral("voice_") + provider_->currentData().toString(), voice());
	s.setValue(kGrp + QStringLiteral("speed_") + provider_->currentData().toString(), speed_->value());
	s.setValue(kGrp + QStringLiteral("style"), style_->text());
	if (model_->count() > 0)
		s.setValue(kGrp + QStringLiteral("model_") + provider_->currentData().toString(), model_->currentData());
	s.setValue(kGrp + QStringLiteral("fit"), fit_->isChecked());
	s.setValue(kSubs + QStringLiteral("maxWords"), maxWords_->value());
	s.setValue(kSubs + QStringLiteral("mode"), mode_->currentIndex());
	s.setValue(kSubs + QStringLiteral("position"), position_->currentIndex());
	s.setValue(kSubs + QStringLiteral("fontPx"), fontPx_->value());
}

TtsProvider TtsDialog::provider() const
{
	return ttsProviderFromId(provider_->currentData().toString());
}

QString TtsDialog::languageCode() const
{
	return language_->currentData().toString();
}

QString TtsDialog::voice() const
{
	// A picked entry's id; a typed one as typed.
	const int i = voice_->currentIndex();
	if (i >= 0 && voice_->itemText(i) == voice_->currentText())
		return voice_->itemData(i).toString();
	return voice_->currentText().trimmed();
}

TtsSpec TtsDialog::spec() const
{
	TtsSpec s;
	s.provider = provider();
	s.voice = voice();
	s.language = languageCode();
	s.style = ttsProviderInfo(provider()).hasStyle ? style_->text().trimmed() : QString();
	s.speed = ttsProviderInfo(provider()).hasSpeed ? speed_->value() : 1.0;
	s.model = model_->currentData().toString();
	return s;
}

void TtsDialog::setVoices(const QVector<TtsVoice> &voices, const QString &select)
{
	voice_->clear();
	for (const TtsVoice &v : voices)
		voice_->addItem(v.name, v.id);
	const int i = voice_->findData(select);
	if (i >= 0)
		voice_->setCurrentIndex(i);
	else if (!select.isEmpty())
		voice_->setEditText(select);
}

void TtsDialog::refreshProvider()
{
	const TtsProviderInfo &i = ttsProviderInfo(provider());
	const QString name = QString::fromUtf8(i.name);
	const QString shortName = name.section(QLatin1Char(' '), 0, 0);
	about_->setText(QString::fromUtf8(i.about));
	QSettings s = settings();
	setVoices(ttsDefaultVoices(provider(), languageCode()),
		  s.value(kGrp + QStringLiteral("voice_") + QString::fromLatin1(i.id)).toString());
	loadVoices_->setVisible(i.listsVoices);
	style_->setVisible(i.hasStyle);
	styleLabel_->setVisible(i.hasStyle);
	speed_->setVisible(i.hasSpeed);
	speedLabel_->setVisible(i.hasSpeed);
	{
		const QSignalBlocker b(model_);
		model_->clear();
		for (const auto &m : ttsModels(provider()))
			model_->addItem(m.second, m.first);
		const int mi = model_->findData(s.value(kGrp + QStringLiteral("model_") + QString::fromLatin1(i.id)).toString());
		model_->setCurrentIndex(std::max(0, mi));
		model_->setVisible(model_->count() > 0);
		modelLabel_->setVisible(model_->count() > 0);
	}
	refreshMarks();
	if (provider() == TtsProvider::ElevenLabs)
		speed_->setRange(0.7, 1.2);
	else
		speed_->setRange(0.25, 4.0);
	speed_->setValue(s.value(kGrp + QStringLiteral("speed_") + QString::fromLatin1(i.id), 1.0).toDouble());

	apiKey_->setPlaceholderText(QString::fromUtf8(i.keyPlaceholder));
	apiKey_->setToolTip(QStringLiteral("Your own %1 API key. Kept on this computer only -- sealed with Windows' "
					   "credential protection, never written into a project.")
				    .arg(name));
	const QString url = QString::fromLatin1(i.keyUrl);
	getKey_->setText(QStringLiteral("<a href='%1' style='color:#6ea8fe;'>Get your %2 API key ↗</a>"
					"&nbsp;&nbsp;<span style='color:#7f858e;'>(%3)</span>")
				 .arg(url, shortName.toHtmlEscaped(), QString::fromUtf8(i.keySteps).toHtmlEscaped()));
	keyFor_ = provider();
	const QString k = TtsEngine::loadKey(provider());
	const bool shared = provider() != TtsProvider::Google;
	keyState_->setText(k.isEmpty()
				   ? QStringLiteral("No %1 key saved yet. Paste one: it is saved as soon as you leave the box.")
					     .arg(shortName)
				   : QStringLiteral("✓ %1 key saved on this computer: %2%3")
					     .arg(shortName, SecretStore::maskedKey(k),
						  shared ? QStringLiteral(" (the same key the Subtitles window uses).")
							 : QStringLiteral(".")));
	if (!k.isEmpty())
		apiKey_->setPlaceholderText(QStringLiteral("Saved: %1  (paste to replace)").arg(SecretStore::maskedKey(k)));
	// A form row does not grow for a label that wraps onto more lines: give
	// each its height for the width it has (the column is fixed, so this
	// holds), or the service's words are cut off at the bottom.
	for (QLabel *l : {about_, getKey_, keyState_}) {
		l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		l->setMinimumHeight(l->heightForWidth(kRightWidth - 116));
	}
}

bool TtsDialog::storeTypedKey(TtsProvider p)
{
	const QString typed = apiKey_->text().trimmed();
	if (typed.isEmpty())
		return false;
	if (typed == TtsEngine::loadKey(p)) {
		apiKey_->clear();
		return true;
	}
	const bool saved = TtsEngine::saveKey(p, typed);
	if (saved) {
		apiKey_->clear();
		if (p == provider())
			refreshProvider();
	} else {
		status_->setText(QStringLiteral("The %1 key could not be saved on this computer, so it is only used "
						"until this window closes. Help > Error logs says why.")
					 .arg(ttsProviderName(p)));
	}
	return saved;
}

QString TtsDialog::currentKey() const
{
	const QString typed = apiKey_->text().trimmed();
	return typed.isEmpty() ? TtsEngine::loadKey(provider()) : typed;
}

void TtsDialog::setText(const QString &text)
{
	text_->setPlainText(text);
}

QString TtsDialog::text() const
{
	return text_->toPlainText();
}

void TtsDialog::refreshCount()
{
	const QVector<TtsSegment> segs = ttsSegments(text());
	int words = 0;
	qint64 pauses = 0;
	for (const TtsSegment &s : segs) {
		words += int(ttsWords(s.text).size());
		pauses += s.pauseBeforeMs;
	}
	// About 150 words a minute, the pace of most narration.
	const qint64 ms = qint64(words) * 400 + pauses;
	count_->setText(words == 0 ? QString()
				   : QStringLiteral("%1 words%2 · about %3:%4 of speech")
					     .arg(words)
					     .arg(segs.size() > 1 ? QStringLiteral(", %1 pauses").arg(segs.size() - 1) : QString())
					     .arg(ms / 60000)
					     .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0')));
}

void TtsDialog::setFitTarget(qint64 fromMs, qint64 toMs)
{
	fitFrom_ = fromMs;
	fitTo_ = toMs;
	refreshFit();
}

void TtsDialog::refreshFit()
{
	if (!fit_)
		return;
	const bool has = fitTo_ > fitFrom_ && fitFrom_ >= 0;
	fit_->setEnabled(has && !engine_->running());
	fit_->setText(has ? QStringLiteral("Fit to the selected clips  (%1 long, from %2)")
				    .arg(clock(fitTo_ - fitFrom_), clock(fitFrom_))
			  : QStringLiteral("Fit to the selected clips  (select clips on the timeline first)"));
}

void TtsDialog::refreshPronunciationButton()
{
	const int n = int(TtsEngine::pronunciations().size());
	pronounce_->setText(n == 0 ? QStringLiteral("Add words…") : QStringLiteral("%1 word(s)…").arg(n));
}

void TtsDialog::editPronunciations()
{
	QDialog d(this);
	d.setWindowTitle(QStringLiteral("Pronunciation"));
	auto *v = new QVBoxLayout(&d);
	auto *hint = new QLabel(QStringLiteral(
					"One word per line: how it is written, =, how it should be said. The voice says "
					"the right side; the captions show the left.\n"
					"Harpia = Árpia\nOBS = ó bê ésse\nHarpia Recorder = Árpia Recórder"),
				&d);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	v->addWidget(hint);
	auto *edit = new QPlainTextEdit(&d);
	edit->setPlainText(TtsEngine::pronunciationText());
	edit->setMinimumSize(420, 220);
	v->addWidget(edit, 1);
	auto *problem = new QLabel(&d);
	problem->setWordWrap(true);
	problem->setStyleSheet(QStringLiteral("color:#e5a84d;"));
	v->addWidget(problem);
	auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	v->addWidget(bb);
	connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	connect(bb, &QDialogButtonBox::accepted, &d, [&]() {
		QStringList bad;
		parsePronunciations(edit->toPlainText(), &bad);
		if (!bad.isEmpty() && problem->text().isEmpty()) {
			// Once: say which lines are not rules; OK again keeps them anyway.
			problem->setText(QStringLiteral("Not rules (need \"written = spoken\"), and will be ignored: %1. "
							"Press OK again to keep the list as it is.")
						 .arg(bad.join(QStringLiteral(", "))));
			return;
		}
		d.accept();
	});
	if (d.exec() != QDialog::Accepted)
		return;
	TtsEngine::setPronunciationText(edit->toPlainText());
	refreshPronunciationButton();
	samples_.clear(); // a sample may say a word differently now
}

QString TtsDialog::readTextFile(const QString &path, QString *err)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (err)
			*err = QStringLiteral("could not open %1").arg(QFileInfo(path).fileName());
		return QString();
	}
	const QByteArray bytes = f.readAll();
	QStringDecoder utf8(QStringConverter::Utf8);
	QString text = utf8(bytes);
	if (utf8.hasError())
		text = QString::fromLatin1(bytes); // an old ANSI file
	if (path.endsWith(QLatin1String(".srt"), Qt::CaseInsensitive)) {
		QStringList keep;
		static const QRegularExpression number(QStringLiteral("^\\d+$"));
		for (const QString &line : text.split(QLatin1Char('\n'))) {
			const QString t = line.trimmed();
			if (t.isEmpty() || number.match(t).hasMatch() || t.contains(QLatin1String("-->")))
				continue;
			keep << t;
		}
		text = keep.join(QLatin1Char(' '));
	}
	return text.trimmed();
}

QString TtsDialog::previewSentence(const QString &text, const QString &language)
{
	const QString plain = ttsPlainText(text);
	if (!plain.isEmpty()) {
		// The first sentence, at most about 25 words.
		const QString first = ttsChunks(plain, 160).value(0);
		static const QRegularExpression end(QStringLiteral("^(.+?[.!?…])(\\s|$)"));
		const QRegularExpressionMatch m = end.match(first);
		const QString s = m.hasMatch() ? m.captured(1) : first;
		if (ttsHasLetter(s))
			return s.trimmed();
	}
	if (language == QLatin1String("pt"))
		return QStringLiteral("Olá! Esta é a voz que vai narrar o seu vídeo.");
	if (language == QLatin1String("es"))
		return QStringLiteral("¡Hola! Esta es la voz que va a narrar tu vídeo.");
	return QStringLiteral("Hi! This is the voice that will narrate your video.");
}

void TtsDialog::playPcm(const std::vector<float> &pcm)
{
	if (!AudioPreview::available()) {
		status_->setText(QStringLiteral("No audio output device to play the sample on."));
		return;
	}
	if (!player_)
		player_ = new AudioPreview(this);
	player_->setBuffer(pcm);
	player_->setVolume(0.9);
	player_->start(0);
}

void TtsDialog::playPreview()
{
	if (player_ && player_->isPlaying()) {
		player_->stop();
		return;
	}
	storeTypedKey(provider());
	const TtsSpec s = spec();
	TtsRequest r;
	r.provider = s.provider;
	r.apiKey = currentKey();
	r.voice = s.voice;
	r.language = s.language;
	r.style = s.style;
	r.speed = s.speed;
	r.text = ttsSpoken(previewSentence(text(), s.language), TtsEngine::pronunciations()).spoken;
	sampleKey_ = s.toJson() + QLatin1Char('|') + r.text;
	if (const auto it = samples_.constFind(sampleKey_); it != samples_.constEnd()) {
		status_->setText(QStringLiteral("Playing the sample again (not asked for twice)."));
		playPcm(it.value());
		return;
	}
	if (r.apiKey.isEmpty()) {
		status_->setText(QStringLiteral("Paste your %1 API key first.").arg(ttsProviderName(r.provider)));
		apiKey_->setFocus();
		return;
	}
	preview_->setEnabled(false);
	status_->setText(QStringLiteral("Asking %1 for a sample: \"%2\"").arg(ttsProviderName(r.provider), r.text));
	sampler_->synthesize(r);
}

void TtsDialog::openTextFile()
{
	QSettings s = settings();
	const QString dir = s.value(kGrp + QStringLiteral("lastFolder"),
				    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
				    .toString();
	const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open a text"), dir,
							  QStringLiteral("Text (*.txt *.md *.srt);;All files (*)"));
	if (path.isEmpty())
		return;
	s.setValue(kGrp + QStringLiteral("lastFolder"), QFileInfo(path).absolutePath());
	QString err;
	const QString t = readTextFile(path, &err);
	if (!err.isEmpty()) {
		status_->setText(err);
		return;
	}
	setText(t);
	status_->setText(QStringLiteral("Read %1.").arg(QFileInfo(path).fileName()));
}

void TtsDialog::loadVoices()
{
	storeTypedKey(provider());
	loadVoices_->setEnabled(false);
	status_->setText(QStringLiteral("Asking %1 for your voices…").arg(ttsProviderName(provider())));
	voices_->listVoices(provider(), currentKey(), languageCode());
}

GroupRule TtsDialog::groupRule() const
{
	QSettings s = settings();
	GroupRule r;
	r.maxWords = maxWords_->value();
	r.maxDurationMs = qint64(s.value(kSubs + QStringLiteral("maxSeconds"), 2.5).toDouble() * 1000.0);
	r.pauseMs = s.value(kSubs + QStringLiteral("pauseMs"), 400).toInt();
	r.maxChars = s.value(kSubs + QStringLiteral("maxChars"), 32).toInt();
	return r;
}

SubtitleLook TtsDialog::look() const
{
	QSettings s = settings();
	SubtitleLook l;
	l.mode = SubtitleMode(mode_->currentIndex());
	l.position = SubtitleLook::Position(position_->currentIndex());
	l.style.fontPx = fontPx_->value();
	l.style.bold = s.value(kSubs + QStringLiteral("bold"), true).toBool();
	l.style.boxEnabled = s.value(kSubs + QStringLiteral("box"), false).toBool();
	return l;
}

QString TtsDialog::speechFolder()
{
	return QDir(harpiaBaseFolder()).filePath(QStringLiteral("Speech"));
}

QString TtsDialog::fileNameFor(const QString &text, const QDateTime &when)
{
	// The first few words, as a file name: "2026-10-15 14-03-22 Ola a todos.wav".
	QStringList words;
	for (const TtsWord &w : ttsWords(ttsPlainText(text))) {
		QString clean;
		for (const QChar c : w.text)
			if (c.isLetterOrNumber())
				clean += c;
		if (!clean.isEmpty())
			words << clean;
		if (words.size() == 5)
			break;
	}
	QString name = when.toString(QStringLiteral("yyyy-MM-dd hh-mm-ss"));
	if (!words.isEmpty())
		name += QLatin1Char(' ') + words.join(QLatin1Char(' ')).left(48);
	return name + QStringLiteral(".wav");
}

void TtsDialog::setBusy(bool on)
{
	go_->setEnabled(!on);
	provider_->setEnabled(!on);
	language_->setEnabled(!on);
	voice_->setEnabled(!on);
	text_->setReadOnly(on);
	cancel_->setText(on ? QStringLiteral("Stop") : QStringLiteral("Cancel"));
	refreshFit();
}

QLayout *TtsDialog::buildMarksBar()
{
	auto *bar = new QHBoxLayout;
	bar->setSpacing(4);
	const auto button = [&](const QString &text, const QString &tip) {
		auto *b = new QToolButton(this);
		b->setText(text);
		b->setToolTip(tip);
		b->setAutoRaise(false);
		b->setToolButtonStyle(Qt::ToolButtonTextOnly);
		bar->addWidget(b);
		return b;
	};
	// A drop-down of marks; each item wraps the selection (a sound or a pause
	// is put in where the cursor is).
	const auto menuOf = [&](const QString &text, const QString &tip, std::initializer_list<const char *> tags) {
		QToolButton *b = button(text, tip);
		auto *menu = new QMenu(b);
		menu->setToolTipsVisible(true);
		for (const char *name : tags) {
			const TtsTag *t = ttsTagNamed(QString::fromLatin1(name));
			QAction *a = menu->addAction(QString::fromUtf8(t->label));
			connect(a, &QAction::triggered, this, [this, t]() {
				if (t->sound)
					insertMark(QStringLiteral("[%1]").arg(QString::fromLatin1(t->tag)));
				else
					wrapSelection(QString::fromLatin1(t->tag));
			});
			markActions_.append({a, t});
		}
		b->setMenu(menu);
		b->setPopupMode(QToolButton::InstantPopup);
		return b;
	};

	QToolButton *pause = button(QStringLiteral("Pause"), QStringLiteral("Silence where the cursor is. Every service: "
										  "Harpia puts the silence in itself."));
	{
		auto *menu = new QMenu(pause);
		for (const QString &d : {QStringLiteral("0.3s"), QStringLiteral("0.5s"), QStringLiteral("1s"), QStringLiteral("2s"),
					 QStringLiteral("3s")})
			connect(menu->addAction(d), &QAction::triggered, this,
				[this, d]() { insertMark(QStringLiteral("[pause %1]").arg(d)); });
		pause->setMenu(menu);
		pause->setPopupMode(QToolButton::InstantPopup);
	}
	menuOf(QStringLiteral("Pace"), QStringLiteral("Say the selected words slower or faster."), {"slow", "fast"});
	menuOf(QStringLiteral("Pitch"), QStringLiteral("Say the selected words higher or lower."), {"high", "low"});
	menuOf(QStringLiteral("Volume"), QStringLiteral("Say the selected words louder or softer."), {"loud", "soft"});
	{
		const TtsTag *t = ttsTagNamed(QStringLiteral("stress"));
		QToolButton *b = button(QString::fromUtf8(t->label), QString());
		auto *a = new QAction(QString::fromUtf8(t->label), b);
		connect(a, &QAction::triggered, this, [this]() { wrapSelection(QStringLiteral("stress")); });
		b->setDefaultAction(a);
		markActions_.append({a, t});
	}
	menuOf(QStringLiteral("Mood"), QStringLiteral("How the selected words sound."),
	       {"whisper", "excited", "sad", "calm", "cheerful"});
	menuOf(QStringLiteral("Sound"), QStringLiteral("A laugh or a sigh where the cursor is."), {"laugh", "sigh"});
	QToolButton *clear = button(QStringLiteral("Clear"),
				    QStringLiteral("Take every mark out of the selected text (or the whole text)."));
	connect(clear, &QToolButton::clicked, this, &TtsDialog::clearMarks);
	bar->addStretch(1);
	auto *outer = new QVBoxLayout;
	outer->setSpacing(3);
	outer->addLayout(bar);
	marksNote_ = new QLabel(this);
	marksNote_->setWordWrap(true);
	marksNote_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	outer->addWidget(marksNote_);
	return outer;
}

void TtsDialog::refreshMarks()
{
	const TtsProvider p = provider();
	const QString model = model_ ? model_->currentData().toString() : QString();
	int none = 0, approx = 0;
	for (const auto &ma : markActions_) {
		const TtsSupportInfo s = ttsSupport(p, model, *ma.second);
		QString label = QString::fromUtf8(ma.second->label);
		if (s.level == TtsSupport::Approx)
			label += QStringLiteral("  ≈"); // done another way
		ma.first->setText(label);
		ma.first->setEnabled(s.level != TtsSupport::None);
		ma.first->setToolTip(s.how);
		none += s.level == TtsSupport::None;
		approx += s.level == TtsSupport::Approx;
	}
	if (marksNote_) {
		const QString name = ttsProviderName(p) + (model_ && model_->count() ? QStringLiteral(" ") + model_->currentText().section(QLatin1Char(':'), 0, 0) : QString());
		marksNote_->setText(none == 0 && approx == 0
					    ? QStringLiteral("%1 does every mark itself.").arg(name)
					    : QStringLiteral("%1: ≈ marks are done another way (hover for how)%2.")
						      .arg(name, none ? QStringLiteral("; greyed ones it cannot do") : QString()));
	}
	if (highlighter_)
		highlighter_->setService(p, model);
}

void TtsDialog::wrapSelection(const QString &tag)
{
	QTextCursor c = text_->textCursor();
	const QString open = QStringLiteral("[%1]").arg(tag), close = QStringLiteral("[/%1]").arg(tag);
	if (c.hasSelection()) {
		QString sel = c.selectedText();
		sel.replace(QChar(0x2029), QLatin1Char('\n')); // a selection's line breaks
		c.insertText(open + sel + close);
	} else {
		c.insertText(open + close);
		c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, int(close.size()));
		text_->setTextCursor(c);
	}
	text_->setFocus();
}

void TtsDialog::insertMark(const QString &mark)
{
	QTextCursor c = text_->textCursor();
	c.clearSelection();
	const QString before = text_->toPlainText().mid(std::max(0, c.position() - 1), 1);
	c.insertText((before.isEmpty() || before.at(0).isSpace() ? QString() : QStringLiteral(" ")) + mark +
		     QStringLiteral(" "));
	text_->setTextCursor(c);
	text_->setFocus();
}

void TtsDialog::clearMarks()
{
	QTextCursor c = text_->textCursor();
	const bool sel = c.hasSelection();
	if (!sel)
		c.select(QTextCursor::Document);
	QString t = c.selectedText();
	t.replace(QChar(0x2029), QLatin1Char('\n'));
	static const QRegularExpression token(QStringLiteral("\\[([^\\[\\]\\n]{1,48})\\]"));
	static const QRegularExpression pause(QStringLiteral("^(?:pause|pausa)\\b"), QRegularExpression::CaseInsensitiveOption);
	QString out;
	qsizetype from = 0;
	auto it = token.globalMatch(t);
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		const QString inside = m.captured(1).trimmed();
		const QString name = inside.startsWith(QLatin1Char('/')) ? inside.mid(1).trimmed() : inside;
		const bool mark = pause.match(inside).hasMatch() || ttsTagNamed(name.toLower()) || ttsIsOwnTag(inside);
		out += t.mid(from, m.capturedStart() - from);
		if (!mark)
			out += m.captured(0);
		from = m.capturedEnd();
	}
	out += t.mid(from);
	c.insertText(out);
	text_->setFocus();
}

bool TtsDialog::confirmMarks()
{
	const TtsMarked m = ttsMark(text());
	const QStringList cannot = ttsUnsupported(m, provider(), model_->currentData().toString());
	if (cannot.isEmpty() && m.problems.isEmpty())
		return true;
	QString msg;
	if (!cannot.isEmpty())
		msg += QStringLiteral("%1 cannot do: %2. Those words are read normally.\n\n")
			       .arg(ttsProviderName(provider()), cannot.join(QStringLiteral(", ")));
	if (!m.problems.isEmpty())
		msg += QStringLiteral("In the marks: %1.\n\n").arg(m.problems.join(QStringLiteral("; ")));
	msg += QStringLiteral("Generate anyway?");
	return QMessageBox::question(this, QStringLiteral("Text to speech"), msg, QMessageBox::Yes | QMessageBox::No,
				     QMessageBox::Yes) == QMessageBox::Yes;
}

void TtsDialog::startJob()
{
	if (engine_->running())
		return;
	if (ttsPlainText(text()).isEmpty()) {
		status_->setText(QStringLiteral("Type or paste a text first."));
		text_->setFocus();
		return;
	}
	storeTypedKey(provider());
	if (currentKey().isEmpty()) {
		status_->setText(QStringLiteral("Paste your %1 API key first.").arg(ttsProviderName(provider())));
		apiKey_->setFocus();
		return;
	}
	if (voice().isEmpty()) {
		status_->setText(QStringLiteral("Pick a voice first."));
		return;
	}
	if (!confirmMarks())
		return;
	saveSettings();
	const bool fit = fit_->isChecked() && fit_->isEnabled();
	TtsJob job;
	job.text = text();
	job.spec = spec();
	job.apiKey = currentKey();
	job.rules = TtsEngine::pronunciations();
	job.fitToMs = fit ? fitTo_ - fitFrom_ : 0;
	job.outPath = QDir(speechFolder()).filePath(fileNameFor(text(), QDateTime::currentDateTime()));
	jobAtMs_ = fit ? fitFrom_ : -1;
	jobFitMs_ = job.fitToMs;
	setBusy(true);
	engine_->start(job);
}

void TtsDialog::onFinished(const TtsResult &r)
{
	setBusy(false);
	const bool stretched = std::abs(r.spec.tempo - 1.0) > 0.001;
	QString fitNote;
	if (jobFitMs_ > 0 && std::llabs(r.durationMs - jobFitMs_) > 300)
		fitNote = QStringLiteral(" It could only be paced ×%1 (more would not sound like speech), so it is %2 s "
					 "where the selection is %3 s.")
				  .arg(r.spec.tempo, 0, 'f', 2)
				  .arg(r.durationMs / 1000.0, 0, 'f', 1)
				  .arg(jobFitMs_ / 1000.0, 0, 'f', 1);
	status_->setText(QStringLiteral("Done: %1 s of speech, %2 words%3. It is on the timeline; Ctrl+Z removes it.%4")
				 .arg(r.durationMs / 1000.0, 0, 'f', 1)
				 .arg(r.words.size())
				 .arg(stretched ? QStringLiteral(", paced ×%1 to fit").arg(r.spec.tempo, 0, 'f', 2) : QString())
				 .arg(fitNote));
	emit speechReady(r.wavPath, r.durationMs, r.words, groupRule(), look(), r.spec.toJson(), jobAtMs_);
}

} // namespace harpia
