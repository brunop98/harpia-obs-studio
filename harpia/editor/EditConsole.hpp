#pragma once

// The editing console: a line of JavaScript against the timeline.
//
//   clip.position = [500, 300]      // the selected clip, in canvas pixels
//   clip.scale = 1.2
//   selection.forEach(c => c.opacity = 0.5)
//   clips.filter(c => c.track == "V1").forEach((c, i) => c.x = 200 + i * 40)
//   run("intro-zoom")               // a saved template
//
// Two jobs, one mechanism. The FASTER HAND: exact numbers, many clips at
// once, without hunting for a slider. TEMPLATES: the same line saved as a
// file and run on the next project, so an edit's recipe outlives the edit.
//
// How it stays safe: a snippet never touches the live model. It runs against
// a plain JavaScript picture of a COPY, and only when the whole snippet has
// finished without an error are the changed values read back into that copy
// and handed to the caller -- who swaps it in as ONE undo step. A typo
// halfway through a loop therefore changes nothing at all. The picture is
// built fresh per run: no state leaks between lines except what the user
// keeps in `globalThis` on purpose.
//
// Units: milliseconds for time, canvas PIXELS for position (the model stores
// fractions of the canvas; the console converts both ways), plain factors for
// scale, 0..1 for opacity.
//
// What a change means: the same thing the Inspector's pinned fields mean --
// the pose at the playhead is read, the property set, and written back as the
// clip's base transform (TlClip::setBaseTransform), so a keyed clip behaves
// exactly as it does when you type into the Inspector.

#include "timeline/TimelineModel.hpp"

#include <QSize>
#include <QString>
#include <QVector>

#include <functional>

namespace harpia {

class EditConsole {
public:
	struct Input {
		TimelineModel model;                     // a copy; the console edits it
		QVector<QPair<int, int>> selection;      // (track, clip), primary first
		QSize canvas{1920, 1080};
		qint64 playheadMs = 0;
		// Name for a clip's source, for `clip.name` and the console's own
		// "clip -> V1[3] intro.mp4" lines. Optional.
		std::function<QString(int sourceId)> sourceName;
		// Source of a saved template by name, or "" when there is none. What
		// `run("name")` calls. Optional; without it run() reports an error.
		std::function<QString(const QString &name)> loadTemplate;
	};
	struct Result {
		bool ok = false;         // the snippet ran to the end
		bool changed = false;    // ...and the model differs from what went in
		QString output;          // print() lines and the value of the last expression
		QString error;           // when !ok
		TimelineModel model;     // the edited copy (equal to the input when !changed)
	};

	// False in a build without the scripting engine: run() then fails with a
	// message saying so, and the window hides the console button.
	static bool available();

	// Run one snippet. Never throws; never touches anything but the copy in
	// `in.model`.
	static Result run(const QString &source, const Input &in);

	// "V1[3]": the lane's name and the clip's slot on it (by start time), which
	// is how the console names a clip and how `clips` is ordered.
	static QString refName(const TimelineModel &m, int track, int clip);

	// The property list, for help() and the panel.
	static QString helpText();
};

} // namespace harpia
