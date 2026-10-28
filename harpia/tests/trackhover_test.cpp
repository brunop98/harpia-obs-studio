// Hover must keep carrying the right source after many videos and recover on
// the very first move after an interrupted cut/reorder/trim gesture.
#include <QtWidgets>
#include <QSignalSpy>
#define private public
#include "editor/TrackEditor.hpp"
#undef private

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const QString &description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description.toUtf8().constData());
	failures += !condition;
}
void mouse(TrackEditor &track, QEvent::Type type, QPoint at, Qt::MouseButton button,
	   Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
	QMouseEvent event(type, QPointF(at), track.mapToGlobal(at), button, buttons, modifiers);
	QApplication::sendEvent(&track, &event);
}
QVector<CutSegment> cuts()
{
	QVector<CutSegment> result;
	for (int id = 1; id <= 40; ++id)
		result.push_back({600000 + id * 1000, 606000 + id * 1000, id == 1 ? 1.3 : 1.0, id});
	return result;
}
void setup(TrackEditor &track)
{
	track.resize(1200, 220);
	track.setSegments(cuts());
	track.setActiveSource(7);
	track.setDuration(1800000);
	track.show();
	QApplication::processEvents();
}
QPoint source(const TrackEditor &track)
{
	return QPoint(600, track.sourceRect().center().y());
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	TrackEditor track;
	setup(track);
	QSignalSpy hovered(&track, &TrackEditor::hoverScrub);
	QSignalSpy scrubbed(&track, &TrackEditor::scrubSource);
	QSignalSpy changed(&track, &TrackEditor::segmentsChanged);
	int lastSource = -1;
	qint64 lastTime = -1;
	QObject::connect(&track, &TrackEditor::hoverScrub, [&](qint64 time) {
		lastSource = track.scrubSourceId();
		lastTime = time;
	});
	bool sourceIds = true;
	for (int id = 1; id <= 40; ++id) {
		track.setActiveSource(id);
		track.setDuration(1800000);
		track.zoom_ = track.zoomTarget_ = 7.5;
		track.viewStart_ = track.viewTarget_ = 600000;
		mouse(track, QEvent::MouseMove, source(track), Qt::NoButton, Qt::NoButton);
		sourceIds &= lastSource == id && lastTime == 720000;
	}
	check(sourceIds && hovered.size() == 40,
	      QStringLiteral("forty long sources emit their own IDs even at the same twelve-minute hover time"));
	bool outputIds = true;
	for (int i = 0; i < track.segs_.size(); ++i) {
		mouse(track, QEvent::MouseMove, track.segmentRects()[i].center(), Qt::NoButton, Qt::NoButton);
		outputIds &= lastSource == track.segs_[i].sourceId && lastTime >= track.segs_[i].srcStartMs &&
			lastTime <= track.segs_[i].srcEndMs;
	}
	check(outputIds && hovered.size() == 80,
	      QStringLiteral("hovering every Output cut resolves its own source among forty videos"));

	setup(track);
	QPoint body = track.segmentRects()[0].center();
	mouse(track, QEvent::MouseButtonPress, body, Qt::LeftButton, Qt::LeftButton);
	mouse(track, QEvent::MouseMove, body + QPoint(80, 0), Qt::NoButton, Qt::LeftButton);
	const auto selected = track.selectedIndices();
	changed.clear();
	hovered.clear();
	scrubbed.clear();
	mouse(track, QEvent::MouseMove, source(track), Qt::NoButton, Qt::NoButton);
	check(hovered.size() == 1 && scrubbed.isEmpty() && lastSource == 7 &&
	      track.mode_ == TrackEditor::Mode::None && track.segments() == cuts() &&
	      track.selectedIndices() == selected && changed.isEmpty(),
	      QStringLiteral("the first no-button move after a lost reorder release resumes hover without moving a cut"));

	for (QEvent::Type type : {QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
		setup(track);
		track.setCursor(Qt::SizeAllCursor);
		const QPoint at = source(track);
		mouse(track, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
		mouse(track, QEvent::MouseMove, at + QPoint(120, 0), Qt::NoButton, Qt::LeftButton);
		changed.clear();
		QEvent canceled(type);
		QApplication::sendEvent(&track, &canceled);
		const bool cursor = track.testAttribute(Qt::WA_SetCursor) && track.cursor().shape() == Qt::SizeAllCursor;
		mouse(track, QEvent::MouseButtonRelease, at + QPoint(120, 0), Qt::LeftButton, Qt::NoButton);
		hovered.clear();
		mouse(track, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
		check(cursor && hovered.size() == 1 && track.mode_ == TrackEditor::Mode::None &&
		      track.segments() == cuts() && changed.isEmpty(),
		      QStringLiteral("interrupting source creation (%1) discards it, restores the cursor and resumes hover").arg(int(type)));
	}

	for (bool left : {false, true})
		for (bool retime : {false, true})
			for (QEvent::Type type : {QEvent::None, QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
				setup(track);
				track.setCursor(Qt::SizeAllCursor);
				const QRect rect = track.segmentRects()[0];
				const QPoint at(left ? rect.left() + 1 : rect.right() - 1, rect.center().y());
				const auto modifiers = retime ? Qt::ShiftModifier : Qt::NoModifier;
				mouse(track, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
				mouse(track, QEvent::MouseMove, at + QPoint(8, 0), Qt::NoButton, Qt::LeftButton, modifiers);
				const bool live = !(track.segments()[0] == cuts()[0]);
				const auto selection = track.selectedIndices();
				changed.clear();
				hovered.clear();
				if (type == QEvent::None) {
					mouse(track, QEvent::MouseMove, source(track), Qt::NoButton, Qt::NoButton);
				} else {
					QEvent canceled(type);
					QApplication::sendEvent(&track, &canceled);
					mouse(track, QEvent::MouseMove, source(track), Qt::NoButton, Qt::NoButton);
				}
				check(live && track.segments() == cuts() && changed.isEmpty() && hovered.size() == 1 &&
				      track.selectedIndices() == selection && track.mode_ == TrackEditor::Mode::None,
				      QStringLiteral("interrupted %1 edge %2 (%3) restores exact bounds/speed and resumes hover without an edit")
					.arg(left ? QStringLiteral("left") : QStringLiteral("right"),
					     retime ? QStringLiteral("retime") : QStringLiteral("trim")).arg(int(type)));
			}

	for (int replacement = 0; replacement < 3; ++replacement) {
		setup(track);
		const QRect rect = track.segmentRects()[0];
		const QPoint at(rect.right() - 1, rect.center().y());
		mouse(track, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
		mouse(track, QEvent::MouseMove, at + QPoint(10, 0), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
		changed.clear();
		QVector<CutSegment> expected = cuts();
		if (replacement == 0)
			track.setActiveSource(12);
		else if (replacement == 1)
			track.setDuration(1200000);
		else {
			expected[0].sourceId = 42;
			track.setSegments(expected);
		}
		hovered.clear();
		mouse(track, QEvent::MouseMove, source(track), Qt::NoButton, Qt::NoButton);
		check(track.mode_ == TrackEditor::Mode::None && track.segments() == expected && changed.isEmpty() &&
		      hovered.size() == 1 && lastSource == (replacement == 0 ? 12 : 7),
		      QStringLiteral("source/duration/segment replacement (%1) cancels pending edits before installing new state").arg(replacement));
	}

	setup(track);
	const QPoint from(100, track.sourceRect().center().y()), to(250, from.y());
	mouse(track, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
	mouse(track, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
	changed.clear();
	mouse(track, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
	check(track.segments().size() == 41 && track.segments().last().sourceId == 7 && changed.size() == 1,
	      QStringLiteral("a completed source drag still creates its cut and commits exactly once"));
	for (bool retime : {false, true}) {
		setup(track);
		const QRect rect = track.segmentRects()[0];
		const QPoint at(rect.right() - 1, rect.center().y());
		mouse(track, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
		mouse(track, QEvent::MouseMove, at + QPoint(8, 0), Qt::NoButton, Qt::LeftButton,
			retime ? Qt::ShiftModifier : Qt::NoModifier);
		changed.clear();
		mouse(track, QEvent::MouseButtonRelease, at + QPoint(8, 0), Qt::LeftButton, Qt::NoButton);
		check(!(track.segments()[0] == cuts()[0]) && changed.size() == 1 && track.mode_ == TrackEditor::Mode::None,
		      QStringLiteral("a completed edge %1 retains its edit and commits once")
			.arg(retime ? QStringLiteral("retime") : QStringLiteral("trim")));
	}
	{
		QWidget parent;
		parent.setCursor(Qt::CrossCursor);
		TrackEditor child(&parent);
		setup(child);
		child.unsetCursor();
		body = child.segmentRects()[0].center();
		mouse(child, QEvent::MouseButtonPress, body, Qt::LeftButton, Qt::LeftButton);
		mouse(child, QEvent::MouseMove, body + QPoint(80, 0), Qt::NoButton, Qt::LeftButton);
		parent.setCursor(Qt::PointingHandCursor);
		QEvent canceled(QEvent::UngrabMouse);
		QApplication::sendEvent(&child, &canceled);
		check(!child.testAttribute(Qt::WA_SetCursor) && child.cursor().shape() == Qt::PointingHandCursor,
		      QStringLiteral("canceling a left edit restores inherited cursor behavior"));
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
