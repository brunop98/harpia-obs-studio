#pragma once

// The window you see while Harpia is starting, and the arithmetic behind its
// progress bar.
//
// Startup is mostly libobs work -- bringing up the graphics device, then
// loading every capture/encoder/output plugin -- and none of it can be done
// after the window appears, because the window's own capture source needs it.
// So the honest fix is twofold: do less of it (see main.cpp), and stop showing
// nothing while the rest happens.
//
// The progress is REAL, not a timer pretending. Two things make that work:
//
//   - the phases are known in advance, so the bar can be driven by "which phase
//     are we in" rather than by elapsed time;
//   - the phases are weighted by how long they took LAST time, remembered
//     across runs. Fixed weights would be a guess about a machine I cannot
//     measure -- plugin loading dominates on one box and the graphics device on
//     another -- and a guess makes the bar lurch. Measured weights make it
//     roughly linear on the machine it is actually running on, and they
//     self-correct after one run.
//
// StartupTimeline is separated from the widget because the weighting, the
// remembering and the log line are the parts that can be wrong, and none of
// them needs a screen to check.

#include <QElapsedTimer>
#include <QMap>
#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;

namespace harpia {

// The phases, in the order they run. Anything that takes long enough to see
// gets its own entry -- a phase the bar cannot distinguish is a phase the user
// watches it sit still through.
enum class StartupPhase {
	Backend,    // obs_startup
	Audio,      // obs_reset_audio
	Video,      // obs_reset_video: the graphics device
	Plugins,    // obs_load_all_modules: usually the big one
	Components, // the missing-dependency self-check
	Presets,    // preset + region files off disk
	Window,     // building MainWindow
	Finishing,  // the first hardware probe, recent clips, readiness
	Done
};

int startupPhaseCount();                       // excluding Done
StartupPhase startupPhaseAt(int index);
const char *startupPhaseKey(StartupPhase p);   // stable id, for QSettings
QString startupPhaseLabel(StartupPhase p);     // what the splash says

// Drives the bar and records what actually happened.
//
// Deliberately free of QSettings and of any widget: the caller loads and saves
// the remembered timings, so a test can hand it a table and check the
// arithmetic instead of the platform's settings store.
class StartupTimeline {
public:
	// `remembered` maps startupPhaseKey() -> milliseconds from a previous run.
	// Empty (a first run, or a new install) falls back to built-in weights.
	explicit StartupTimeline(const QMap<QString, int> &remembered = {});

	// Enter a phase. `nowMs` is a monotonic clock reading; the caller passes it
	// so the whole class stays testable without sleeping.
	void begin(StartupPhase p, qint64 nowMs);
	// Everything is done -- closes out the last phase.
	void finish(qint64 nowMs);

	StartupPhase phase() const { return phase_; }
	// 0..100, weighted by the remembered durations. Reports the work COMPLETED,
	// so it reads 0 while the first phase runs and 100 only once finish() has
	// been called -- a bar that hits 100 with work left to do is the specific
	// lie this is trying not to tell.
	int percent() const;
	QString label() const { return startupPhaseLabel(phase_); }

	// What each phase actually took, for the log and for the next run's weights.
	QMap<QString, int> measured() const { return measured_; }
	int totalMs() const;
	// "video 310ms, plugins 1240ms, window 180ms" -- slowest first, so the line
	// in the log answers "what should I look at" without any arithmetic.
	QString summary() const;

private:
	int weightOf(StartupPhase p) const;

	QMap<QString, int> weights_;  // ms, remembered or default
	QMap<QString, int> measured_; // ms, this run
	StartupPhase phase_ = StartupPhase::Backend;
	qint64 phaseStart_ = 0;
	bool started_ = false;
};

// The window itself. Frameless, centred, styled to match the app so it does not
// flash a different-looking box before the real one.
class StartupSplash : public QWidget {
	Q_OBJECT
public:
	explicit StartupSplash(const QString &versionText, QWidget *parent = nullptr);

	// Paint this phase. Calls processEvents(), because startup runs before the
	// event loop does and an unpainted splash is worse than none.
	void showPhase(const QString &label, int percent);

	// For tests: what is currently on screen.
	QString labelText() const;
	int percent() const;

private:
	QLabel *status_ = nullptr;
	QProgressBar *bar_ = nullptr;
};

} // namespace harpia
