#pragma once

// The typewriter reveal, as arithmetic.
//
// Everything here is a pure function of (the caption, how far through, what to
// reveal by), so the effect can be tested without a canvas, a clip or a window
// -- and so the component itself is a dozen lines that call it.
//
// The whole job is finding ONE CUT INDEX into the original string and handing
// back its prefix. Not rebuilding the text word by word: the caption's own
// spacing, its blank lines and its line breaks are part of how it is laid out,
// and a reveal that reassembles the words with single spaces would re-flow the
// block as it typed.

#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

// What a typing effect reveals a piece at a time.
enum class RevealUnit { Characters = 0, Words = 1, Lines = 2 };
inline constexpr int kRevealUnitCount = 3;
inline const char *revealUnitName(int u)
{
	switch (u) {
	case 1: return "Words";
	case 2: return "Lines";
	default: return "Characters";
	}
}

// The cut positions for a unit: the index just past each character, word or
// line. Always ends at the string's length, so progress 1 shows all of it.
//
// A "word" ends at the last character of a run of non-space; the spaces after
// it belong to the NEXT step, which is what makes the following word appear
// with its leading space already in place rather than jumping left.
inline QVector<int> revealStops(const QString &s, RevealUnit unit)
{
	QVector<int> stops;
	const int n = s.size();
	if (n == 0)
		return stops;
	switch (unit) {
	case RevealUnit::Characters:
		stops.reserve(n);
		for (int i = 1; i <= n; ++i)
			stops.append(i);
		break;
	case RevealUnit::Words:
		for (int i = 0; i < n; ++i) {
			const bool wordChar = !s.at(i).isSpace();
			const bool lastOfWord = wordChar && (i + 1 == n || s.at(i + 1).isSpace());
			if (lastOfWord)
				stops.append(i + 1);
		}
		if (stops.isEmpty() || stops.back() != n)
			stops.append(n);
		break;
	case RevealUnit::Lines:
		for (int i = 0; i < n; ++i)
			if (s.at(i) == QLatin1Char('\n'))
				stops.append(i + 1);
		if (stops.isEmpty() || stops.back() != n)
			stops.append(n);
		break;
	}
	return stops;
}

// How much of `s` is on screen at `u` (0..1).
//
// Rounded UP, so the first unit appears as soon as the reveal begins rather
// than after 1/n of it: a caption that shows nothing for the first fifth of its
// own effect reads as a caption that has not started.
inline QString revealedText(const QString &s, double u, RevealUnit unit)
{
	if (s.isEmpty())
		return s;
	u = std::clamp(u, 0.0, 1.0);
	if (u >= 1.0)
		return s;
	if (u <= 0.0)
		return QString();
	const QVector<int> stops = revealStops(s, unit);
	if (stops.isEmpty())
		return s;
	const int n = int(stops.size());
	const int step = std::clamp(int(std::ceil(u * n)), 1, n);
	return s.left(stops[step - 1]);
}

// Is the caret lit at this moment? `hz` is blinks per second; 0 (or less) is a
// caret that never blinks, which is a real choice rather than a broken one.
inline bool caretVisibleAt(double tSec, double hz)
{
	if (hz <= 0.0)
		return true;
	// A full cycle is on then off, so the period is 1/hz and the caret is lit
	// for the first half of it. fmod keeps the sign of its argument, so a
	// negative time (a clip evaluated before its own start) is wrapped forward
	// rather than left in a half-cycle of its own.
	double phase = std::fmod(tSec * hz, 1.0);
	if (phase < 0.0)
		phase += 1.0;
	return phase < 0.5;
}

} // namespace harpia
