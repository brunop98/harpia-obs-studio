#pragma once

// Rich text in captions, written the way Unity writes it: tags in the text.
//
//   <b>bold</b>  <i>italic</i>  <u>underline</u>  <s>strike</s>
//   <color=#ff8800>…</color>   (#rgb, #rgba, #rrggbb, #rrggbbaa or a name: red, yellow…)
//   <size=90>…</size>          (px on the 1080-tall reference, like the font size;
//                               also <size=150%> and <size=+20> / <size=-20>)
//   <alpha=#80>                (opacity of what follows, until </alpha> or the next one)
//   <mark=#ffff0080>…</mark>   (a highlight behind the words)
//   <br>                       (a line break)
//   <noparse>…</noparse>       (everything inside is shown as typed)
//
// Tag names and colour names ignore case, so a caption set to UPPER CASE keeps
// its tags. A tag that is not one of these is shown as typed, as in Unity; an
// opening tag nobody closes simply lasts to the end, and a stray closing tag
// is dropped -- which is what keeps a typewriter reveal or a word-by-word
// subtitle, which cut the text anywhere between tags, drawing correctly.
//
// Everything here is pure: the parse, the plain text and where the tags are.
// The compositor lays the runs out.

#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

#include <algorithm>

namespace harpia {

namespace rich_text {

struct Style {
	bool bold = false, italic = false, underline = false, strike = false;
	QColor color;      // invalid = the caption's own colour
	int alpha = -1;    // 0..255, <0 = the colour's own
	double sizePx = 0; // on the 1080 reference; <=0 = the caption's own size
	QColor mark;       // invalid = no highlight

	bool operator==(const Style &o) const
	{
		return bold == o.bold && italic == o.italic && underline == o.underline && strike == o.strike &&
		       color == o.color && alpha == o.alpha && sizePx == o.sizePx && mark == o.mark;
	}
	bool operator!=(const Style &o) const { return !(*this == o); }
};

struct Run {
	QString text;
	Style style;
};

// One line: its runs, in order. Lines come from typed line breaks and <br>.
using Line = QVector<Run>;

// A colour as Unity reads it: #rgb, #rgba, #rrggbb, #rrggbbaa (alpha LAST,
// unlike Qt), or a colour name. Quotes around the value are allowed.
inline QColor parseColor(QString v)
{
	v = v.trimmed();
	if (v.size() >= 2 && (v.startsWith(QLatin1Char('"')) || v.startsWith(QLatin1Char('\''))) &&
	    v.endsWith(v.at(0)))
		v = v.mid(1, v.size() - 2).trimmed();
	if (v.startsWith(QLatin1Char('#'))) {
		const QString h = v.mid(1);
		bool ok = false;
		const uint n = h.toUInt(&ok, 16);
		if (!ok)
			return QColor();
		const auto nib = [](uint x) { return int(x * 17); };
		switch (h.size()) {
		case 3: return QColor(nib((n >> 8) & 15), nib((n >> 4) & 15), nib(n & 15));
		case 4: return QColor(nib((n >> 12) & 15), nib((n >> 8) & 15), nib((n >> 4) & 15), nib(n & 15));
		case 6: return QColor(int((n >> 16) & 255), int((n >> 8) & 255), int(n & 255));
		case 8: return QColor(int((n >> 24) & 255), int((n >> 16) & 255), int((n >> 8) & 255), int(n & 255));
		default: return QColor();
		}
	}
	if (v.isEmpty())
		return QColor();
	for (const QChar ch : v)
		if (!ch.isLetter())
			return QColor();
	return QColor::fromString(v.toLower());
}

// One tag, recognised or not.
struct Tag {
	enum class Kind { None, Bold, Italic, Underline, Strike, Color, Size, Alpha, Mark, Break, NoParse };
	Kind kind = Kind::None;
	bool closing = false;
	QString value; // after '=' (opening tags only)
	int length = 0; // characters from '<' to '>' inclusive
};

// The tag starting at `at` (which must be '<'), or Kind::None when the text
// there is not one of ours -- then it is shown as typed.
inline Tag tagAt(const QString &s, int at)
{
	Tag t;
	if (at < 0 || at >= s.size() || s.at(at) != QLatin1Char('<'))
		return t;
	const int end = s.indexOf(QLatin1Char('>'), at + 1);
	if (end < 0)
		return t;
	QString body = s.mid(at + 1, end - at - 1);
	if (body.contains(QLatin1Char('<')) || body.contains(QLatin1Char('\n')))
		return t;
	bool closing = false;
	if (body.startsWith(QLatin1Char('/'))) {
		closing = true;
		body.remove(0, 1);
	}
	QString name = body, value;
	const int eq = body.indexOf(QLatin1Char('='));
	if (eq >= 0) {
		name = body.left(eq);
		value = body.mid(eq + 1);
	}
	name = name.trimmed().toLower();
	if (closing && eq >= 0)
		return t;
	using K = Tag::Kind;
	K k = K::None;
	if (name == QLatin1String("b"))
		k = K::Bold;
	else if (name == QLatin1String("i"))
		k = K::Italic;
	else if (name == QLatin1String("u"))
		k = K::Underline;
	else if (name == QLatin1String("s"))
		k = K::Strike;
	else if (name == QLatin1String("color") || name == QLatin1String("colour"))
		k = K::Color;
	else if (name == QLatin1String("size"))
		k = K::Size;
	else if (name == QLatin1String("alpha"))
		k = K::Alpha;
	else if (name == QLatin1String("mark"))
		k = K::Mark;
	else if (name == QLatin1String("br") || name == QLatin1String("br/"))
		k = K::Break;
	else if (name == QLatin1String("noparse"))
		k = K::NoParse;
	if (k == K::None)
		return t;
	const bool needsValue = k == K::Color || k == K::Size || k == K::Alpha || k == K::Mark;
	if (!closing && needsValue && value.trimmed().isEmpty())
		return t;
	if (!needsValue && eq >= 0)
		return t;
	if (closing && k == K::Break)
		return t;
	// A value that means nothing is not a tag either: "<color=banana>" shows.
	if (!closing && (k == K::Color || k == K::Mark) && !parseColor(value).isValid())
		return t;
	if (!closing && k == K::Alpha) {
		const QString v = value.trimmed(); // "#80": two hex digits
		bool ok = false;
		if (v.size() == 3 && v.startsWith(QLatin1Char('#')))
			v.mid(1).toUInt(&ok, 16);
		if (!ok)
			return t;
	}
	if (!closing && k == K::Size) {
		QString v = value.trimmed();
		if (v.endsWith(QLatin1Char('%')))
			v.chop(1);
		if (v.endsWith(QLatin1String("px"), Qt::CaseInsensitive))
			v.chop(2);
		bool ok = false;
		v.toDouble(&ok);
		if (!ok)
			return t;
	}
	t.kind = k;
	t.closing = closing;
	t.value = value.trimmed();
	t.length = end - at + 1;
	return t;
}

// Does the text use any tag at all? Captions without one are drawn exactly as
// before rich text existed.
inline bool hasTags(const QString &s)
{
	for (int i = s.indexOf(QLatin1Char('<')); i >= 0; i = s.indexOf(QLatin1Char('<'), i + 1))
		if (tagAt(s, i).kind != Tag::Kind::None)
			return true;
	return false;
}

// For every character: is it part of a tag (and so never drawn)? Text inside
// <noparse> is not.
inline QVector<bool> tagMask(const QString &s)
{
	QVector<bool> mask(s.size(), false);
	bool noparse = false;
	for (int i = 0; i < s.size();) {
		if (s.at(i) == QLatin1Char('<')) {
			const Tag t = tagAt(s, i);
			if (t.kind != Tag::Kind::None && (!noparse || (t.kind == Tag::Kind::NoParse && t.closing))) {
				if (t.kind == Tag::Kind::NoParse)
					noparse = !t.closing;
				for (int k = 0; k < t.length; ++k)
					mask[i + k] = true;
				i += t.length;
				continue;
			}
		}
		++i;
	}
	return mask;
}

// The runs, line by line. `basePx` is the caption's own size, for <size=150%>
// and <size=+20>.
inline QVector<Line> parse(const QString &s, double basePx)
{
	QVector<Line> lines(1);
	// Each kind of tag nests on its own, as in Unity: </b> ends the bold and
	// nothing else, whatever was opened inside it. Per kind, the style as it
	// was before each opening still open.
	QHash<int, QVector<Style>> saved;
	Style cur;
	QString buf;
	bool noparse = false;
	const auto flush = [&]() {
		if (buf.isEmpty())
			return;
		Line &l = lines.last();
		if (!l.isEmpty() && l.last().style == cur)
			l.last().text += buf;
		else
			l.push_back({buf, cur});
		buf.clear();
	};
	const auto newLine = [&]() {
		flush();
		lines.push_back(Line());
	};
	for (int i = 0; i < s.size();) {
		const QChar ch = s.at(i);
		if (ch == QLatin1Char('\n')) {
			newLine();
			++i;
			continue;
		}
		if (ch == QLatin1Char('<')) {
			const Tag t = tagAt(s, i);
			const bool usable = t.kind != Tag::Kind::None &&
					    (!noparse || (t.kind == Tag::Kind::NoParse && t.closing));
			if (usable) {
				i += t.length;
				using K = Tag::Kind;
				if (t.kind == K::NoParse) {
					noparse = !t.closing;
					continue;
				}
				if (t.kind == K::Break) {
					newLine();
					continue;
				}
				flush();
				if (t.closing) {
					// Back to how this kind was before its opening; a stray
					// closing tag changes nothing.
					QVector<Style> &sv = saved[int(t.kind)];
					if (sv.isEmpty())
						continue;
					const Style prev = sv.takeLast();
					switch (t.kind) {
					case K::Bold: cur.bold = prev.bold; break;
					case K::Italic: cur.italic = prev.italic; break;
					case K::Underline: cur.underline = prev.underline; break;
					case K::Strike: cur.strike = prev.strike; break;
					case K::Color:
						cur.color = prev.color;
						cur.alpha = prev.alpha;
						break;
					case K::Size: cur.sizePx = prev.sizePx; break;
					case K::Alpha: cur.alpha = prev.alpha; break;
					case K::Mark: cur.mark = prev.mark; break;
					default: break;
					}
					continue;
				}
				Style st = cur;
				switch (t.kind) {
				case K::Bold: st.bold = true; break;
				case K::Italic: st.italic = true; break;
				case K::Underline: st.underline = true; break;
				case K::Strike: st.strike = true; break;
				case K::Color: {
					st.color = parseColor(t.value);
					// A colour that says its own alpha replaces an <alpha>
					// before it; one that does not keeps it.
					if (st.color.alpha() != 255)
						st.alpha = -1;
					break;
				}
				case K::Mark: st.mark = parseColor(t.value); break;
				case K::Alpha: st.alpha = int(t.value.mid(1).toUInt(nullptr, 16)); break;
				case K::Size: {
					QString v = t.value;
					const double cur0 = st.sizePx > 0 ? st.sizePx : basePx;
					if (v.endsWith(QLatin1Char('%'))) {
						v.chop(1);
						st.sizePx = cur0 * v.toDouble() / 100.0;
					} else {
						if (v.endsWith(QLatin1String("px"), Qt::CaseInsensitive))
							v.chop(2);
						const double n = v.toDouble();
						st.sizePx = (v.startsWith(QLatin1Char('+')) || v.startsWith(QLatin1Char('-')))
								    ? cur0 + n
								    : n;
					}
					st.sizePx = std::clamp(st.sizePx, 4.0, 2000.0);
					break;
				}
				default: break;
				}
				saved[int(t.kind)].push_back(cur);
				cur = st;
				continue;
			}
		}
		buf += ch;
		++i;
	}
	flush();
	return lines;
}

// What the caption says, without the tags: for labels, file names and
// anything else that shows the words rather than draws them.
inline QString plainText(const QString &s)
{
	if (!s.contains(QLatin1Char('<')))
		return s;
	QStringList out;
	for (const Line &l : parse(s, 64.0)) {
		QString line;
		for (const Run &r : l)
			line += r.text;
		out << line;
	}
	return out.join(QLatin1Char('\n'));
}

} // namespace rich_text

} // namespace harpia
