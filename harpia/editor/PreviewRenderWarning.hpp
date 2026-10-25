#pragma once

// Per-editor, nonmodal render feedback. Diagnostics arrive synchronously from
// that editor's compositor; export workers use their own RenderError instead.
#include "../ui/EditorLog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

namespace harpia {

class PreviewRenderWarning final : public QWidget {
public:
	explicit PreviewRenderWarning(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("previewRenderWarning"));
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(8, 6, 8, 6);
		label_ = new QLabel(this);
		label_->setObjectName(QStringLiteral("previewRenderWarningText"));
		label_->setTextFormat(Qt::PlainText);
		label_->setWordWrap(true);
		label_->setStyleSheet(QStringLiteral("color:#f5ce83;"));
		layout->addWidget(label_, 1);
		auto *details = new QPushButton(QStringLiteral("Details…"), this);
		details->setObjectName(QStringLiteral("previewRenderWarningDetails"));
		layout->addWidget(details);
		connect(details, &QPushButton::clicked, this, [this] { showDetails(); });
		hide();
	}

	void setFailure(const QString &key, const QString &summary, const QString &report)
	{
		if (key != currentKey_) {
			currentKey_ = key;
			report_ = report;
			label_->setText(QStringLiteral("Preview could not render: %1").arg(summary));
			label_->setToolTip(report);
			if (detailsText_)
				detailsText_->setPlainText(report_);
		}
		// A broken shader can be evaluated at every playback tick. Record one
		// report per distinct problem, with a bounded per-window history.
		if (!reported_.contains(key)) {
			if (reported_.size() >= 128)
				reported_.remove(reportOrder_.takeFirst());
			reported_.insert(key);
			reportOrder_.append(key);
			editorLog(EditorLogLevel::Warning, QStringLiteral("Preview render failed"), report_);
		}
		show();
	}

	void clearFailure()
	{
		currentKey_.clear();
		hide();
	}

	void resetHistory()
	{
		clearFailure();
		reported_.clear();
		reportOrder_.clear();
	}

private:
	void showDetails()
	{
		if (!details_) {
			auto *dialog = new QDialog(this);
			dialog->setAttribute(Qt::WA_DeleteOnClose);
			dialog->setWindowTitle(QStringLiteral("Preview render details"));
			dialog->resize(640, 360);
			auto *layout = new QVBoxLayout(dialog);
			detailsText_ = new QPlainTextEdit(dialog);
			detailsText_->setReadOnly(true);
			layout->addWidget(detailsText_);
			auto *buttons = new QHBoxLayout;
			auto *copy = new QPushButton(QStringLiteral("Copy report"), dialog);
			connect(copy, &QPushButton::clicked, dialog, [this] {
				QApplication::clipboard()->setText(detailsText_->toPlainText());
			});
			buttons->addWidget(copy);
			buttons->addStretch();
			auto *close = new QPushButton(QStringLiteral("Close"), dialog);
			connect(close, &QPushButton::clicked, dialog, &QDialog::close);
			buttons->addWidget(close);
			layout->addLayout(buttons);
			details_ = dialog;
		}
		detailsText_->setPlainText(report_);
		details_->show(); // modeless, and only in response to the Details button
		details_->raise();
		details_->activateWindow();
	}

	QLabel *label_ = nullptr;
	QPointer<QDialog> details_;
	QPointer<QPlainTextEdit> detailsText_;
	QString currentKey_, report_;
	QSet<QString> reported_;
	QStringList reportOrder_;
};

} // namespace harpia
