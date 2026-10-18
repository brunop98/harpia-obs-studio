#pragma once

// Whose sound "PC Audio" records: every app, only some, or every app but one
// (core/PcAudioApps.hpp has the rules). Opened from the button beside the PC
// Audio checkbox; one choice for every preset, kept in the app's settings.

#include "core/PcAudioApps.hpp"
#include "editor/SystemAudioCapture.hpp"

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QDialogButtonBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QRadioButton;
class QSettings;
class QStackedWidget;

namespace harpia {

// The choice as saved, with the names of the apps in it ("spotify.exe" ->
// "Spotify"), so the button can say who without asking Windows.
namespace pc_audio_settings {
PcAudioChoice load(QSettings &s, QHash<QString, QString> *names = nullptr);
void save(QSettings &s, const PcAudioChoice &choice, const QHash<QString, QString> &names);
} // namespace pc_audio_settings

// What the button says: "All apps", "Only Google Chrome", "Only 3 apps",
// "All but Spotify". `names` gives an app's own name by its file name; the
// file name without ".exe" otherwise. Without `available`, always "All apps".
QString pcAudioSummary(const PcAudioChoice &c, bool available, const QHash<QString, QString> &names);
// The names of the apps the choice records, or leaves out: for the tooltip.
QStringList pcAudioAppNames(const PcAudioChoice &c, const QHash<QString, QString> &names);

class PcAudioAppsDialog : public QDialog {
public:
	using AppLister = std::function<QVector<SystemAudioApp>()>;

	// `available`: whether this system can single apps out at all.
	// `listApps`: the running apps (system_audio::runningApps), asked again by
	// Refresh. `names`: names remembered for apps not running now.
	PcAudioAppsDialog(const PcAudioChoice &choice, bool available, AppLister listApps,
			  const QHash<QString, QString> &names, QWidget *parent = nullptr);

	PcAudioChoice choice() const;
	// Every app's name it has seen, the remembered ones included.
	const QHash<QString, QString> &names() const { return names_; }

	// For tests.
	QRadioButton *modeButton(PcAudioFrom f) const;
	QListWidget *onlyList() const { return onlyList_; }
	QListWidget *exceptList() const { return exceptList_; }
	QString statusText() const;
	bool okEnabled() const;
	void refreshForTest() { fillLists(); }

private:
	void fillLists();
	void syncState();
	PcAudioFrom mode() const;
	QStringList checkedOnly() const;
	QString selectedExcept() const;

	bool available_ = false;
	AppLister listApps_;
	QHash<QString, QString> names_; // lower-case exe -> name
	PcAudioChoice start_;           // what to tick on the first fill
	bool filled_ = false;

	QRadioButton *allRadio_ = nullptr;
	QRadioButton *onlyRadio_ = nullptr;
	QRadioButton *exceptRadio_ = nullptr;
	QStackedWidget *pages_ = nullptr;
	QListWidget *onlyList_ = nullptr;
	QListWidget *exceptList_ = nullptr;
	QLabel *status_ = nullptr;
	QDialogButtonBox *buttons_ = nullptr;
};

} // namespace harpia
