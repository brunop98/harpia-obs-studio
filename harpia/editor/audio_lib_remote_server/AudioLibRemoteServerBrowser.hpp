#pragma once

#include "AudioLibRemoteServerSound.hpp"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkReply;
class QPushButton;
class QTreeWidget;
class QTimer;
class QVBoxLayout;

namespace harpia {
class AudioPreview;

class AudioLibRemoteServerBrowser : public QWidget {
	Q_OBJECT
public:
	explicit AudioLibRemoteServerBrowser(const QString &action, bool draggable, QWidget *parent);
	~AudioLibRemoteServerBrowser() override;

	static QString chooseSound(QWidget *parent, const QString &action);
	static QString importSound(QWidget *parent, const AudioLibRemoteServerSound &sound);
	void stopPreview();
	void focusSearch();

signals:
	void soundChosen(const AudioLibRemoteServerSound &sound);
	void previewStarted();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void buildFilters(QVBoxLayout *layout, QPushButton *toggle);
	void editConnection();
	void scheduleSearch();
	void searchPage(int offset);
	void cancelSearch();
	void updateActions();
	void setErrorReport(const QString &report);
	void togglePreview();
	AudioLibRemoteServerSound selectedSound() const;

	QLineEdit *query_ = nullptr;
	QLineEdit *nameFilter_ = nullptr;
	QLineEdit *pathFilter_ = nullptr;
	QComboBox *extensionFilter_ = nullptr;
	QLineEdit *minDurationFilter_ = nullptr;
	QLineEdit *maxDurationFilter_ = nullptr;
	QCheckBox *playableFilter_ = nullptr;
	QComboBox *sort_ = nullptr;
	QComboBox *direction_ = nullptr;
	QTreeWidget *results_ = nullptr;
	QPushButton *previous_ = nullptr;
	QPushButton *next_ = nullptr;
	QPushButton *previewButton_ = nullptr;
	QPushButton *copyErrorDetails_ = nullptr;
	QPushButton *action_ = nullptr;
	QLabel *status_ = nullptr;
	QTimer *searchDelay_ = nullptr;
	QTimer *previewTimer_ = nullptr;
	AudioPreview *preview_ = nullptr;
	QPointer<QNetworkReply> searchReply_;
	QPointer<QNetworkReply> previewReply_;
	QString pageStatus_;
	QString errorReport_;
	int offset_ = 0;
	int total_ = 0;
	quint64 previewGeneration_ = 0;
	bool previewing_ = false;
};

} // namespace harpia
