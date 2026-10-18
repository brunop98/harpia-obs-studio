#include "PcAudioAppsDialog.hpp"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

namespace {

const QString kFromKey = QStringLiteral("recorder/pcAudioFrom");
const QString kOnlyKey = QStringLiteral("recorder/pcAudioOnlyApps");
const QString kExceptKey = QStringLiteral("recorder/pcAudioExceptApp");
const QString kNamesKey = QStringLiteral("recorder/pcAudioAppNames");

constexpr int kExeRole = Qt::UserRole;

QString exeKey(const QString &exe)
{
	return QString::fromStdString(normalizeAppExe(exe.toStdString()));
}

QString nameFor(const QString &exe, const QHash<QString, QString> &names)
{
	const QString name = names.value(exeKey(exe));
	return name.isEmpty() ? QFileInfo(exe).completeBaseName() : name;
}

QString shortName(const QString &name)
{
	return name.size() <= 18 ? name : name.left(17) + QChar(0x2026);
}

} // namespace

// ---- saved -----------------------------------------------------------------

PcAudioChoice pc_audio_settings::load(QSettings &s, QHash<QString, QString> *names)
{
	PcAudioChoice c;
	c.from = pcAudioFromKey(s.value(kFromKey).toString().toStdString());
	for (const QString &app : s.value(kOnlyKey).toStringList()) {
		const std::string exe = normalizeAppExe(app.toStdString());
		if (!exe.empty() && std::find(c.onlyApps.begin(), c.onlyApps.end(), exe) == c.onlyApps.end())
			c.onlyApps.push_back(exe);
	}
	c.exceptApp = normalizeAppExe(s.value(kExceptKey).toString().toStdString());
	if (names) {
		names->clear();
		const QVariantMap m = s.value(kNamesKey).toMap();
		for (auto it = m.constBegin(); it != m.constEnd(); ++it)
			names->insert(exeKey(it.key()), it.value().toString());
	}
	return c;
}

void pc_audio_settings::save(QSettings &s, const PcAudioChoice &choice, const QHash<QString, QString> &names)
{
	s.setValue(kFromKey, QString::fromLatin1(pcAudioFromKey(choice.from)));
	QStringList only;
	for (const std::string &app : choice.onlyApps)
		only << QString::fromStdString(app);
	s.setValue(kOnlyKey, only);
	s.setValue(kExceptKey, QString::fromStdString(choice.exceptApp));
	// Only the names of the apps in the choice: the rest of what the list
	// showed is of no use once the window closes.
	QVariantMap m;
	for (const QString &app : only)
		if (names.contains(app))
			m.insert(app, names.value(app));
	const QString except = QString::fromStdString(choice.exceptApp);
	if (!except.isEmpty() && names.contains(except))
		m.insert(except, names.value(except));
	s.setValue(kNamesKey, m);
}

QString pcAudioSummary(const PcAudioChoice &c, bool available, const QHash<QString, QString> &names)
{
	if (!available || c.from == PcAudioFrom::All)
		return QStringLiteral("All apps");
	if (c.from == PcAudioFrom::AllExceptApp) {
		const std::vector<PcAudioCapture> one = pcAudioCaptures(c, true);
		if (one.empty() || one.front().wholeMix())
			return QStringLiteral("All apps");
		return QStringLiteral("All but %1").arg(shortName(nameFor(QString::fromStdString(one.front().exe), names)));
	}
	const std::vector<PcAudioCapture> apps = pcAudioCaptures(c, true);
	if (apps.empty())
		return QStringLiteral("No apps");
	if (apps.size() > 1)
		return QStringLiteral("Only %1 apps").arg(int(apps.size()));
	return QStringLiteral("Only %1").arg(shortName(nameFor(QString::fromStdString(apps.front().exe), names)));
}

QStringList pcAudioAppNames(const PcAudioChoice &c, const QHash<QString, QString> &names)
{
	QStringList out;
	for (const PcAudioCapture &cap : pcAudioCaptures(c, true))
		if (!cap.wholeMix())
			out << nameFor(QString::fromStdString(cap.exe), names);
	return out;
}

// ---- the window --------------------------------------------------------------

PcAudioAppsDialog::PcAudioAppsDialog(const PcAudioChoice &choice, bool available, AppLister listApps,
				     const QHash<QString, QString> &names, QWidget *parent)
	: QDialog(parent), available_(available), listApps_(std::move(listApps)), start_(choice)
{
	for (auto it = names.constBegin(); it != names.constEnd(); ++it)
		names_.insert(exeKey(it.key()), it.value());
	setWindowTitle(QStringLiteral("PC audio from"));
	setMinimumWidth(420);
	auto *v = new QVBoxLayout(this);
	v->setSpacing(8);

	auto *intro = new QLabel(QStringLiteral("Record the sound of every app, only some, or every app but one "
						"— so music you listen to while recording stays out of the file. "
						"For every preset."),
				 this);
	intro->setWordWrap(true);
	intro->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	v->addWidget(intro);

	allRadio_ = new QRadioButton(QStringLiteral("All apps"), this);
	onlyRadio_ = new QRadioButton(QStringLiteral("Only these apps"), this);
	exceptRadio_ = new QRadioButton(QStringLiteral("All apps except one"), this);
	allRadio_->setToolTip(QStringLiteral("Everything you hear, as before"));
	onlyRadio_->setToolTip(QStringLiteral("A game, a browser: only the apps you tick are heard in the recording"));
	exceptRadio_->setToolTip(QStringLiteral("Everything but one app: leave your music player out once, for "
						"every game after"));
	for (QRadioButton *r : {allRadio_, onlyRadio_, exceptRadio_})
		v->addWidget(r);
	onlyRadio_->setEnabled(available_);
	exceptRadio_->setEnabled(available_);
	const PcAudioFrom from = available_ ? choice.from : PcAudioFrom::All;
	(from == PcAudioFrom::OnlyApps ? onlyRadio_ : from == PcAudioFrom::AllExceptApp ? exceptRadio_ : allRadio_)
		->setChecked(true);

	pages_ = new QStackedWidget(this);
	auto *allPage = new QLabel(QStringLiteral("Everything you hear is recorded, music included."), pages_);
	allPage->setWordWrap(true);
	allPage->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	onlyList_ = new QListWidget(pages_);
	onlyList_->setObjectName(QStringLiteral("pcAudioOnlyList"));
	onlyList_->setSelectionMode(QAbstractItemView::NoSelection);
	exceptList_ = new QListWidget(pages_);
	exceptList_->setObjectName(QStringLiteral("pcAudioExceptList"));
	exceptList_->setSelectionMode(QAbstractItemView::SingleSelection);
	pages_->addWidget(allPage);
	pages_->addWidget(onlyList_);
	pages_->addWidget(exceptList_);
	pages_->setMinimumHeight(200);
	v->addWidget(pages_, 1);

	auto *row = new QHBoxLayout;
	auto *refresh = new QPushButton(QStringLiteral("Refresh list"), this);
	refresh->setToolTip(QStringLiteral("Look again for running apps: start the game or the player first"));
	row->addWidget(refresh);
	row->addStretch(1);
	v->addLayout(row);

	status_ = new QLabel(this);
	status_->setObjectName(QStringLiteral("pcAudioStatus"));
	status_->setWordWrap(true);
	v->addWidget(status_);

	buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	v->addWidget(buttons_);
	connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

	for (QRadioButton *r : {allRadio_, onlyRadio_, exceptRadio_})
		connect(r, &QRadioButton::toggled, this, [this](bool on) {
			if (on)
				syncState();
		});
	connect(refresh, &QPushButton::clicked, this, [this]() { fillLists(); });
	connect(onlyList_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
		// Each app is a capture of its own, and there are only so many.
		if (item->checkState() == Qt::Checked && checkedOnly().size() > kMaxPcAudioApps) {
			const QSignalBlocker block(onlyList_);
			item->setCheckState(Qt::Unchecked);
		}
		syncState();
	});
	connect(exceptList_, &QListWidget::itemSelectionChanged, this, [this]() { syncState(); });
	refresh->setEnabled(available_);

	fillLists();
}

QRadioButton *PcAudioAppsDialog::modeButton(PcAudioFrom f) const
{
	return f == PcAudioFrom::OnlyApps ? onlyRadio_ : f == PcAudioFrom::AllExceptApp ? exceptRadio_ : allRadio_;
}

PcAudioFrom PcAudioAppsDialog::mode() const
{
	if (onlyRadio_->isChecked())
		return PcAudioFrom::OnlyApps;
	if (exceptRadio_->isChecked())
		return PcAudioFrom::AllExceptApp;
	return PcAudioFrom::All;
}

QStringList PcAudioAppsDialog::checkedOnly() const
{
	QStringList out;
	for (int i = 0; i < onlyList_->count(); ++i) {
		const QListWidgetItem *it = onlyList_->item(i);
		if (it->checkState() == Qt::Checked && !it->data(kExeRole).toString().isEmpty())
			out << it->data(kExeRole).toString();
	}
	return out;
}

QString PcAudioAppsDialog::selectedExcept() const
{
	const QList<QListWidgetItem *> sel = exceptList_->selectedItems();
	return sel.isEmpty() ? QString() : sel.first()->data(kExeRole).toString();
}

void PcAudioAppsDialog::fillLists()
{
	// What is ticked now -- or, the first time, what the choice says -- comes
	// back ticked: Refresh only adds what started since.
	QStringList only;
	QString except;
	if (filled_) {
		only = checkedOnly();
		except = selectedExcept();
	} else {
		for (const std::string &app : start_.onlyApps)
			only << QString::fromStdString(app);
		except = QString::fromStdString(normalizeAppExe(start_.exceptApp));
	}
	filled_ = true;

	struct Entry {
		QString exe; // lower case
		QString name;
		QString note;
	};
	QVector<Entry> entries;
	QStringList listed;
	if (available_ && listApps_) {
		for (const SystemAudioApp &a : listApps_()) {
			const QString key = exeKey(a.exe);
			if (key.isEmpty() || listed.contains(key))
				continue;
			listed << key;
			if (!a.name.isEmpty())
				names_.insert(key, a.name);
			entries.append({key, nameFor(key, names_), a.playing ? QStringLiteral("playing") : QString()});
		}
	}
	// Chosen before and not running now: still chosen, and still shown.
	QStringList remembered = only;
	if (!except.isEmpty())
		remembered << except;
	for (const QString &key : remembered)
		if (!listed.contains(key)) {
			listed << key;
			entries.append({key, nameFor(key, names_), QStringLiteral("not running")});
		}

	const QSignalBlocker b1(onlyList_);
	const QSignalBlocker b2(exceptList_);
	onlyList_->clear();
	exceptList_->clear();
	for (const Entry &e : entries) {
		const QString text = e.note.isEmpty() ? e.name : QStringLiteral("%1   · %2").arg(e.name, e.note);
		auto *a = new QListWidgetItem(text, onlyList_);
		a->setData(kExeRole, e.exe);
		a->setToolTip(e.exe);
		a->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		a->setCheckState(only.contains(e.exe) ? Qt::Checked : Qt::Unchecked);
		auto *b = new QListWidgetItem(text, exceptList_);
		b->setData(kExeRole, e.exe);
		b->setToolTip(e.exe);
		if (e.exe == except)
			b->setSelected(true);
	}
	if (entries.isEmpty()) {
		for (QListWidget *l : {onlyList_, exceptList_}) {
			auto *none = new QListWidgetItem(QStringLiteral("No apps found. Start the app, then Refresh list."), l);
			none->setFlags(Qt::NoItemFlags);
		}
	}
	syncState();
}

void PcAudioAppsDialog::syncState()
{
	const PcAudioFrom m = mode();
	pages_->setCurrentIndex(m == PcAudioFrom::OnlyApps ? 1 : m == PcAudioFrom::AllExceptApp ? 2 : 0);
	QString text;
	bool ok = true;
	bool warn = false;
	if (!available_) {
		text = QStringLiteral("Picking apps needs Windows 10 version 2004 or newer. PC Audio records "
				      "everything you hear.");
	} else if (m == PcAudioFrom::OnlyApps) {
		const int n = int(checkedOnly().size());
		if (n == 0) {
			text = QStringLiteral("Tick the apps to record.");
			ok = false;
			warn = true;
		} else {
			text = QStringLiteral("%1 recorded. Everything else — music, calls, notifications — "
					      "stays out.")
				       .arg(n == 1 ? QStringLiteral("1 app") : QStringLiteral("%1 apps").arg(n));
			if (n >= kMaxPcAudioApps)
				text += QStringLiteral(" Up to %1 apps at once.").arg(kMaxPcAudioApps);
		}
	} else if (m == PcAudioFrom::AllExceptApp) {
		const QString ex = selectedExcept();
		if (ex.isEmpty()) {
			text = QStringLiteral("Pick the app to leave out.");
			ok = false;
			warn = true;
		} else {
			text = QStringLiteral("Everything is recorded except %1.").arg(nameFor(ex, names_));
		}
	} else {
		text = QStringLiteral("Everything you hear is recorded, music included.");
	}
	status_->setText(text);
	status_->setStyleSheet(warn ? QStringLiteral("color:#e3b341;") : QStringLiteral("color:#9a9fa8;"));
	if (QPushButton *b = buttons_->button(QDialogButtonBox::Ok))
		b->setEnabled(ok);
}

QString PcAudioAppsDialog::statusText() const
{
	return status_->text();
}

bool PcAudioAppsDialog::okEnabled() const
{
	const QPushButton *b = buttons_->button(QDialogButtonBox::Ok);
	return b && b->isEnabled();
}

PcAudioChoice PcAudioAppsDialog::choice() const
{
	PcAudioChoice c;
	c.from = mode();
	for (const QString &app : checkedOnly())
		c.onlyApps.push_back(app.toStdString());
	c.exceptApp = selectedExcept().toStdString();
	return c;
}

} // namespace harpia
