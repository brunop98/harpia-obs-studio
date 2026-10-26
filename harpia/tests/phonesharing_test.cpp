#include "ui/PhoneSharingDialog.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

template<typename T> T *named(QWidget &dialog, const char *name)
{
	T *result = dialog.findChild<T *>(QString::fromLatin1(name));
	if (!result) {
		std::fprintf(stderr, "Missing dialog control: %s\n", name);
		std::exit(1);
	}
	return result;
}

bool writeFile(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write("rendered video bytes") == 20;
}

struct Providers {
	QString found;
	QString chosen;
	QString remembered;
	QString launchedApplication;
	QStringList handedFiles;
	QList<QUrl> openedUrls;
	int launches = 0;
	int remembers = 0;
	int airDropStarts = 0;
	QObject *airDropReceiver = nullptr;
	bool browserWorks = true;
	bool localSendWorks = true;
	bool airDropAvailable = false;
	bool airDropWorks = true;
	std::function<void(const QString &)> airDropCompletion;

	PhoneSharingServices services()
	{
		PhoneSharingServices value;
		value.findLocalSend = [this]() { return found; };
		value.chooseLocalSend = [this](QWidget *) { return chosen; };
		value.rememberLocalSend = [this](const QString &path) {
			remembered = found = path;
			++remembers;
		};
		value.startLocalSend = [this](const QString &application, const QStringList &files, QString *error) {
			++launches;
			launchedApplication = application;
			handedFiles = files;
			if (!localSendWorks && error)
				*error = QStringLiteral("The selected LocalSend application could not start.");
			return localSendWorks;
		};
		value.airDropAvailable = [this]() { return airDropAvailable; };
		value.startAirDrop = [this](const QStringList &files, QObject *receiver,
			std::function<void(const QString &)> completion, QString *error) {
			++airDropStarts;
			handedFiles = files;
			airDropReceiver = receiver;
			airDropCompletion = std::move(completion);
			if (!airDropWorks && error)
				*error = QStringLiteral("AirDrop is unavailable on this Mac.");
			return airDropWorks;
		};
		value.openUrl = [this](const QUrl &url) {
			openedUrls.append(url);
			return browserWorks;
		};
		return value;
	}
};
} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir directory;
	check(directory.isValid(), "rendered files use a private temporary directory");
	const QString first = directory.filePath(QStringLiteral("São Paulo 🎬 part 1.mp4"));
	const QString second = directory.filePath(QStringLiteral("Part 2 & final.mp4"));
	check(writeFile(first) && writeFile(second), "two exported file fixtures exist");
	{
		Providers provider;
		PhoneSharingDialog dialog({first}, provider.services());
		dialog.show();
		QApplication::processEvents();
		auto *send = named<QPushButton>(dialog, "sendWithLocalSend");
		auto *recommendation = named<QLabel>(dialog, "localSendRecommendation");
		check(recommendation->text().contains(QStringLiteral("not found")) &&
			recommendation->text().contains(QStringLiteral("Recommended")) &&
			recommendation->text().contains(QStringLiteral("free")) &&
			recommendation->text().contains(QStringLiteral("iPhone")),
			"missing LocalSend recommends installing the free app on both devices");
		check(!send->isEnabled(), "LocalSend handoff is disabled until the optional application is found");
		check(provider.launches == 0 && provider.openedUrls.isEmpty() && provider.remembers == 0,
			"opening the dialog does not launch, install or open a website");
		if (argc > 1)
			check(dialog.grab().save(QString::fromLocal8Bit(argv[1])), "optional recommendation screenshot is saved");
		check(!dialog.findChild<QPushButton *>(QStringLiteral("sendWithAirDrop")),
			"unsupported AirDrop is absent rather than offered as a working method");
		named<QPushButton>(dialog, "downloadLocalSend")->click();
		check(provider.openedUrls == QList<QUrl>{QUrl(QStringLiteral("https://localsend.org/download"))},
			"Get LocalSend opens only the official download page after an explicit click");
		provider.browserWorks = false;
		named<QPushButton>(dialog, "downloadLocalSend")->click();
		check(named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("https://localsend.org/download")),
			"a failed browser launch leaves a copyable download address");
		provider.found = directory.filePath(QStringLiteral("LocalSend portable/localsend_app"));
		named<QPushButton>(dialog, "refreshLocalSend")->click();
		check(send->isEnabled() && recommendation->text().contains(provider.found),
			"Refresh detects an installation and enables the handoff");
		check(provider.launches == 0 && provider.remembers == 0,
			"detecting an installation does not send files or overwrite preferences");
		provider.chosen = directory.filePath(QStringLiteral("My Apps/LocalSend app"));
		named<QPushButton>(dialog, "locateLocalSend")->click();
		check(provider.remembers == 1 && provider.remembered == provider.chosen && provider.launches == 0,
			"Locate remembers the explicitly selected application without launching it");
		provider.chosen.clear();
		named<QPushButton>(dialog, "locateLocalSend")->click();
		check(provider.remembers == 1 && recommendation->text().contains(provider.remembered),
			"canceling Locate retains the previously selected application");
		send->click();
		check(provider.launches == 1 && provider.launchedApplication == provider.remembered &&
			provider.handedFiles == QStringList{first},
			"Open in LocalSend hands the actual exported file to the chosen application");
		check(dialog.isVisible() && named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("Choose your iPhone")) &&
			!named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("completed")),
			"opening LocalSend keeps the dialog open and asks for a destination rather than claiming delivery");
		send->click();
		check(provider.launches == 2 && dialog.isVisible(), "the LocalSend action can be used again without dismissing the result");
		provider.localSendWorks = false;
		send->click();
		check(named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("could not start")) && dialog.isVisible(),
			"LocalSend launch failures remain visible in the dialog");
	}
	{
		Providers provider;
		provider.found = QStringLiteral("/fake/LocalSend");
		const QString relativeFirst = QDir::current().relativeFilePath(first);
		PhoneSharingDialog dialog({relativeFirst, second, first, QString()}, provider.services());
		named<QPushButton>(dialog, "sendWithLocalSend")->click();
		check(provider.handedFiles == QStringList({first, second}),
			"handoff keeps selection order, removes duplicate paths and preserves Unicode and spaces");
		check(named<QLabel>(dialog, "phoneSharingFiles")->text().contains(QStringLiteral("2 files")),
			"the dialog describes the deduplicated multi-file selection");
		QFile::remove(second);
		named<QPushButton>(dialog, "sendWithLocalSend")->click();
		check(provider.launches == 1 && !named<QPushButton>(dialog, "sendWithLocalSend")->isEnabled() &&
			named<QLabel>(dialog, "phoneSharingStatus")->text().contains(second),
			"a file deleted after opening is reported and no partial selection is sent");
		check(writeFile(second), "deleted fixture is restored for AirDrop checks");
	}
	{
		Providers provider;
		provider.found = QStringLiteral("/fake/LocalSend");
		provider.airDropAvailable = true;
		PhoneSharingDialog empty({}, provider.services());
		check(!named<QPushButton>(empty, "sendWithLocalSend")->isEnabled() &&
			!named<QPushButton>(empty, "sendWithAirDrop")->isEnabled() &&
			named<QLabel>(empty, "phoneSharingStatus")->text().contains(QStringLiteral("No files")),
			"an empty selection disables both methods with a useful explanation");
		PhoneSharingDialog missing({directory.filePath(QStringLiteral("missing.mp4"))}, provider.services());
		check(!named<QPushButton>(missing, "sendWithLocalSend")->isEnabled() &&
			!named<QPushButton>(missing, "sendWithAirDrop")->isEnabled(),
			"a missing selected file disables both transfer methods");
		PhoneSharingDialog folder({directory.path()}, provider.services());
		check(!named<QPushButton>(folder, "sendWithLocalSend")->isEnabled(),
			"the rendered-file handoff never sends a whole folder accidentally");
	}
	{
		Providers provider;
		provider.airDropAvailable = true;
		PhoneSharingDialog dialog({first, second}, provider.services());
		dialog.show();
		auto *send = named<QPushButton>(dialog, "sendWithAirDrop");
		send->click();
		check(provider.airDropStarts == 1 && provider.airDropReceiver == qApp && provider.handedFiles == QStringList({first, second}),
			"AirDrop receives the ordered exported files with application lifetime");
		check(named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("Choose your iPhone")) && dialog.isVisible(),
			"starting AirDrop leaves selection and acceptance to the native picker");
		check(!send->isEnabled(), "AirDrop cannot start a second session while the first picker is active");
		send->click();
		check(provider.airDropStarts == 1, "a repeated click does not create duplicate in-flight AirDrop sessions");
		if (provider.airDropCompletion)
			provider.airDropCompletion(QStringLiteral("The iPhone declined the transfer."));
		check(named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("declined")) && send->isEnabled(),
			"an asynchronous AirDrop failure explains the result and permits retry");
		send->click();
		if (provider.airDropCompletion)
			provider.airDropCompletion(QString());
		check(named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("completed")) && send->isEnabled(),
			"only a successful AirDrop callback reports completion and permits another share");
		provider.airDropWorks = false;
		send->click();
		check(named<QLabel>(dialog, "phoneSharingStatus")->text().contains(QStringLiteral("unavailable")) && send->isEnabled(),
			"an immediate AirDrop failure stays actionable rather than leaving the button busy");
	}
	{
		Providers provider;
		provider.airDropAvailable = true;
		auto *dialog = new PhoneSharingDialog({first}, provider.services());
		named<QPushButton>(*dialog, "sendWithAirDrop")->click();
		QPointer<PhoneSharingDialog> guard(dialog);
		delete dialog;
		if (provider.airDropCompletion)
			provider.airDropCompletion(QStringLiteral("A late native transfer result"));
		check(!guard && provider.airDropReceiver == qApp,
			"native completion after closing the dialog safely ignores the destroyed UI");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
