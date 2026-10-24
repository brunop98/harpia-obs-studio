#pragma once

#include "SilenceTrim.hpp"
#include "TimelineAudio.hpp"

#include <QDialog>
#include <QMap>
#include <QVector>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class QCheckBox;
class QCloseEvent;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;

namespace harpia {

// Analyze audio without changing the project, then review every proposed cut.
// Only acceptance hands the checked ranges back to the editor. The editor
// ripples all tracks together so a quiet pause cannot separate sound and video.
class SilenceTrimDialog : public QDialog {
	Q_OBJECT
public:
	SilenceTrimDialog(const TimelineModel &timeline, const TimelineAudio::SourceLookup &pathFor,
			  const QVector<QPair<int, int>> &selected, QWidget *parent = nullptr);
	~SilenceTrimDialog() override;

	QVector<TlSpan> selectedCuts() const;

public slots:
	void accept() override;
	void reject() override;

signals:
	void previewRequested(const QVector<TlSpan> &cuts);
	void previewStopped();

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	struct Analysis {
		std::shared_ptr<const std::vector<float>> pcm;
		QVector<TlSpan> cuts;
		QString error;
		bool hasAudio = false;
		bool hasSignal = false;
	};

	void buildUi(bool canSelect);
	void startAnalysis();
	void completeAnalysis(Analysis result);
	void optionsChanged();
	void updateSummary();
	void stopPreview();
	void setBusy(bool busy);
	TimelineModel analysisModel(bool selectedOnly, QVector<TlSpan> *scopes) const;
	silence_trim::Options options() const;

	TimelineModel timeline_;
	QMap<int, QString> sources_;
	QVector<QPair<int, int>> selected_;
	std::shared_ptr<const std::vector<float>> pcm_;
	QVector<TlSpan> candidates_;
	QString analysisMessage_;
	bool cachedSelectedOnly_ = false;
	bool hasCache_ = false;
	bool busy_ = false;
	bool closing_ = false;
	bool previewing_ = false;
	std::atomic<bool> cancel_{false};
	std::thread worker_;

	QCheckBox *selectedOnly_ = nullptr;
	QDoubleSpinBox *threshold_ = nullptr;
	QDoubleSpinBox *minimum_ = nullptr;
	QDoubleSpinBox *keep_ = nullptr;
	QCheckBox *edges_ = nullptr;
	QListWidget *cuts_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *summary_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QPushButton *analyze_ = nullptr;
	QPushButton *preview_ = nullptr;
	QPushButton *apply_ = nullptr;
	QPushButton *cancelButton_ = nullptr;
	QTimer *optionsTimer_ = nullptr;
};

} // namespace harpia
