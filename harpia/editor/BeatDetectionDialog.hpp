#pragma once

#include "BeatDetector.hpp"
#include "timeline/TimelineModel.hpp"

#include <QDialog>
#include <QVector>

#include <atomic>
#include <thread>

class QCloseEvent;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace harpia {

// Review beats before changing the timeline. The worker owns copied media and
// timing inputs; only the accepted output positions are handed to the editor.
class BeatDetectionDialog : public QDialog {
	Q_OBJECT
public:
	BeatDetectionDialog(const QString &sourcePath, const TlClip &clip, QWidget *parent = nullptr);
	~BeatDetectionDialog() override;

	QVector<qint64> selectedBeatTimes() const;

public slots:
	void accept() override;
	void reject() override;

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	void startAnalysis();
	void completeAnalysis(beat_detection::Result result);
	void updateReview();
	void setBusy(bool busy);
	double playbackSpeed() const;

	QString sourcePath_;
	TlClip clip_;
	QVector<qint64> beatTimes_;
	double sourceBpm_ = 0.0;
	bool busy_ = false;
	bool closing_ = false;
	std::atomic<bool> cancel_{false};
	std::thread worker_;

	QLabel *status_ = nullptr;
	QLabel *summary_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QComboBox *density_ = nullptr;
	QListWidget *candidates_ = nullptr;
	QPushButton *apply_ = nullptr;
	QPushButton *cancelButton_ = nullptr;
};

} // namespace harpia
