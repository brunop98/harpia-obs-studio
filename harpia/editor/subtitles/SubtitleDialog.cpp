#include "SubtitleDialog.hpp"

#include "../../ui/EditorLog.hpp"

#include "SpeechTranscriber.hpp"
#include "SecretStore.hpp"
#include "../../ui/InfoHint.hpp"
#include "../timeline/TimelineCompositor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {
QSettings settings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
const QString kGrp = QStringLiteral("subtitles/");
const QSize kPreviewBox(300, 400); // the most the preview may take
} // namespace

QVector<QPair<QString, QString>> SubtitleDialog::languages()
{
	return {{QStringLiteral("Detect automatically"), QString()},
		{QStringLiteral("Portuguese"), QStringLiteral("pt")},
		{QStringLiteral("English"), QStringLiteral("en")},
		{QStringLiteral("Spanish"), QStringLiteral("es")},
		{QStringLiteral("French"), QStringLiteral("fr")},
		{QStringLiteral("German"), QStringLiteral("de")},
		{QStringLiteral("Italian"), QStringLiteral("it")},
		{QStringLiteral("Japanese"), QStringLiteral("ja")},
		{QStringLiteral("Korean"), QStringLiteral("ko")},
		{QStringLiteral("Chinese"), QStringLiteral("zh")},
		{QStringLiteral("Russian"), QStringLiteral("ru")},
		{QStringLiteral("Arabic"), QStringLiteral("ar")},
		{QStringLiteral("Hindi"), QStringLiteral("hi")},
		{QStringLiteral("Dutch"), QStringLiteral("nl")},
		{QStringLiteral("Polish"), QStringLiteral("pl")},
		{QStringLiteral("Turkish"), QStringLiteral("tr")}};
}

SubtitleDialog::SubtitleDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Subtitles"));
	setModal(false);
	transcriber_ = new SpeechTranscriber(this);
	buildUi();
	loadSettings();

	connect(transcriber_, &SpeechTranscriber::progress, this, [this](const QString &s) {
		status_->setText(QStringLiteral("%1  (%2)").arg(s, targetIdx_ >= 0 && targetIdx_ < targets_.size()
									     ? targets_[targetIdx_].name
									     : QString()));
	});
	connect(transcriber_, &SpeechTranscriber::failed, this, [this](const QString &why) { finishJob(why); });
	connect(transcriber_, &SpeechTranscriber::finished, this, [this](const Transcript &t) {
		// Shift this chunk's words into source time and keep going.
		const qint64 off = chunks_[chunkIdx_].offsetMs;
		for (ClipWordTime w : t.words) {
			w.startMs += off;
			w.endMs += off;
			words_.append(w);
		}
		++chunkIdx_;
		nextChunk();
	});
}

SubtitleDialog::~SubtitleDialog()
{
	if (!workDir_.isEmpty())
		QDir(workDir_).removeRecursively();
}

void SubtitleDialog::buildUi()
{
	// Preview on the left, settings on the right.
	auto *outer = new QHBoxLayout(this);
	outer->setContentsMargins(14, 12, 14, 12);
	outer->setSpacing(14);
	auto *left = new QVBoxLayout;
	left->setSpacing(6);
	auto *pvTitle = new QLabel(QStringLiteral("Preview"), this);
	pvTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
	left->addWidget(pvTitle);
	preview_ = new QLabel(this);
	preview_->setAlignment(Qt::AlignCenter);
	preview_->setFixedSize(previewCanvas_.scaled(kPreviewBox, Qt::KeepAspectRatio) + QSize(2, 2));
	preview_->setStyleSheet(QStringLiteral("background:#101114; border:1px solid #2a2d33;"));
	preview_->setToolTip(QStringLiteral(
		"How the captions will look over the current frame, with the settings on the right. "
		"The sample sentence plays in a loop so the word grouping and the Show mode can be seen."));
	left->addWidget(preview_);
	auto *pvNote = new QLabel(QStringLiteral("Sample text over the current frame. The real words "
						 "come from the speech."), this);
	pvNote->setWordWrap(true);
	pvNote->setFixedWidth(kPreviewBox.width());
	pvNote->setStyleSheet(QStringLiteral("color:#7f858e;"));
	left->addWidget(pvNote);
	left->addStretch(1);
	outer->addLayout(left);
	auto *lay = new QVBoxLayout;
	lay->setSpacing(10);
	outer->addLayout(lay, 1);

	lay->addWidget(infoHint(QStringLiteral(
				   "Turns the speech in the selected clips into caption clips on a Subtitles lane, "
				   "lined up with the audio. Edit any caption's text in the Inspector afterwards; "
				   "Ctrl+Z removes the whole batch."),
			       this),
		       0, Qt::AlignLeft);

	// ---- Service ----
	service_ = new QGroupBox(QStringLiteral("Speech to text"), this);
	auto *svc = service_;
	auto *sf = new QFormLayout(svc);
	sf->setHorizontalSpacing(10);
	provider_ = new QComboBox(svc);
	for (const SpeechProviderInfo &i : speechProviders())
		provider_->addItem(QString::fromUtf8(i.name), QString::fromLatin1(i.id));
	provider_->setToolTip(QStringLiteral(
		"Which service turns the speech into words. Each one uses its own API key, saved "
		"separately, and bills its own account. All of them give word-level timings."));
	sf->addRow(QStringLiteral("Service"), provider_);
	about_ = new QLabel(svc);
	about_->setWordWrap(true);
	about_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	sf->addRow(QString(), about_);
	language_ = new QComboBox(svc);
	for (const auto &l : languages())
		language_->addItem(l.first, l.second);
	language_->setToolTip(QStringLiteral(
		"Telling the service the language makes it faster and more accurate. Detect "
		"works, but can guess wrong on a short clip or one with music under it."));
	sf->addRow(QStringLiteral("Language"), language_);
	auto *keyRow = new QHBoxLayout;
	apiKey_ = new QLineEdit(svc);
	apiKey_->setEchoMode(QLineEdit::Password);
	keyRow->addWidget(apiKey_, 1);
	auto *saveKey = new QPushButton(QStringLiteral("Save key"), svc);
	connect(saveKey, &QPushButton::clicked, this, [this]() { storeTypedKey(provider()); });
	// Pasting a key and moving on (or pressing Enter) saves it too: pressing
	// Save key was easy to miss, and a key only typed lasted one run.
	connect(apiKey_, &QLineEdit::editingFinished, this, [this]() { storeTypedKey(keyFor_); });
	keyRow->addWidget(saveKey);
	sf->addRow(QStringLiteral("API key"), keyRow);
	// Where to get one, a click away; follows the picked service.
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
	// Every service's key page at once, for signing up to more than one.
	auto *allKeys = new QLabel(speechKeyLinksHtml(), svc);
	allKeys->setTextFormat(Qt::RichText);
	allKeys->setOpenExternalLinks(true);
	allKeys->setTextInteractionFlags(Qt::TextBrowserInteraction);
	allKeys->setWordWrap(true);
	allKeys->setToolTip(QStringLiteral("Each link opens that service's API key page in your browser."));
	sf->addRow(QStringLiteral("All key pages"), allKeys);
	connect(provider_, &QComboBox::currentIndexChanged, this, [this]() {
		// A key typed for the service being left is saved as ITS key before the
		// box empties for the next one.
		storeTypedKey(keyFor_);
		apiKey_->clear();
		refreshProvider();
	});
	lay->addWidget(svc);

	// ---- Grouping ----
	auto *grp = new QGroupBox(QStringLiteral("Words per caption"), this);
	auto *gf = new QFormLayout(grp);
	gf->setHorizontalSpacing(10);
	maxWords_ = new QSpinBox(grp);
	maxWords_->setRange(1, 20);
	maxWords_->setToolTip(QStringLiteral("1 makes a caption per word. 3 to 5 reads like most social captions."));
	gf->addRow(QStringLiteral("At most words"), maxWords_);
	maxSeconds_ = new QDoubleSpinBox(grp);
	maxSeconds_->setRange(0.5, 15.0);
	maxSeconds_->setDecimals(1);
	maxSeconds_->setSingleStep(0.5);
	maxSeconds_->setSuffix(QStringLiteral(" s"));
	gf->addRow(QStringLiteral("At most on screen"), maxSeconds_);
	pauseMs_ = new QSpinBox(grp);
	pauseMs_->setRange(0, 3000);
	pauseMs_->setSingleStep(50);
	pauseMs_->setSuffix(QStringLiteral(" ms"));
	pauseMs_->setToolTip(QStringLiteral("A silence at least this long starts a new caption. 0 turns it off."));
	gf->addRow(QStringLiteral("Break on a pause of"), pauseMs_);
	maxChars_ = new QSpinBox(grp);
	maxChars_->setRange(6, 120);
	gf->addRow(QStringLiteral("At most characters"), maxChars_);
	lay->addWidget(grp);

	// ---- Look ----
	auto *lk = new QGroupBox(QStringLiteral("Appearance (everything else in the Inspector afterwards)"), this);
	auto *lf = new QFormLayout(lk);
	lf->setHorizontalSpacing(10);
	mode_ = new QComboBox(lk);
	mode_->addItem(QStringLiteral("Whole phrase"));
	mode_->addItem(QStringLiteral("One word at a time"));
	mode_->addItem(QStringLiteral("Build up word by word"));
	lf->addRow(QStringLiteral("Show"), mode_);
	position_ = new QComboBox(lk);
	position_->addItem(QStringLiteral("Bottom"));
	position_->addItem(QStringLiteral("Middle"));
	position_->addItem(QStringLiteral("Top"));
	lf->addRow(QStringLiteral("Position"), position_);
	fontPx_ = new QSpinBox(lk);
	fontPx_->setRange(16, 240);
	fontPx_->setSuffix(QStringLiteral(" px"));
	lf->addRow(QStringLiteral("Text size"), fontPx_);
	bold_ = new QCheckBox(QStringLiteral("Bold"), lk);
	box_ = new QCheckBox(QStringLiteral("Dark box behind the text"), lk);
	auto *flags = new QHBoxLayout;
	flags->addWidget(bold_);
	flags->addWidget(box_);
	flags->addStretch(1);
	lf->addRow(QString(), flags);
	lay->addWidget(lk);

	// ---- Go ----
	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	lay->addWidget(status_);
	auto *btns = new QHBoxLayout;
	btns->addStretch(1);
	cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
	connect(cancel_, &QPushButton::clicked, this, [this]() {
		if (transcriber_->busy())
			transcriber_->cancel();
		else
			reject();
	});
	btns->addWidget(cancel_);
	go_ = new QPushButton(QStringLiteral("Generate subtitles"), this);
	go_->setDefault(true);
	go_->setMinimumHeight(32);
	connect(go_, &QPushButton::clicked, this, &SubtitleDialog::startJob);
	btns->addWidget(go_);
	lay->addLayout(btns);
	setMinimumWidth(460 + kPreviewBox.width() + 14);

	// Any setting that changes the look redraws the preview; the timer steps
	// through the sample so grouping and the word-by-word modes show moving.
	const auto redraw = [this]() { updatePreview(); };
	for (QSpinBox *sb : {maxWords_, pauseMs_, maxChars_, fontPx_})
		connect(sb, &QSpinBox::valueChanged, this, redraw);
	connect(maxSeconds_, &QDoubleSpinBox::valueChanged, this, redraw);
	for (QComboBox *cb : {mode_, position_, language_})
		connect(cb, &QComboBox::currentIndexChanged, this, redraw);
	for (QCheckBox *ck : {bold_, box_})
		connect(ck, &QCheckBox::toggled, this, redraw);
	previewTimer_ = new QTimer(this);
	previewTimer_->setInterval(450);
	connect(previewTimer_, &QTimer::timeout, this, [this]() {
		if (!isVisible())
			return;
		++previewStep_;
		updatePreview();
	});
	previewTimer_->start();
}

QString SubtitleDialog::sampleSentence(const QString &languageCode)
{
	if (languageCode == QLatin1String("pt"))
		return QStringLiteral("Assim as suas legendas vão aparecer no vídeo, palavra por palavra");
	if (languageCode == QLatin1String("es"))
		return QStringLiteral("Así es como se verán tus subtítulos en el vídeo, palabra por palabra");
	return QStringLiteral("This is how your captions will look in the video, word by word");
}

QString SubtitleDialog::previewText(const QString &sentence, const GroupRule &rule, SubtitleMode mode, int step)
{
	// Evenly spoken words, no pauses: what the grouping rules then make of it.
	const QStringList list = sentence.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (list.isEmpty())
		return QString();
	QVector<ClipWordTime> words;
	for (int i = 0; i < list.size(); ++i) {
		ClipWordTime w;
		w.text = list[i];
		w.startMs = i * 350;
		w.endMs = w.startMs + 330;
		words.append(w);
	}
	const QVector<CaptionPiece> pieces = groupWords(words, rule);
	int at = ((step % int(list.size())) + int(list.size())) % int(list.size());
	for (const CaptionPiece &p : pieces) {
		if (at >= p.words.size()) {
			at -= p.words.size();
			continue;
		}
		if (mode == SubtitleMode::Whole)
			return p.text();
		if (mode == SubtitleMode::OneWord)
			return p.words[at].text;
		QString t; // build up word by word
		for (int i = 0; i <= at; ++i)
			t += (i ? QStringLiteral(" ") : QString()) + p.words[i].text;
		return t;
	}
	return QString();
}

QImage SubtitleDialog::renderPreview(const QImage &bg, QSize canvas, const SubtitleLook &look, const QString &text,
				     QSize box)
{
	if (canvas.isEmpty())
		canvas = QSize(1920, 1080);
	const QSize out = canvas.scaled(box, Qt::KeepAspectRatio).expandedTo(QSize(16, 16));
	QImage img(out, QImage::Format_ARGB32_Premultiplied);
	img.fill(QColor(0x2b, 0x2f, 0x36));
	QPainter p(&img);
	if (!bg.isNull()) {
		p.setRenderHint(QPainter::SmoothPixmapTransform, true);
		p.drawImage(QRect(QPoint(0, 0), out), bg);
	} else {
		QLinearGradient g(0, 0, 0, out.height());
		g.setColorAt(0, QColor(0x3a, 0x44, 0x55));
		g.setColorAt(1, QColor(0x1c, 0x1f, 0x26));
		p.fillRect(img.rect(), g);
	}
	if (!text.isEmpty()) {
		// Exactly the clip the dialog will make, drawn by the compositor: the
		// font scales with the canvas, so a smaller canvas is a smaller copy.
		TlClip c;
		c.type = TlClip::Type::Text;
		c.srcEndMs = 1000;
		c.text = look.style;
		c.text.text = text;
		TlTransform tf;
		tf.posX = look.posX;
		tf.posY = subtitlePosY(look);
		TimelineCompositor::drawClip(p, c, tf, out, QImage());
	}
	p.end();
	return img;
}

void SubtitleDialog::setPreviewBackground(const QImage &frame, QSize canvas)
{
	previewBg_ = frame;
	if (!canvas.isEmpty())
		previewCanvas_ = canvas;
	else if (!frame.isNull())
		previewCanvas_ = frame.size();
	// The box takes the canvas's shape: wide for 16:9, tall for 9:16.
	if (preview_)
		preview_->setFixedSize(previewCanvas_.scaled(kPreviewBox, Qt::KeepAspectRatio) + QSize(2, 2));
	updatePreview();
}

void SubtitleDialog::updatePreview()
{
	if (!preview_ || !mode_)
		return;
	const QString text = previewText(sampleSentence(languageCode()), groupRule(), SubtitleMode(mode_->currentIndex()),
					 previewStep_);
	preview_->setPixmap(QPixmap::fromImage(renderPreview(previewBg_, previewCanvas_, look(), text,
							    preview_->size() - QSize(2, 2))));
}

void SubtitleDialog::loadSettings()
{
	QSettings s = settings();
	const QString lang = s.value(kGrp + QStringLiteral("language"), QStringLiteral("pt")).toString();
	for (int i = 0; i < language_->count(); ++i)
		if (language_->itemData(i).toString() == lang)
			language_->setCurrentIndex(i);
	maxWords_->setValue(s.value(kGrp + QStringLiteral("maxWords"), 3).toInt());
	maxSeconds_->setValue(s.value(kGrp + QStringLiteral("maxSeconds"), 2.5).toDouble());
	pauseMs_->setValue(s.value(kGrp + QStringLiteral("pauseMs"), 400).toInt());
	maxChars_->setValue(s.value(kGrp + QStringLiteral("maxChars"), 32).toInt());
	mode_->setCurrentIndex(std::clamp(s.value(kGrp + QStringLiteral("mode"), 0).toInt(), 0, 2));
	position_->setCurrentIndex(std::clamp(s.value(kGrp + QStringLiteral("position"), 0).toInt(), 0, 2));
	fontPx_->setValue(s.value(kGrp + QStringLiteral("fontPx"), 64).toInt());
	bold_->setChecked(s.value(kGrp + QStringLiteral("bold"), true).toBool());
	box_->setChecked(s.value(kGrp + QStringLiteral("box"), false).toBool());
	const int pi = provider_->findData(
		QString::fromLatin1(speechProviderInfo(speechProviderFromId(s.value(kGrp + QStringLiteral("provider")).toString())).id));
	provider_->setCurrentIndex(std::max(0, pi));
	refreshProvider();
	updatePreview();
}

SpeechProvider SubtitleDialog::provider() const
{
	return speechProviderFromId(provider_->currentData().toString());
}

void SubtitleDialog::refreshProvider()
{
	const SpeechProviderInfo &i = speechProviderInfo(provider());
	const QString name = QString::fromUtf8(i.name);
	about_->setText(QString::fromUtf8(i.about));
	apiKey_->setPlaceholderText(QString::fromUtf8(i.keyPlaceholder));
	apiKey_->setToolTip(QStringLiteral(
		"Your own %1 API key. Kept on this computer only -- sealed with Windows' credential "
		"protection, never written into a project. Transcription is billed to that account.")
				    .arg(name));
	const QString url = QString::fromLatin1(i.keyUrl);
	getKey_->setText(QStringLiteral("<a href='%1' style='color:#6ea8fe;'>Get your %2 API key \u2197</a>"
					"&nbsp;&nbsp;<span style='color:#7f858e;'>(%3)</span>")
				 .arg(url, name.section(QLatin1Char(' '), 0, 0).toHtmlEscaped(),
				      QString::fromUtf8(i.keySteps).toHtmlEscaped()));
	getKey_->setToolTip(QStringLiteral("Opens %1 in your browser.").arg(QUrl(url).host()));
	keyFor_ = provider();
	const QString k = SecretStore::loadApiKey(provider());
	const QString shortName = name.section(QLatin1Char(' '), 0, 0);
	keyState_->setText(k.isEmpty() ? QStringLiteral("No %1 key saved yet. Paste one: it is saved as soon as you "
							"leave the box, or press Save key.")
						 .arg(shortName)
				       : QStringLiteral("\u2713 %1 key saved on this computer: %2. It is used "
							"automatically; paste a new one only to replace it.")
						 .arg(shortName, SecretStore::maskedKey(k)));
	// The box itself says a key is there, so an empty-looking field is not
	// read as "the key was lost".
	if (!k.isEmpty())
		apiKey_->setPlaceholderText(QStringLiteral("Saved: %1  (paste to replace)").arg(SecretStore::maskedKey(k)));
	// The wrapped lines above change height with the service: let the form
	// and the window grow to fit instead of clipping them.
	for (QLabel *l : {about_, getKey_, keyState_})
		l->setMinimumHeight(l->heightForWidth(std::max(200, l->width() > 0 ? l->width() : 330)));
	if (layout()) {
		layout()->invalidate();
		layout()->activate();
	}
	if (isVisible())
		resize(width(), std::max(height(), sizeHint().height()));
}

bool SubtitleDialog::storeTypedKey(SpeechProvider p)
{
	const QString typed = apiKey_->text().trimmed();
	if (typed.isEmpty())
		return false;
	if (typed == SecretStore::loadApiKey(p)) {
		apiKey_->clear();
		return true;
	}
	const bool saved = SecretStore::saveApiKey(p, typed);
	if (saved) {
		apiKey_->clear();
		if (p == provider())
			refreshProvider();
	} else if (status_) {
		// Kept in the box, so this run still works; the log says why.
		status_->setText(QStringLiteral("The %1 key could not be saved on this computer, so it will only be "
						"used until the window closes. Help > Error logs says why.")
					 .arg(speechProviderName(p)));
	}
	return saved;
}

QString SubtitleDialog::currentKey() const
{
	// A key typed but not saved still works for this run; saved is the norm.
	const QString typed = apiKey_->text().trimmed();
	return typed.isEmpty() ? SecretStore::loadApiKey(provider()) : typed;
}

void SubtitleDialog::saveSettings()
{
	QSettings s = settings();
	s.setValue(kGrp + QStringLiteral("provider"), provider_->currentData().toString());
	s.setValue(kGrp + QStringLiteral("language"), languageCode());
	s.setValue(kGrp + QStringLiteral("maxWords"), maxWords_->value());
	s.setValue(kGrp + QStringLiteral("maxSeconds"), maxSeconds_->value());
	s.setValue(kGrp + QStringLiteral("pauseMs"), pauseMs_->value());
	s.setValue(kGrp + QStringLiteral("maxChars"), maxChars_->value());
	s.setValue(kGrp + QStringLiteral("mode"), mode_->currentIndex());
	s.setValue(kGrp + QStringLiteral("position"), position_->currentIndex());
	s.setValue(kGrp + QStringLiteral("fontPx"), fontPx_->value());
	s.setValue(kGrp + QStringLiteral("bold"), bold_->isChecked());
	s.setValue(kGrp + QStringLiteral("box"), box_->isChecked());
}

void SubtitleDialog::setTargets(const QVector<Target> &targets)
{
	targets_ = targets;
	qint64 ms = 0;
	for (const Target &t : targets_)
		ms += t.media.srcLenMs();
	status_->setText(targets_.isEmpty()
				 ? QStringLiteral("Select one or more video or audio clips on the timeline first.")
				 : QStringLiteral("%1 clip(s), %2 min %3 s of audio to transcribe.")
					   .arg(targets_.size())
					   .arg(ms / 60000)
					   .arg((ms / 1000) % 60));
	go_->setEnabled(!targets_.isEmpty());
}

GroupRule SubtitleDialog::groupRule() const
{
	GroupRule r;
	r.maxWords = maxWords_->value();
	r.maxDurationMs = qint64(maxSeconds_->value() * 1000.0);
	r.pauseMs = pauseMs_->value();
	r.maxChars = maxChars_->value();
	return r;
}

SubtitleLook SubtitleDialog::look() const
{
	SubtitleLook l;
	l.mode = SubtitleMode(mode_->currentIndex());
	l.position = SubtitleLook::Position(position_->currentIndex());
	l.style.fontPx = fontPx_->value();
	l.style.bold = bold_->isChecked();
	l.style.boxEnabled = box_->isChecked();
	return l;
}

QString SubtitleDialog::languageCode() const
{
	return language_->currentData().toString();
}

void SubtitleDialog::setBusy(bool on)
{
	go_->setEnabled(!on && !targets_.isEmpty());
	language_->setEnabled(!on);
	provider_->setEnabled(!on);
	cancel_->setText(on ? QStringLiteral("Stop") : QStringLiteral("Cancel"));
}

void SubtitleDialog::startJob()
{
	if (targets_.isEmpty())
		return;
	storeTypedKey(provider());
	if (currentKey().isEmpty()) {
		status_->setText(QStringLiteral("Paste your %1 API key first.").arg(speechProviderName(provider())));
		apiKey_->setFocus();
		return;
	}
	saveSettings();
	setBusy(true);
	out_.clear();
	wordsTotal_ = 0;
	targetIdx_ = -1;
	if (workDir_.isEmpty())
		workDir_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
			   QStringLiteral("/harpia-speech-%1").arg(QCoreApplication::applicationPid());
	// Advance to the first target.
	chunkIdx_ = 0;
	chunks_.clear();
	words_.clear();
	nextChunk();
}

void SubtitleDialog::nextChunk()
{
	// Finished the current target's chunks: build its captions, move on.
	if (targetIdx_ >= 0 && chunkIdx_ >= chunks_.size()) {
		const Target &t = targets_[targetIdx_];
		const QVector<CaptionPiece> pieces = groupWords(words_, groupRule());
		const QVector<TlClip> clips = buildSubtitleClips(pieces, t.media, look());
		out_ += clips;
		wordsTotal_ += words_.size();
		for (const SpeechAudioChunk &c : chunks_)
			QFile::remove(c.wavPath);
		chunks_.clear();
		words_.clear();
		chunkIdx_ = 0;
	}
	if (chunks_.isEmpty()) {
		++targetIdx_;
		if (targetIdx_ >= targets_.size()) {
			finishJob(QString());
			return;
		}
		const Target &t = targets_[targetIdx_];
		status_->setText(QStringLiteral("Preparing the audio of %1…").arg(t.name));
		QString err;
		editorLog(EditorLogLevel::Info, QStringLiteral("Subtitles"),
			  QStringLiteral("clip %1 of %2: %3 (%4, source %5-%6 ms), language %7, service %8")
				  .arg(targetIdx_ + 1)
				  .arg(targets_.size())
				  .arg(t.name, QDir::toNativeSeparators(t.path))
				  .arg(t.media.srcStartMs)
				  .arg(t.media.srcEndMs)
				  .arg(languageCode().isEmpty() ? QStringLiteral("detect") : languageCode(),
				       speechProviderName(provider())));
		chunks_ = AudioForSpeech::prepare(t.path, t.media.srcStartMs, t.media.srcEndMs, workDir_, &err);
		if (chunks_.isEmpty()) {
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				  QStringLiteral("could not prepare the audio of %1: %2").arg(t.name, err));
			finishJob(QStringLiteral("%1: %2").arg(t.name, err));
			return;
		}
		chunkIdx_ = 0;
	}
	SpeechTranscriber::Job job;
	job.provider = provider();
	job.wavPath = chunks_[chunkIdx_].wavPath;
	job.apiKey = currentKey();
	job.language = languageCode();
	status_->setText(QStringLiteral("Sending %1 (part %2 of %3)…")
				 .arg(targets_[targetIdx_].name)
				 .arg(chunkIdx_ + 1)
				 .arg(chunks_.size()));
	transcriber_->transcribe(job);
}

void SubtitleDialog::finishJob(const QString &error)
{
	for (const SpeechAudioChunk &c : chunks_)
		QFile::remove(c.wavPath);
	chunks_.clear();
	setBusy(false);
	if (!error.isEmpty()) {
		status_->setText(QStringLiteral("Stopped: %1").arg(error));
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
			  QStringLiteral("stopped at clip %1 of %2, part %3: %4 (see the line above for what came back)")
				  .arg(std::max(1, targetIdx_ + 1))
				  .arg(targets_.size())
				  .arg(chunkIdx_ + 1)
				  .arg(error));
		return;
	}
	if (out_.isEmpty()) {
		status_->setText(QStringLiteral("No speech was found in the selected clips."));
		return;
	}
	const QString summary = QStringLiteral("%1 captions from %2 words").arg(out_.size()).arg(wordsTotal_);
	editorLog(EditorLogLevel::Info, QStringLiteral("Subtitles"), QStringLiteral("done: %1").arg(summary));
	status_->setText(QStringLiteral("Done: %1. They are on the Subtitles lane; Ctrl+Z removes them.").arg(summary));
	emit captionsReady(out_, summary);
}

} // namespace harpia
