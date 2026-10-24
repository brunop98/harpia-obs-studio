// Repeated Inspector synchronization must not rebuild an unchanged style list
// or repolish unchanged colour buttons. Freshly edited preset files still take
// effect, including edits that keep the same preset names.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TextStyleJson.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

class StyleChanges : public QObject {
public:
	int changes = 0;
protected:
	bool eventFilter(QObject *, QEvent *event) override
	{
		if (event->type() == QEvent::StyleChange)
			++changes;
		return false;
	}
};

static bool writePresets(const QString &path, const QJsonObject &styles)
{
	QSaveFile file(path);
	return file.open(QIODevice::WriteOnly) &&
	       file.write(QJsonDocument(styles).toJson()) >= 0 && file.commit();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir settings;
	qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
	qputenv("XDG_DATA_HOME", (settings.path() + QStringLiteral("/data")).toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaInspectorReuse"));
	QCoreApplication::setApplicationName(QStringLiteral("Test"));
	VideoEditorWindow window{QString()};
	for (auto *button : window.findChildren<QPushButton *>()) {
		if (button->text() == QStringLiteral("Full Editing"))
			button->click();
		if (button->text() == QStringLiteral("Inspector") && button->isCheckable())
			button->setChecked(true);
	}
	window.show();
	QApplication::processEvents();
	auto *timeline = window.findChild<TimelineView *>();
	auto *picker = window.findChild<QComboBox *>(QStringLiteral("textStylePreset"));
	auto *highlight = window.findChild<QPushButton *>(QStringLiteral("captionHighlightColor"));
	auto *remove = window.findChild<QPushButton *>(QStringLiteral("deleteTextStylePreset"));
	check(timeline && picker && highlight && remove, "the production Inspector and style controls are available");
	if (!timeline || !picker || !highlight || !remove)
		return 1;
	const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
		QStringLiteral("/harpia/text-presets.json");
	TlText warm;
	warm.fontPx = 72;
	warm.color = QColor(Qt::yellow);
	QJsonObject styles;
	styles.insert(QStringLiteral("Warm"), textStyleToJson(warm));
	styles.insert(QStringLiteral("Instagram Classic"), textStyleToJson(warm));
	check(writePresets(path, styles), "isolated saved styles can be written atomically");
	TlClip caption;
	caption.type = TlClip::Type::Text;
	caption.srcEndMs = 2000;
	caption.text.text = QStringLiteral("Keep my words");
	ComponentInstance animation;
	animation.typeId = QStringLiteral("harpia.subtitle");
	animation.instanceId = QStringLiteral("subtitle");
	animation.props.insert(QStringLiteral("mode"), 3);
	caption.components.append(animation);
	TlTrack track;
	track.clips = {caption};
	TimelineModel model;
	model.tracks = {track};
	timeline->setModelAndCommit(model);
	timeline->selectClip(0, 0);
	check(picker->findData(QStringLiteral("saved/Warm")) >= 0 &&
	      picker->findData(QStringLiteral("builtin/instagram-classic")) >= 0 &&
	      picker->findText(QStringLiteral("Instagram Classic (Saved)")) >= 0,
	      "fresh styles coexist with built-ins and same-named saved styles");
	picker->setCurrentIndex(picker->findData(QStringLiteral("saved/Warm")));
	int removedRows = 0, insertedRows = 0;
	QObject observer;
	QObject::connect(picker->model(), &QAbstractItemModel::rowsRemoved, &observer, [&]() { ++removedRows; });
	QObject::connect(picker->model(), &QAbstractItemModel::rowsInserted, &observer, [&]() { ++insertedRows; });
	StyleChanges stylesChanged;
	highlight->installEventFilter(&stylesChanged);
	for (int i = 0; i < 100; ++i)
		timeline->selectClip(0, 0);
	check(removedRows == 0 && insertedRows == 0, "100 unchanged Inspector refreshes preserve the existing picker rows");
	check(stylesChanged.changes == 0, "unchanged Inspector colours cause no stylesheet repolishing");
	check(picker->currentData().toString() == QStringLiteral("saved/Warm") && remove->isEnabled(),
	      "stable rows keep the selected style and its Delete state");
	warm.fontPx = 93;
	warm.color = QColor(Qt::cyan);
	styles.insert(QStringLiteral("Warm"), textStyleToJson(warm));
	check(writePresets(path, styles), "a second writer can change a style without renaming it");
	timeline->selectClip(0, 0);
	check(removedRows == 0 && insertedRows == 0, "style-value changes do not need to rebuild identical list entries");
	QMetaObject::invokeMethod(picker, "activated", Qt::DirectConnection, Q_ARG(int, picker->currentIndex()));
	check(timeline->model().tracks[0].clips[0].text.fontPx == 93 &&
	      timeline->model().tracks[0].clips[0].text.color == QColor(Qt::cyan) &&
	      timeline->model().tracks[0].clips[0].text.text == caption.text.text,
	      "applying a stable entry reads the newly edited style and preserves caption content");
	styles.insert(QStringLiteral("New style"), textStyleToJson(warm));
	check(writePresets(path, styles), "an external writer can add a style");
	timeline->selectClip(0, 0);
	check(picker->findData(QStringLiteral("saved/New style")) >= 0 &&
	      picker->currentData().toString() == QStringLiteral("saved/Warm") && removedRows > 0,
	      "a changed name list rebuilds immediately and retains the selected identity");
	styles.remove(QStringLiteral("Warm"));
	check(writePresets(path, styles), "an external writer can remove the selected style");
	timeline->selectClip(0, 0);
	check(picker->findData(QStringLiteral("saved/Warm")) < 0 && picker->currentIndex() == 0 && !remove->isEnabled(),
	      "deleting the selected entry restores the placeholder and disables Delete");
	check(QFile::remove(path), "the saved-style file can be removed externally");
	timeline->selectClip(0, 0);
	check(picker->count() == 7 && picker->findData(QStringLiteral("builtin/tiktok-classic")) >= 0,
	      "a removed file clears custom entries and leaves every built-in available");
	const int removedBefore = removedRows, insertedBefore = insertedRows;
	timeline->selectClip(0, 0);
	check(removedRows == removedBefore && insertedRows == insertedBefore,
	      "repeated refreshes with a missing file also preserve stable rows");
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
