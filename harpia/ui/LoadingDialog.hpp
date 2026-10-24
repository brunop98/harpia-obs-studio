#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTimer;

namespace harpia {

// A loading operation owns the dialog's lifetime. Its updates do not process
// events: the caller keeps the GUI responsive while doing work separately.
// Loading cannot be dismissed halfway through a project/media transaction.
class LoadingDialog : public QDialog {
	Q_OBJECT
public:
	explicit LoadingDialog(const QString &title, QWidget *parent = nullptr);

public slots:
	// A new stage has unknown duration until the caller supplies a real count.
	void setStage(const QString &message, const QString &detail = QString());
	// completed is the number of items already processed, from zero to total.
	void setItemProgress(int completed, int total);
	void finish();
	void accept() override;
	void reject() override;

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	void refreshElapsed();
	void setIndeterminate();

	QLabel *stage_ = nullptr;
	QLineEdit *detail_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QLabel *elapsed_ = nullptr;
	QTimer *timer_ = nullptr;
	QElapsedTimer clock_;
	bool finished_ = false;
};

} // namespace harpia
