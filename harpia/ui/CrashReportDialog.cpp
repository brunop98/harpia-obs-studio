#include "ui/CrashReportDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace harpia {

QString CrashReportDialog::summaryOf(const QString &report)
{
	QStringList lines;
	for (const QString &line : report.split(QLatin1Char('\n'))) {
		if (line.trimmed().startsWith(QLatin1String("Stack")))
			break;
		lines << line;
	}
	while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
		lines.removeLast();
	while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
		lines.removeFirst();
	// A report with no stack line is summarised by its first few lines.
	if (lines.size() > 8)
		lines = lines.mid(0, 8);
	return lines.join(QLatin1Char('\n'));
}

CrashReportDialog::CrashReportDialog(const QString &report, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Harpia crashed last time"));
	setSizeGripEnabled(true);

	auto *top = new QHBoxLayout;
	auto *icon = new QLabel(this);
	const int iconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, this);
	icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(iconSize, iconSize));
	icon->setAlignment(Qt::AlignTop);
	top->addWidget(icon);
	auto *words = new QVBoxLayout;
	auto *intro = new QLabel(QStringLiteral("The previous session ended in a crash. The description below was "
						"captured as it happened; the session log has the full lead-up."),
				 this);
	intro->setWordWrap(true);
	words->addWidget(intro);
	auto *summary = new QLabel(summaryOf(report), this);
	summary->setWordWrap(true);
	summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
	QFont bold = summary->font();
	bold.setBold(true);
	summary->setFont(bold);
	words->addWidget(summary);
	top->addLayout(words, 1);

	box_ = new QPlainTextEdit(report, this);
	box_->setReadOnly(true);
	box_->setLineWrapMode(QPlainTextEdit::NoWrap);
	box_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

	auto *buttons = new QDialogButtonBox(this);
	copy_ = buttons->addButton(QStringLiteral("Copy report"), QDialogButtonBox::ActionRole);
	QPushButton *logs = buttons->addButton(QStringLiteral("Open logs folder"), QDialogButtonBox::ActionRole);
	QPushButton *close = buttons->addButton(QDialogButtonBox::Close);
	close->setDefault(true);
	connect(copy_, &QPushButton::clicked, this, [this, report]() {
		QApplication::clipboard()->setText(report);
		copy_->setText(QStringLiteral("Copied"));
		QTimer::singleShot(1500, this, [this]() { copy_->setText(QStringLiteral("Copy report")); });
	});
	connect(logs, &QPushButton::clicked, this, [this]() {
		openLogs_ = true;
		accept();
	});
	connect(close, &QPushButton::clicked, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(top);
	layout->addWidget(box_, 1);
	layout->addWidget(buttons);

	// Room for the report, but never more than most of the screen: the box
	// scrolls the rest, and the buttons stay on screen.
	const QScreen *screen = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
	const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
	resize(qMin(760, avail.width() * 9 / 10), qMin(560, avail.height() * 3 / 4));
	setMaximumHeight(avail.height());
}

} // namespace harpia
