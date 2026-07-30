// The startup progress bar, and whether it is telling the truth.
//
// A splash is easy to fake and the fake is worse than nothing: a bar driven by
// a timer, or one that reaches 100% while the app is still loading, teaches
// people that the bar means nothing. So the two claims worth checking are that
// it MOVES ONLY when real work finishes, and that it never claims to be done
// before it is.
//
// The other half is the weighting. Fixed weights would be a guess about a
// machine I cannot measure here -- plugin loading dominates on one box and the
// graphics device on another -- so the weights come from the previous run's
// measured durations. That makes the bar roughly linear on the machine it is
// actually running on, and it is the part most likely to break quietly, since a
// wrong weight still produces a bar that looks fine.
#include "ui/StartupSplash.hpp"

#include <QApplication>

#include <algorithm>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// Walk every phase, spending `ms` in each, and return the percent seen on entry
// to each one.
static QVector<int> walk(StartupTimeline &t, const QVector<int> &msPerPhase)
{
	QVector<int> seen;
	qint64 now = 0;
	for (int i = 0; i < startupPhaseCount(); ++i) {
		t.begin(startupPhaseAt(i), now);
		seen.append(t.percent());
		now += msPerPhase.value(i, 10);
	}
	t.finish(now);
	return seen;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the bar only moves forward, and only on real work --\n");
	{
		StartupTimeline t;
		const QVector<int> seen = walk(t, {});
		std::printf("    ");
		for (int p : seen)
			std::printf(" %d%%", p);
		std::printf("\n");
		ok(!seen.isEmpty() && seen.first() == 0, "it starts at zero");
		bool monotonic = true;
		for (int i = 1; i < seen.size(); ++i)
			monotonic = monotonic && seen[i] >= seen[i - 1];
		ok(monotonic, "and never goes backwards");
		// The specific lie a progress bar can tell: sitting full while the
		// application is still working.
		ok(*std::max_element(seen.begin(), seen.end()) < 100,
		   "it never reaches 100% while there is still a phase to run");
		ok(t.percent() == 100, "and reaches it once everything is done");
	}
	{
		// The case that actually reaches the ceiling: a warm run where the
		// last phase measured nothing at all last time, so its share of the
		// bar rounds away and the phases before it account for the whole of
		// it. Without the clamp the bar sits at a full 100% while
		// finishStartup() is still probing cameras -- which is the exact
		// impression a progress bar exists to avoid giving.
		QMap<QString, int> lastPhaseFree;
		for (int i = 0; i < startupPhaseCount(); ++i)
			lastPhaseFree.insert(QString::fromLatin1(startupPhaseKey(startupPhaseAt(i))),
					     4000);
		lastPhaseFree.insert(QStringLiteral("finishing"), 0);
		StartupTimeline t(lastPhaseFree);
		qint64 now = 0;
		int atLast = 0;
		for (int i = 0; i < startupPhaseCount(); ++i) {
			t.begin(startupPhaseAt(i), now);
			atLast = t.percent();
			now += 10;
		}
		std::printf("     last phase weighted at zero: enters at %d%%\n", atLast);
		ok(atLast < 100, "even a phase with no weight left does not read as finished");
		t.finish(now);
		ok(t.percent() == 100, "and 100% still means 100%");
	}

	std::printf("\n-- every phase gets some of the bar --\n");
	{
		// A phase with no width is a phase the bar skips past without ever
		// naming, which is how "Loading plugins" ends up never being seen on a
		// machine where it is instant.
		StartupTimeline t;
		const QVector<int> seen = walk(t, {});
		bool distinct = true;
		for (int i = 1; i < seen.size(); ++i)
			distinct = distinct && seen[i] > seen[i - 1];
		ok(distinct, "each phase starts at a higher percentage than the last");

		// Even when the remembered timings say a phase took no time at all.
		QMap<QString, int> zeros;
		for (int i = 0; i < startupPhaseCount(); ++i)
			zeros.insert(QString::fromLatin1(startupPhaseKey(startupPhaseAt(i))), 0);
		StartupTimeline z(zeros);
		const QVector<int> zseen = walk(z, {});
		std::printf("     all-zero weights:");
		for (int p : zseen)
			std::printf(" %d%%", p);
		std::printf("\n");
		bool zdistinct = true;
		for (int i = 1; i < zseen.size(); ++i)
			zdistinct = zdistinct && zseen[i] > zseen[i - 1];
		ok(zdistinct, "including when every remembered duration is zero");
	}

	std::printf("\n-- the weights follow what actually took the time --\n");
	{
		// The point of remembering. On a machine where plugins dominate, the
		// bar should spend most of its travel on plugins -- so the percentage
		// on ENTERING plugins is low and the jump after it is large.
		QMap<QString, int> slowPlugins;
		for (int i = 0; i < startupPhaseCount(); ++i)
			slowPlugins.insert(QString::fromLatin1(startupPhaseKey(startupPhaseAt(i))), 10);
		slowPlugins.insert(QStringLiteral("plugins"), 5000);
		StartupTimeline t(slowPlugins);

		int before = -1, after = -1;
		qint64 now = 0;
		for (int i = 0; i < startupPhaseCount(); ++i) {
			const StartupPhase p = startupPhaseAt(i);
			t.begin(p, now);
			if (p == StartupPhase::Plugins)
				before = t.percent();
			else if (before >= 0 && after < 0)
				after = t.percent();
			now += 10;
		}
		std::printf("     plugins spans %d%% -> %d%%\n", before, after);
		ok(before >= 0 && before < 15, "a slow plugin phase begins early in the bar");
		ok(after - before > 70, "and takes most of the bar's travel");
	}
	{
		// ...and the mirror image, so the test is checking the weighting
		// rather than a hard-coded shape that happens to favour plugins.
		QMap<QString, int> slowVideo;
		for (int i = 0; i < startupPhaseCount(); ++i)
			slowVideo.insert(QString::fromLatin1(startupPhaseKey(startupPhaseAt(i))), 10);
		slowVideo.insert(QStringLiteral("video"), 5000);
		StartupTimeline t(slowVideo);
		int atVideo = -1, atPlugins = -1;
		qint64 now = 0;
		for (int i = 0; i < startupPhaseCount(); ++i) {
			const StartupPhase p = startupPhaseAt(i);
			t.begin(p, now);
			if (p == StartupPhase::Video)
				atVideo = t.percent();
			if (p == StartupPhase::Plugins)
				atPlugins = t.percent();
			now += 10;
		}
		std::printf("     video spans %d%% -> %d%%\n", atVideo, atPlugins);
		ok(atPlugins - atVideo > 70, "a slow graphics device takes the bar instead");
	}

	std::printf("\n-- a nonsense remembered timing is ignored --\n");
	{
		// The machine that slept for an hour mid-startup must not poison every
		// run afterwards with a bar that sits at 2%.
		QMap<QString, int> absurd;
		absurd.insert(QStringLiteral("plugins"), 9999999);
		StartupTimeline t(absurd);
		const QVector<int> seen = walk(t, {});
		int biggestJump = 0;
		for (int i = 1; i < seen.size(); ++i)
			biggestJump = std::max(biggestJump, seen[i] - seen[i - 1]);
		std::printf("     biggest single jump %d%%\n", biggestJump);
		ok(biggestJump < 90, "an implausible duration falls back to the default weight");

		QMap<QString, int> negative;
		negative.insert(QStringLiteral("plugins"), -5);
		StartupTimeline n(negative);
		const QVector<int> nseen = walk(n, {});
		bool nmono = true;
		for (int i = 1; i < nseen.size(); ++i)
			nmono = nmono && nseen[i] > nseen[i - 1];
		ok(nmono, "and so does a negative one");
	}

	std::printf("\n-- what it measured is what it reports --\n");
	{
		StartupTimeline t;
		qint64 now = 0;
		const QVector<int> spend = {5, 10, 300, 1200, 5, 5, 150, 200};
		for (int i = 0; i < startupPhaseCount(); ++i) {
			t.begin(startupPhaseAt(i), now);
			now += spend.value(i, 0);
		}
		t.finish(now);
		const QMap<QString, int> m = t.measured();
		ok(m.value(QStringLiteral("plugins")) == 1200, "each phase's own duration is recorded");
		ok(m.value(QStringLiteral("finishing")) == 200,
		   "including the last one, which only finish() can close");
		ok(t.totalMs() == 1875, "and the total is the sum of them");
		std::printf("     %s\n", qPrintable(t.summary()));
		// Slowest first: the log line has to answer "what should I look at"
		// without the reader doing arithmetic.
		ok(t.summary().startsWith(QStringLiteral("plugins 1200ms")),
		   "the summary leads with the slowest phase");
	}

	std::printf("\n-- and the window shows what it is told --\n");
	{
		StartupSplash s(QStringLiteral("v0.1.207"));
		s.showPhase(QStringLiteral("Loading plugins"), 42);
		ok(s.labelText() == QStringLiteral("Loading plugins"), "the status line says the phase");
		ok(s.percent() == 42, "and the bar shows the percentage");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
