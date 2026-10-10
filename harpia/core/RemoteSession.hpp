#pragma once

// The rules for a recording started by a remote client (the Unity package),
// apart from the recorder so they can be checked without one.
//
//   * A remote start never takes over a recording you started by hand: it is
//     refused, and that recording is left alone.
//   * Only a recording the remote started can be paused, resumed or stopped by
//     it -- leaving Play Mode never stops your own take.
//   * Repeating a command is harmless (start while already recording for the
//     remote, pause while paused, stop while stopping all just say "ok").
//   * A remote stop can ask for a run shorter than N ms to be deleted, without
//     the "keep or discard?" question a preset's minimum length asks.

#include <QString>
#include <QtGlobal>

namespace harpia {

struct RecorderSnapshot {
	bool recording = false; // the output is running (paused or not)
	bool paused = false;
	bool starting = false;
	bool stopping = false;
	bool countingDown = false;
};

class RemoteSession {
public:
	struct Answer {
		bool proceed = false; // do the action
		bool ok = false;      // report success to the client
		int status = 200;     // HTTP status when not ok
		QString message;
	};

	Answer start(const RecorderSnapshot &s) const
	{
		if (owns_ && (s.recording || s.starting))
			return {false, true, 200, QStringLiteral("already recording")};
		if (s.stopping)
			return {false, false, 409, QStringLiteral("Harpia is still saving the previous recording")};
		if (s.recording || s.starting || s.countingDown)
			return {false, false, 409,
				QStringLiteral("Harpia is already recording (started by hand), so it was left alone")};
		return {true, true, 200, {}};
	}

	Answer pause(const RecorderSnapshot &s) const
	{
		if (const Answer a = mustOwnRunning(s); !a.proceed)
			return a;
		if (s.paused)
			return {false, true, 200, QStringLiteral("already paused")};
		return {true, true, 200, {}};
	}

	Answer resume(const RecorderSnapshot &s) const
	{
		if (const Answer a = mustOwnRunning(s); !a.proceed)
			return a;
		if (!s.paused)
			return {false, true, 200, QStringLiteral("not paused")};
		return {true, true, 200, {}};
	}

	Answer stop(const RecorderSnapshot &s) const
	{
		if (!owns_)
			return {false, false, 409, QStringLiteral("no recording started by this client")};
		if (s.stopping)
			return {false, true, 200, QStringLiteral("already stopping")};
		if (!s.recording && !s.starting)
			return {false, true, 200, QStringLiteral("not recording")};
		return {true, true, 200, {}};
	}

	// The recorder tells the session what happened.
	void began()
	{
		owns_ = true;
		discardBelowMs_ = 0;
	}
	void stopRequested(int discardShorterThanMs) { discardBelowMs_ = qMax(0, discardShorterThanMs); }
	void ended()
	{
		owns_ = false;
		discardBelowMs_ = 0;
	}

	bool owns() const { return owns_; }
	// Delete this run instead of saving it (only when the remote's stop asked).
	bool discard(qint64 contentMs) const { return owns_ && discardBelowMs_ > 0 && contentMs < discardBelowMs_; }

private:
	Answer mustOwnRunning(const RecorderSnapshot &s) const
	{
		if (!owns_)
			return {false, false, 409, QStringLiteral("no recording started by this client")};
		if (s.stopping || (!s.recording && !s.starting))
			return {false, false, 409, QStringLiteral("not recording")};
		return {true, true, 200, {}};
	}

	bool owns_ = false;
	int discardBelowMs_ = 0;
};

} // namespace harpia
