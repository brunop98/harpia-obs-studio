#pragma once

#include <QDialog>
#include <QStringList>
#include <QUrl>
#include <functional>

class QLabel;
class QPushButton;

namespace harpia {

// Providers stay separate from the dialog so selecting a method can be tested
// without starting an external app, opening a browser or using real settings.
struct PhoneSharingServices {
	std::function<QString()> findLocalSend;
	std::function<QString(QWidget *)> chooseLocalSend;
	std::function<void(const QString &)> rememberLocalSend;
	std::function<bool(const QString &, const QStringList &, QString *)> startLocalSend;
	std::function<bool()> airDropAvailable;
	std::function<bool(const QStringList &, QObject *, std::function<void(const QString &)>, QString *)> startAirDrop;
	std::function<bool(const QUrl &)> openUrl;
};

class PhoneSharingDialog : public QDialog {
public:
	explicit PhoneSharingDialog(const QStringList &files, QWidget *parent = nullptr);
	PhoneSharingDialog(const QStringList &files, PhoneSharingServices services, QWidget *parent = nullptr);
	static void runModal(const QStringList &files, QWidget *parent = nullptr);

private:
	void refreshLocalSend();
	void updateAvailability();
	bool filesReady(QString *error = nullptr) const;
	void setStatus(const QString &message, bool error = false);
	void sendLocalSend();
	void sendAirDrop();

	QStringList files_;
	PhoneSharingServices services_;
	QString localSend_;
	QLabel *localSendInfo_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *localSendButton_ = nullptr;
	QPushButton *airDropButton_ = nullptr;
	bool airDropInFlight_ = false;
};

} // namespace harpia
