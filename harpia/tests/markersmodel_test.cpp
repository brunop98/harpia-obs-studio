#include "editor/SilenceTrim.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QCoreApplication>
#include <QJsonDocument>

#include <cstdio>

using namespace harpia;

namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

TimelineModel fixture()
{
	TimelineModel model;
	TlTrack track;
	TlClip clip;
	clip.type = TlClip::Type::Video;
	clip.sourceId = 1;
	clip.srcEndMs = 10000;
	track.clips = {clip};
	model.tracks = {track};
	model.markers = {0, 2000, 6000};
	model.markerLabels = {{0, QStringLiteral("Intro")}, {2000, QStringLiteral("Part two")},
			      {6000, QStringLiteral("Ending – fim")}};
	return model;
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	{
		TimelineModel model = fixture();
		const auto clips = model.tracks;
		check(!model.hasVideoEnd() && model.durationMs() == 10000, "legacy timelines end at their last clip");
		model.videoEndMs = 4000;
		check(model.hasVideoEnd() && model.durationMs() == 4000 && model.contentDurationMs() == 10000 && model.tracks == clips,
		      "the explicit output end leaves every clip editable beyond it");
		model.videoEndMs = 12000;
		check(model.durationMs() == 12000 && model.contentDurationMs() == 10000, "an end marker may include a trailing gap");
		model.videoEndMs = -1;
		check(model.durationMs() == 10000, "resetting the end restores the content duration");
		TimelineModel changed = model;
		changed.markerLabels[2000] = QStringLiteral("Renamed");
		check(changed != model, "marker labels participate in undo snapshot equality");
		changed = model;
		changed.videoEndMs = 4000;
		check(changed != model, "the output end participates in undo snapshot equality");
	}
	{
		TimelineModel model = fixture();
		model.videoEndMs = 4000;
		QJsonObject root;
		timelineToProject(model, root);
		const QJsonObject disk = QJsonDocument::fromJson(QJsonDocument(root).toJson()).object();
		const TimelineModel opened = timelineFromProject(disk);
		check(opened == model, "positions, Unicode labels, end marker and untrimmed clips survive a project round trip");
		check(root.value(QStringLiteral("markers")).toArray().at(1).isDouble(), "marker positions keep their legacy numeric format");
		TimelineModel legacy = fixture();
		legacy.markerLabels.clear();
		root.remove(QStringLiteral("markerLabels"));
		root.remove(QStringLiteral("videoEndMs"));
		check(timelineFromProject(root) == legacy, "old numeric-only projects remain compatible");
		timelineToProject(model, root);
		timelineToProject(TimelineModel{}, root);
		check(!root.contains(QStringLiteral("markers")) && !root.contains(QStringLiteral("markerLabels")) &&
		      !root.contains(QStringLiteral("videoEndMs")), "saving removed metadata clears a reused project object");
		TimelineModel onlyEnd;
		onlyEnd.videoEndMs = 5000;
		root = {};
		timelineToProject(onlyEnd, root);
		check(timelineFromProject(root) == onlyEnd, "an end marker is saved even before any clips are added");
	}
	{
		QJsonObject root;
		root[QStringLiteral("markers")] = QJsonArray{6000, 0, 1000, 1000, -1, QStringLiteral("200"), true, 0.5, 1e30};
		root[QStringLiteral("markerLabels")] = QJsonObject{{QStringLiteral("1000"), QStringLiteral("Chapter")},
			{QStringLiteral("2000"), QStringLiteral("Orphan")}, {QStringLiteral("bad"), QStringLiteral("Invalid")},
			{QStringLiteral("0"), false}, {QStringLiteral("6000"), QStringLiteral("   ")}};
		root[QStringLiteral("videoEndMs")] = 6000;
		TimelineModel loaded = timelineFromProject(root);
		check(loaded.markers == QVector<qint64>{0, 1000, 6000} &&
		      loaded.markerLabels == QMap<qint64, QString>{{1000, QStringLiteral("Chapter")}} && loaded.videoEndMs == 6000,
		      "invalid positions, duplicate markers and orphan or invalid labels are ignored");
		for (const QJsonValue &end : {QJsonValue(0), QJsonValue(-1), QJsonValue(0.5), QJsonValue(1e30), QJsonValue(QStringLiteral("6000"))}) {
			root[QStringLiteral("videoEndMs")] = end;
			check(!timelineFromProject(root).hasVideoEnd(), "invalid end marker values retain automatic duration");
		}
	}
	{
		TimelineModel original = fixture();
		original.videoEndMs = 4000;
		const TimelineModel selected = sliceTimeline(original, 1000, 7000);
		check(selected.markers == QVector<qint64>{1000, 5000} &&
		      selected.markerLabels == QMap<qint64, QString>{{1000, QStringLiteral("Part two")}, {5000, QStringLiteral("Ending – fim")}},
		      "selection exports keep only their own markers and rebase every label");
		check(selected.videoEndMs == 6000 && selected.durationMs() == 6000 && original.videoEndMs == 4000,
		      "an explicit selection owns its span even beyond the project's end marker");
		const TimelineModel withGap = sliceTimeline(original, 8000, 14000);
		check(withGap.durationMs() == 6000 && withGap.contentDurationMs() == 2000,
		      "a selected export preserves its requested trailing gap");
		check(!sliceTimeline(original, 7000, 6000).hasVideoEnd(), "an invalid slice stays empty");
	}
	{
		TimelineModel original = fixture();
		original.markers = {500, 2500, 3000, 8000};
		original.markerLabels = {{500, QStringLiteral("Intro")}, {2500, QStringLiteral("Cut")},
			{3000, QStringLiteral("Join")}, {8000, QStringLiteral("Past the end")}};
		original.videoEndMs = 6000;
		const auto edit = silence_trim::apply(original, {{2000, 3000}});
		check(edit.ok() && edit.model.videoEndMs == 5000 && edit.model.durationMs() == 5000,
		      "ripple pause removal moves the explicit end by the removed duration");
		check(edit.model.markers == QVector<qint64>{500, 2000, 7000} &&
		      edit.model.markerLabels == QMap<qint64, QString>{{500, QStringLiteral("Intro")},
			{2000, QStringLiteral("Join")}, {7000, QStringLiteral("Past the end")}},
		      "ripple removes cut markers and keeps each surviving label attached to its moved position");
		check(edit.model.contentDurationMs() == 9000 && edit.model.tracks[0].clips.last().srcEndMs == 10000 &&
		      original.contentDurationMs() == 10000,
		      "ripple preserves editable material beyond the output end and leaves the source model untouched");
	}
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
