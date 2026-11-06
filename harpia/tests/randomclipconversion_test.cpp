#include <QtWidgets>
#include <QSignalSpy>
#include <QTest>
#define private public
#include "editor/timeline/TimelineView.hpp"
#undef private
#include <cstdio>

using namespace harpia;
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	int failures = 0;
	const auto check = [&](bool pass, const char *why) {
		std::printf("%s %s\n", pass ? "PASS" : "FAIL", why);
		failures += !pass;
	};
	TimelineView view;
	view.resize(900, 400);
	view.show();
	TlClip original;
	original.sourceId = 4;
	original.srcStartMs = 1234;
	original.srcEndMs = 3234;
	original.outStartMs = 1000;
	original.posX = 0.3;
	original.speed = 1.7;
	original.crop = QRect(2, 3, 80, 90);
	TlKeyframe key;
	key.tMs = 50;
	original.keys = {key};
	TlTrack track;
	track.clips = {original};
	TimelineModel model;
	model.tracks = {track};
	view.setModel(model);
	QSignalSpy commits(&view, &TimelineView::editCommitted);
	QSignalSpy inspect(&view, &TimelineView::inspectClipRequested);
	const auto open = [&](bool enabled) {
		bool found = false;
		QTimer driver;
		QObject::connect(&driver, &QTimer::timeout, [&]() {
			auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
			if (!menu || menu->objectName() != QStringLiteral("timelineClipMenu")) {
				return;
			}
			for (QAction *action : menu->actions()) {
				if (action->text() == QStringLiteral("Convert to Random Clip")) {
					found = true;
					check(action->isEnabled() == enabled,
					      "conversion action follows the track lock");
					if (enabled) {
						menu->setActiveAction(action);
						QTest::keyClick(menu, Qt::Key_Return);
					} else {
						menu->close();
					}
					return;
				}
			}
			menu->close();
		});
		driver.start(10);
		view.showClipMenu(0, 0, view.mapToGlobal(QPoint(150, 150)), 1300);
		check(found, "Convert to Random Clip is directly in the clip's right-click menu");
	};
	open(true);
	const auto converted = view.model().tracks[0].clips[0];
	check(converted.clipOptions.size() == 1 && converted.clipShown == 0 &&
		      converted.clipOptionAt(0).sourceId == 4 && converted.clipOptionAt(0).srcStartMs == 1234,
	      "conversion keeps the original footage and timestamp as option 1");
	TlClip restored = converted;
	restored.clipOptions.clear();
	check(restored == original, "conversion retains every original clip field, framing and keyframe");
	check(commits.size() == 1 && inspect.size() == 1, "conversion is one committed edit and opens the inspector");
	model.tracks[0].locked = true;
	view.setModel(model);
	open(false);
	check(view.model() == model && commits.size() == 1, "locked clips cannot be converted");
	return failures ? 1 : 0;
}
