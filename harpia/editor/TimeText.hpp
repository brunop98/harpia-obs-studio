#pragma once

// Milliseconds as text, in the handful of shapes this app actually shows.
//
// Six places used to spell this out by hand — the trim timeline, the preview
// readout, both rulers, the hover tag and the library — and two of them were
// character-for-character identical. Six copies means six chances for one of
// them to pad differently or round the other way, which is exactly the sort of
// difference nobody notices until two numbers on the same screen disagree.
//
// Each function is named for what it is FOR rather than for its format, because
// the choice of format is the interesting part: a ruler wants the shortest thing
// that stays unambiguous, a scrub readout wants every millisecond.

#include <QLatin1Char>
#include <QString>

namespace harpia {

// "m:ss.mmm" — the preview's position readout, where the exact frame matters.
inline QString timeTextMs(qint64 ms)
{
	return QStringLiteral("%1:%2.%3")
		.arg(ms / 60000)
		.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
		.arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

// "mm:ss.d" — the trim timeline's Start/End labels: zero-padded so the two sit
// under each other without the text shifting as the minutes tick over.
inline QString timeTextTenths(qint64 ms)
{
	return QStringLiteral("%1:%2.%3")
		.arg(ms / 60000, 2, 10, QLatin1Char('0'))
		.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
		.arg((ms % 1000) / 100);
}

// "m:ss.cc" — the hover tag, which wants more precision than a ruler tick but
// less width than full milliseconds.
inline QString timeTextCentis(qint64 ms)
{
	return QStringLiteral("%1:%2.%3")
		.arg(ms / 60000)
		.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
		.arg((ms % 1000) / 10, 2, 10, QLatin1Char('0'));
}

// A ruler tick. Under a minute, and not on a whole second, it reads "1.5s" —
// which is shorter and clearer than "0:01" when the ticks are that close
// together. Otherwise "m:ss".
inline QString timeTextRuler(qint64 ms)
{
	if (ms % 1000 && ms < 60000)
		return QStringLiteral("%1.%2s").arg(ms / 1000).arg((ms % 1000) / 100);
	return QStringLiteral("%1:%2").arg(ms / 60000).arg((ms / 1000) % 60, 2, 10,
							   QLatin1Char('0'));
}

// A clip's length in a list: "h:mm:ss" only once there are hours to show.
inline QString timeTextDuration(qint64 ms)
{
	const qint64 secs = ms / 1000;
	const qint64 h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
	if (h > 0)
		return QStringLiteral("%1:%2:%3")
			.arg(h)
			.arg(m, 2, 10, QLatin1Char('0'))
			.arg(s, 2, 10, QLatin1Char('0'));
	return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10,
									  QLatin1Char('0'));
}

} // namespace harpia
