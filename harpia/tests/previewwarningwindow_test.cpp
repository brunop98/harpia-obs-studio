// Drive the real preview compositor and warning UI through timeline signals.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "ui/EditorLog.hpp"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
QStringList logged;
void check(bool value, const char *message)
{
	std::printf("  %s %s\n", value ? "PASS" : "FAIL", message);
	failures += !value;
}
void capture(EditorLogLevel level, const QString &report)
{
	if (level == EditorLogLevel::Warning && report.contains(QStringLiteral("Preview render failed")))
		logged.append(report);
}
TimelineModel fixture(bool enabled)
{
	TimelineModel result;
	TlTrack track;
	track.kind = TlTrack::Kind::Effect;
	track.name = QStringLiteral("Missing shader track");
	TlClip clip;
	clip.type = TlClip::Type::Effect;
	clip.srcEndMs = 1000;
	ComponentInstance shader;
	shader.typeId = QStringLiteral("harpia.shader.portable-missing");
	shader.instanceId = QStringLiteral("uninstalled-effect");
	shader.enabled = enabled;
	clip.components.append(shader);
	track.clips.append(clip);
	result.tracks.append(track);
	return result;
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QStandardPaths::setTestModeEnabled(true);
	QTemporaryDir settings;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
	const auto original = editorLogSink();
	editorLogSink() = capture;
	VideoEditorWindow window{QString()};
	window.resize(1100, 800);
	window.show();
	for (auto *button : window.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Full Editing"))
			button->click();
	QTest::qWait(30);
	auto *timeline = window.findChild<TimelineView *>();
	auto *banner = window.findChild<QWidget *>(QStringLiteral("previewRenderWarning"));
	auto *label = window.findChild<QLabel *>(QStringLiteral("previewRenderWarningText"));
	check(timeline && banner && label, "the real editor exposes nonmodal preview feedback beside its canvas");
	if (!timeline || !banner || !label)
		return 1;
	timeline->setModelAndCommit(fixture(true));
	for (int frame = 0; frame < 20; ++frame)
		timeline->scrub(100 + frame * 20);
	QTest::qWait(30);
	check(banner->isVisible() && label->text().contains(QStringLiteral("portable-missing")) &&
		label->text().contains(QStringLiteral(".frag")), "a missing custom shader shows its name and a recovery action during preview");
	check(logged.size() == 1 && logged.first().contains(QStringLiteral("Track: 1 (Missing shader track)")) &&
		logged.first().contains(QStringLiteral("Clip: 1")), "repeated actual frame failures produce one scoped log with track and clip context");
	check(!QApplication::activeModalWidget(), "timeline scrubbing continues without a modal warning");
	timeline->setModelAndCommit(fixture(false));
	timeline->scrub(500);
	QTest::qWait(30);
	check(banner->isHidden(), "disabling the failing shader clears the real preview warning");
	timeline->setModelAndCommit(fixture(true));
	timeline->scrub(500);
	QTest::qWait(50); // requestPreview coalesces scrubs until its next timer tick
	check(!banner->isHidden() && logged.size() == 1, "returning to the faulty clip restores its banner without repeating the log");
	timeline->setModelAndCommit(fixture(false));
	timeline->scrub(500); // healthy result cached by the preview
	timeline->scrub(500); // return through the cache lookup
	QTest::qWait(50);
	check(banner->isHidden(), "a cached healthy frame also clears a previous failure banner");
	editorLogSink() = original;
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
