#include "editor/timeline/TimelineView.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QApplication>
#include <QMouseEvent>
#include <cstdio>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why)
{
	std::printf("%s %s\n", pass ? "PASS" : "FAIL", why);
	failures += !pass;
}
static TlClip clip(int id, qint64 start, int group = 0, int link = 0)
{
	TlClip c;
	c.sourceId = id; c.outStartMs = start; c.srcStartMs = 100; c.srcEndMs = 2100;
	c.group = group; c.link = link;
	return c;
}
static void mouse(TimelineView &v, QEvent::Type type, QPoint p, Qt::KeyboardModifiers mods = {})
{
	QMouseEvent e(type, QPointF(p), v.mapToGlobal(QPointF(p)),
		type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
		type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
	QApplication::sendEvent(&v, &e);
}
static void click(TimelineView &v, int t, int c, Qt::KeyboardModifiers mods = {})
{
	const QRect r = v.clipRectForTest(t, c);
	const QPoint p(r.center().x(), r.bottom() - 6);
	mouse(v, QEvent::MouseButtonPress, p, mods);
	mouse(v, QEvent::MouseButtonRelease, p, mods);
}
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	TimelineModel m;
	TlTrack video; video.kind = TlTrack::Kind::Video;
	video.clips = {clip(1, 1000, 4, 7), clip(2, 4000, 4), clip(3, 7000, 8, 9), clip(4, 10000)};
	TlTrack audio; audio.kind = TlTrack::Kind::Audio;
	audio.clips = {clip(1, 1500, 0, 7), clip(3, 7200, 8, 9)};
	m.tracks = {video, audio};
	TimelineView v; v.resize(1400, 300); v.setModel(m); v.show();
	QApplication::processEvents();
	v.selectClip(0, 0);
	ok(v.selectedPairs().size() == 3 && v.selectedPairs(false).size() == 1, "group expansion keeps explicit selection separate");
	ok(v.isSelected(0, 1) && v.isSelected(1, 0), "highlight includes group and media-link partners");
	click(v, 0, 1);
	ok(v.selectedPairs(false) == QVector<QPair<int, int>>{{0, 1}}, "clicking implicit member replaces the explicit edit target");
	v.selectClip(0, 0);
	v.addToSelection(0, 2); v.toggleSelectionGroup();
	const auto grouped = v.model();
	const int id = grouped.tracks[0].clips[0].group;
	ok(id != 0 && grouped.groupedClips(id).size() == 5, "groups merge flat with transitive linked partners");
	ok(grouped.tracks[0].clips[3].group == 0, "unselected unrelated footage stays ungrouped");
	v.selectClip(0, 0); v.toggleSelectionGroup();
	ok(v.model().groupedClips(id).isEmpty(), "G on one member ungroups entire group");
	ok(v.model().tracks[0].clips[0].link == 7 && v.model().tracks[1].clips[1].link == 9, "ungroup preserves narration and audio/video links");
	v.setModel(grouped); v.selectClip(0, 0);
	v.nudgeSelection(-10000);
	ok(v.model().tracks[0].clips[0].outStartMs == 0 && v.model().tracks[0].clips[2].outStartMs == 6000 &&
	   v.model().tracks[1].clips[0].outStartMs == 500, "nudge clamps a shared delta at earliest start");
	ok(v.model().tracks[0].clips[3].outStartMs == 10000, "nudge leaves unrelated footage fixed");
	v.setModel(grouped); v.selectClip(0, 0);
	v.applyToSelection([](TlClip &c) { c.rotation = 13; });
	ok(v.model().tracks[0].clips[0].rotation == 13 && v.model().tracks[0].clips[1].rotation == 0,
	   "Inspector edits only explicitly selected clips");
	v.setModel(grouped); v.selectClip(0, 0);
	v.setSelectionSpeed(2);
	ok(v.model().tracks[0].clips[0].speed == 2 && v.model().tracks[0].clips[2].speed == 2, "group speed changes every member");
	ok(v.model().tracks[0].clips[0].outStartMs == 1000 && v.model().tracks[0].clips[1].outStartMs == 2500 &&
	   v.model().tracks[1].clips[0].outStartMs == 1250, "speed scales gaps and offsets around earliest start");
	ok(v.model().tracks[0].clips[3] == grouped.tracks[0].clips[3], "group speed does not ripple unrelated footage");
	TimelineModel varied = grouped; varied.tracks[0].clips[1].speed = 10; varied.tracks[0].rippleSpeed = true;
	v.setModel(varied); v.selectClip(0, 0); v.setSelectionSpeed(20);
	ok(v.model().tracks[0].clips[0].speed == 2 && v.model().tracks[0].clips[1].speed == 20,
	   "shared speed factor respects fastest member and relative speeds");
	v.setSelectionSpeed(0.01);
	ok(std::abs(v.model().tracks[0].clips[0].speed - 0.1) < 1e-9 &&
	   std::abs(v.model().tracks[0].clips[1].speed - 1) < 1e-9, "shared factor respects slowest member");
	TimelineModel locked = grouped; locked.tracks[1].locked = true;
	v.setModel(locked); v.selectClip(0, 0); v.nudgeSelection(1000); v.setSelectionSpeed(2); v.toggleSelectionGroup();
	ok(v.model() == locked, "one locked member blocks whole move, speed change and grouping");
	v.setModel(grouped); click(v, 0, 0, Qt::AltModifier);
	ok(v.selectedPairs().size() == 1 && v.selectionState().individual, "Alt click selects a single member instead of panning");
	const TlSelection alt = v.selectionState();
	v.setSelectionSpeed(3);
	ok(v.model().tracks[0].clips[0].speed == 3 && v.model().tracks[0].clips[1].speed == 1, "Alt-selected speed edits only one member");
	const auto copiedOne = v.copySelection();
	ok(copiedOne.size() == 1 && copiedOne[0].clip.group == 0, "Alt copy clears persistent group membership");
	v.selectClip(0, 1); v.setSelectionState(alt);
	ok(v.selectedPairs().size() == 1 && v.selectionState() == alt, "selection snapshots restore Alt override");
	const QPoint ruler(v.xForMs(3000), 5);
	mouse(v, QEvent::MouseButtonPress, ruler); mouse(v, QEvent::MouseButtonRelease, ruler);
	ok(v.selectionState().individual && v.selectedPairs().size() == 1, "ruler seek preserves individual override");
	v.setModel(grouped); click(v, 0, 0); click(v, 0, 1, Qt::ControlModifier);
	ok(v.selectedPairs().isEmpty(), "Ctrl click removes the whole group");
	click(v, 1, 0, Qt::ControlModifier);
	ok(v.selectedPairs().size() == 5, "Ctrl click on audio selects group before volume-line gestures");
	v.setModel(grouped); v.selectClip(0, 0); const auto entries = v.copySelection(); v.pasteAt(entries, 16000);
	const auto pasted = v.model(); const int newGroup = pasted.tracks[0].clips[4].group;
	ok(newGroup != id && pasted.groupedClips(newGroup).size() == 5, "paste remaps persistent groups");
	ok(pasted.tracks[0].clips[4].link != 7 && pasted.tracks[0].clips[4].link == pasted.tracks[1].clips[2].link,
	   "paste remaps media links without joining originals");
	v.setModel(grouped); v.pasteTrack(grouped.tracks[0], 0);
	ok(v.model().tracks[0].clips[0].group != id && v.model().tracks[0].clips[0].group == v.model().tracks[0].clips[1].group &&
	   v.model().tracks[0].clips[0].link != 7, "track duplication remaps group and link IDs");
	v.setModel(grouped); v.selectClip(0, 0); v.setPlayhead(2000); v.splitAtPlayhead();
	ok(v.model().tracks[0].clips.size() == 5 && v.model().tracks[1].clips.size() == 2, "split touches only explicit selection");
	ok(v.model().tracks[0].clips[4].group == id, "both split halves retain group membership");
	v.setModel(grouped); v.selectClip(0, 0); v.deleteSelected();
	ok(v.model().tracks[0].clips.size() == 3 && v.model().tracks[1].clips.size() == 2, "delete only removes explicit selection");
	v.setModel(grouped); click(v, 0, 0);
	const QPoint from(v.clipRectForTest(0, 0).center().x(), v.clipRectForTest(0, 0).bottom() - 6);
	const QPoint to(v.xForMs(0), v.clipRectForTest(1, 0).center().y());
	mouse(v, QEvent::MouseButtonPress, from); mouse(v, QEvent::MouseMove, to); mouse(v, QEvent::MouseButtonRelease, to);
	ok(v.model().tracks[0].clips.size() == 4 && v.model().tracks[1].clips.size() == 2, "group drag keeps every member on its track");
	ok(v.model().tracks[0].clips[1].outStartMs - v.model().tracks[0].clips[0].outStartMs == 3000 &&
	   v.model().tracks[1].clips[0].outStartMs - v.model().tracks[0].clips[0].outStartMs == 500,
	   "dragging against zero preserves all original offsets");
	return failures ? 1 : 0;
}
