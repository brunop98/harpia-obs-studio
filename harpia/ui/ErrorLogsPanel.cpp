#include "ErrorLogsPanel.hpp"

#include "core/Logger.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {

// Detect the level word in a log line ("[HH:MM:SS] LEVEL message"). Returns an
// empty string for continuation lines (e.g. stack-trace tails).
QString levelOf(const QString &line)
{
	const int close = line.indexOf(QStringLiteral("] "));
	if (close < 0)
		return {};
	const QString rest = line.mid(close + 2).trimmed();
	for (const char *lvl : {"ERROR", "WARNING", "INFO", "DEBUG"}) {
		if (rest.startsWith(QLatin1String(lvl)))
			return QString::fromLatin1(lvl);
	}
	return {};
}

QColor colorFor(const QString &level)
{
	if (level == QStringLiteral("ERROR"))
		return QColor(0xe2, 0x53, 0x4a);
	if (level == QStringLiteral("WARNING"))
		return QColor(0xd2, 0x99, 0x22);
	return QColor(0xd0, 0xd0, 0xd0);
}

} // namespace

ErrorLogsPanel::ErrorLogsPanel(QWidget *parent) : QWidget(parent, Qt::Window)
{
	setWindowTitle(QStringLiteral("Error Logs"));
	resize(760, 540);

	auto *root = new QVBoxLayout(this);

	// Session selector + open buttons.
	auto *top = new QHBoxLayout;
	top->addWidget(new QLabel(QStringLiteral("Session:"), this));
	sessionCombo_ = new QComboBox(this);
	sessionCombo_->setMinimumWidth(280);
	top->addWidget(sessionCombo_, 1);
	auto *openFileBtn = new QPushButton(QStringLiteral("Open Log File"), this);
	auto *openFolderBtn = new QPushButton(QStringLiteral("Open Log Folder"), this);
	top->addWidget(openFileBtn);
	top->addWidget(openFolderBtn);
	root->addLayout(top);

	pathLabel_ = new QLabel(this);
	pathLabel_->setStyleSheet(QStringLiteral("color: gray;"));
	pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	root->addWidget(pathLabel_);

	// Filters.
	auto *filters = new QHBoxLayout;
	errorCheck_ = new QCheckBox(QStringLiteral("Errors"), this);
	warningCheck_ = new QCheckBox(QStringLiteral("Warnings"), this);
	infoCheck_ = new QCheckBox(QStringLiteral("Info"), this);
	errorCheck_->setChecked(true);
	warningCheck_->setChecked(true);
	infoCheck_->setChecked(true);
	filters->addWidget(errorCheck_);
	filters->addWidget(warningCheck_);
	filters->addWidget(infoCheck_);
	filters->addStretch(1);
	// The view only reloads on show — a Refresh button keeps it useful while
	// the window stays open during a recording.
	auto *refreshBtn = new QPushButton(QStringLiteral("Refresh"), this);
	auto *copyBtn = new QPushButton(QStringLiteral("Copy to Clipboard"), this);
	auto *clearBtn = new QPushButton(QStringLiteral("Clear Logs"), this);
	filters->addWidget(refreshBtn);
	filters->addWidget(copyBtn);
	filters->addWidget(clearBtn);
	root->addLayout(filters);

	view_ = new QTextEdit(this);
	view_->setReadOnly(true);
	view_->setLineWrapMode(QTextEdit::NoWrap);
	view_->setStyleSheet(QStringLiteral("background:#15161a; font-family: monospace;"));
	root->addWidget(view_, 1);

	connect(sessionCombo_, &QComboBox::currentIndexChanged, this, &ErrorLogsPanel::onSessionChanged);
	connect(openFileBtn, &QPushButton::clicked, this, &ErrorLogsPanel::openLogFile);
	connect(openFolderBtn, &QPushButton::clicked, this, &ErrorLogsPanel::openLogFolder);
	connect(refreshBtn, &QPushButton::clicked, this, &ErrorLogsPanel::refresh);
	connect(copyBtn, &QPushButton::clicked, this, &ErrorLogsPanel::copyToClipboard);
	connect(clearBtn, &QPushButton::clicked, this, &ErrorLogsPanel::clearLogs);
	connect(errorCheck_, &QCheckBox::toggled, this, &ErrorLogsPanel::applyFilter);
	connect(warningCheck_, &QCheckBox::toggled, this, &ErrorLogsPanel::applyFilter);
	connect(infoCheck_, &QCheckBox::toggled, this, &ErrorLogsPanel::applyFilter);
}

void ErrorLogsPanel::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	refresh();
}

void ErrorLogsPanel::refresh()
{
	const QString previous = sessionCombo_->currentData().toString();
	{
		QSignalBlocker block(sessionCombo_);
		sessionCombo_->clear();
		const std::vector<std::string> files = Logger::instance().sessionFiles();
		const QString current = QString::fromStdString(Logger::instance().sessionFilePath());
		for (const std::string &f : files) {
			const QString path = QString::fromStdString(f);
			QString label = QFileInfo(path).fileName();
			if (path == current)
				label += QStringLiteral("  (current)");
			sessionCombo_->addItem(label, path);
		}
		// Restore prior selection, else pick the newest (current) session.
		int idx = previous.isEmpty() ? 0 : sessionCombo_->findData(previous);
		sessionCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	loadSelectedFile();
}

void ErrorLogsPanel::onSessionChanged()
{
	loadSelectedFile();
}

void ErrorLogsPanel::loadSelectedFile()
{
	lines_.clear();
	const QString path = sessionCombo_->currentData().toString();
	pathLabel_->setText(path);
	if (!path.isEmpty()) {
		QFile f(path);
		if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			QTextStream in(&f);
			while (!in.atEnd())
				lines_ << in.readLine();
		}
	}
	applyFilter();
}

void ErrorLogsPanel::applyFilter()
{
	const bool showErr = errorCheck_->isChecked();
	const bool showWarn = warningCheck_->isChecked();
	const bool showInfo = infoCheck_->isChecked();

	QString html = QStringLiteral("<pre style='margin:0'>");
	QString lastLevel = QStringLiteral("INFO");
	int lastErrorAnchor = -1;
	int anchor = 0;

	for (const QString &line : lines_) {
		QString level = levelOf(line);
		if (level.isEmpty())
			level = lastLevel; // continuation line inherits the previous level
		else
			lastLevel = level;

		const bool visible = (level == QStringLiteral("ERROR") && showErr) ||
				     (level == QStringLiteral("WARNING") && showWarn) ||
				     ((level == QStringLiteral("INFO") || level == QStringLiteral("DEBUG")) &&
				      showInfo);
		if (!visible)
			continue;

		QString escaped = line.toHtmlEscaped();
		if (level == QStringLiteral("ERROR"))
			lastErrorAnchor = anchor;
		html += QStringLiteral("<a name='a%1'></a><span style='color:%2'>%3</span>\n")
				.arg(anchor)
				.arg(colorFor(level).name(), escaped);
		++anchor;
	}
	html += QStringLiteral("</pre>");
	view_->setHtml(html);

	// Auto-scroll to the most recent error.
	if (lastErrorAnchor >= 0)
		view_->scrollToAnchor(QStringLiteral("a%1").arg(lastErrorAnchor));
	else
		view_->moveCursor(QTextCursor::End);
}

void ErrorLogsPanel::openLogFile()
{
	const QString path = sessionCombo_->currentData().toString();
	if (!path.isEmpty())
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ErrorLogsPanel::openLogFolder()
{
	const QString dir = QString::fromStdString(Logger::instance().logDir());
	if (!dir.isEmpty())
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void ErrorLogsPanel::copyToClipboard()
{
	QApplication::clipboard()->setText(view_->toPlainText());
}

void ErrorLogsPanel::clearLogs()
{
	if (QMessageBox::question(this, QStringLiteral("Clear Logs"),
				  QStringLiteral("Delete all stored log files except the current session?")) !=
	    QMessageBox::Yes)
		return;
	Logger::instance().clearAll();
	refresh();
}

} // namespace harpia
