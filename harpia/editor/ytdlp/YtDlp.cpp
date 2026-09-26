#include "YtDlp.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace harpia {

namespace {
const QString kGrp = QStringLiteral("ytdlp/");
QSettings settings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}

struct Probe {
	bool done = false;
	bool ok = false;
	QString path;
	QString version;
};
Probe &probeState()
{
	static Probe p;
	return p;
}
} // namespace

// ---- settings ---------------------------------------------------------------

YtDlpSettings YtDlpSettings::load()
{
	QSettings s = settings();
	YtDlpSettings o;
	o.exePath = s.value(kGrp + QStringLiteral("exePath")).toString();
	o.cookiesBrowser = s.value(kGrp + QStringLiteral("cookiesBrowser")).toString();
	o.cookiesFile = s.value(kGrp + QStringLiteral("cookiesFile")).toString();
	o.downloadDir = s.value(kGrp + QStringLiteral("downloadDir")).toString();
	o.extraArgs = s.value(kGrp + QStringLiteral("extraArgs")).toString();
	o.preferMp4 = s.value(kGrp + QStringLiteral("preferMp4"), true).toBool();
	return o;
}

void YtDlpSettings::save() const
{
	QSettings s = settings();
	s.setValue(kGrp + QStringLiteral("exePath"), exePath);
	s.setValue(kGrp + QStringLiteral("cookiesBrowser"), cookiesBrowser);
	s.setValue(kGrp + QStringLiteral("cookiesFile"), cookiesFile);
	s.setValue(kGrp + QStringLiteral("downloadDir"), downloadDir);
	s.setValue(kGrp + QStringLiteral("extraArgs"), extraArgs);
	s.setValue(kGrp + QStringLiteral("preferMp4"), preferMp4);
}

QStringList YtDlpSettings::cookieBrowsers()
{
	return {QStringLiteral("chrome"), QStringLiteral("firefox"), QStringLiteral("edge"),
		QStringLiteral("brave"), QStringLiteral("chromium"), QStringLiteral("opera"),
		QStringLiteral("vivaldi"), QStringLiteral("safari")};
}

QString YtDlpSettings::defaultDownloadDir()
{
	const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	return (movies.isEmpty() ? QDir::homePath() : movies) + QStringLiteral("/Harpia Downloads");
}

// ---- locating -----------------------------------------------------------------

QString YtDlp::locate(const YtDlpSettings &s)
{
	if (!s.exePath.trimmed().isEmpty()) {
		const QFileInfo fi(s.exePath.trimmed());
		if (fi.isFile())
			return fi.absoluteFilePath();
		// A folder: look for the binary in it.
		if (fi.isDir()) {
			for (const QString &n : {QStringLiteral("yt-dlp.exe"), QStringLiteral("yt-dlp")}) {
				const QFileInfo in(fi.absoluteFilePath() + QLatin1Char('/') + n);
				if (in.isFile())
					return in.absoluteFilePath();
			}
		}
		return QString(); // set but wrong: say so rather than silently use another
	}
	QString found = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
	if (found.isEmpty())
		found = QStandardPaths::findExecutable(QStringLiteral("yt-dlp.exe"));
	return found;
}

bool YtDlp::probe(bool force)
{
	Probe &p = probeState();
	if (p.done && !force)
		return p.ok;
	p = Probe();
	p.done = true;
	p.path = locate(YtDlpSettings::load());
	if (p.path.isEmpty())
		return false;
	QProcess proc;
	proc.setProgram(p.path);
	proc.setArguments({QStringLiteral("--version")});
	proc.start();
	if (!proc.waitForStarted(3000) || !proc.waitForFinished(8000))
		return false;
	p.version = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
	p.ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0 && !p.version.isEmpty();
	return p.ok;
}

bool YtDlp::available()
{
	return probe(false);
}

QString YtDlp::version()
{
	return probeState().version;
}

QString YtDlp::foundPath()
{
	return probeState().path;
}

bool YtDlp::looksLikeUrl(const QString &text)
{
	const QString t = text.trimmed();
	if (!(t.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
	      t.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)))
		return false;
	const QUrl u(t);
	return u.isValid() && !u.host().isEmpty() && !t.contains(QLatin1Char(' '));
}

// ---- arguments ------------------------------------------------------------------

QStringList YtDlp::cookieArgs(const YtDlpSettings &s)
{
	QStringList a;
	if (!s.cookiesBrowser.trimmed().isEmpty())
		a << QStringLiteral("--cookies-from-browser") << s.cookiesBrowser.trimmed();
	else if (!s.cookiesFile.trimmed().isEmpty())
		a << QStringLiteral("--cookies") << s.cookiesFile.trimmed();
	return a;
}

QStringList YtDlp::infoArgs(const QString &url, const YtDlpSettings &s)
{
	QStringList a;
	a << QStringLiteral("-J") << QStringLiteral("--no-playlist") << QStringLiteral("--no-warnings");
	a << cookieArgs(s);
	a << QProcess::splitCommand(s.extraArgs);
	a << QStringLiteral("--") << url.trimmed();
	return a;
}

QString YtDlp::formatSelector(const YtDownloadOptions &o)
{
	// bv* = best video-only stream, ba = best audio-only, b = best combined.
	// The "/" alternatives are fallbacks, so a site with only combined files
	// still downloads. With audio off, only video streams are wanted at all.
	const QString h = o.maxHeight > 0 ? QStringLiteral("[height<=%1]").arg(o.maxHeight) : QString();
	if (o.includeAudio)
		return QStringLiteral("bv*%1+ba/b%1/bv*+ba/b").arg(h);
	return QStringLiteral("bv*%1/bv*/b%1").arg(h);
}

QStringList YtDlp::downloadArgs(const QString &url, const YtDlpSettings &s, const YtDownloadOptions &o)
{
	QStringList a;
	a << QStringLiteral("--no-playlist") << QStringLiteral("--no-warnings") << QStringLiteral("--newline")
	  << QStringLiteral("--progress");
	a << QStringLiteral("-f") << formatSelector(o);
	if (s.preferMp4)
		a << QStringLiteral("--merge-output-format") << QStringLiteral("mp4");
	const QString dir = o.outDir.isEmpty() ? YtDlpSettings::defaultDownloadDir() : o.outDir;
	a << QStringLiteral("-o") << QDir(dir).filePath(QStringLiteral("%(title).80s [%(id)s].%(ext)s"));
	if (!o.printPathTo.isEmpty())
		a << QStringLiteral("--print-to-file") << QStringLiteral("after_move:filepath") << o.printPathTo;
	if (!o.subtitleLang.isEmpty())
		a << QStringLiteral("--write-subs") << QStringLiteral("--write-auto-subs") << QStringLiteral("--sub-langs")
		  << o.subtitleLang << QStringLiteral("--convert-subs") << QStringLiteral("srt");
	a << cookieArgs(s);
	a << QProcess::splitCommand(s.extraArgs);
	a << QStringLiteral("--") << url.trimmed();
	return a;
}

// ---- parsing ----------------------------------------------------------------------

QString YtVideoInfo::durationText() const
{
	if (durationS <= 0)
		return QString();
	const qint64 h = durationS / 3600, m = (durationS / 60) % 60, s = durationS % 60;
	if (h > 0)
		return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
	return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

YtVideoInfo YtDlp::parseInfo(const QByteArray &json, QString *err)
{
	YtVideoInfo v;
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
	if (doc.isNull() || !doc.isObject()) {
		if (err)
			*err = QStringLiteral("yt-dlp did not return video information (%1)").arg(pe.errorString());
		return v;
	}
	QJsonObject o = doc.object();
	// A playlist page, despite --no-playlist: take its first entry.
	if (o.value(QStringLiteral("_type")).toString() == QLatin1String("playlist")) {
		const QJsonArray entries = o.value(QStringLiteral("entries")).toArray();
		if (entries.isEmpty()) {
			if (err)
				*err = QStringLiteral("that is a playlist with no videos");
			return v;
		}
		o = entries.first().toObject();
	}
	v.id = o.value(QStringLiteral("id")).toString();
	v.title = o.value(QStringLiteral("title")).toString();
	v.channel = o.value(QStringLiteral("channel")).toString();
	if (v.channel.isEmpty())
		v.channel = o.value(QStringLiteral("uploader")).toString();
	v.webpage = o.value(QStringLiteral("webpage_url")).toString();
	v.thumbnailUrl = o.value(QStringLiteral("thumbnail")).toString();
	v.description = o.value(QStringLiteral("description")).toString();
	v.uploadDate = o.value(QStringLiteral("upload_date")).toString();
	v.durationS = qint64(o.value(QStringLiteral("duration")).toDouble());
	v.viewCount = o.contains(QStringLiteral("view_count")) && !o.value(QStringLiteral("view_count")).isNull()
			      ? qint64(o.value(QStringLiteral("view_count")).toDouble())
			      : -1;
	for (const QString &k : o.value(QStringLiteral("subtitles")).toObject().keys())
		v.subtitleLangs << k;
	for (const QString &k : o.value(QStringLiteral("automatic_captions")).toObject().keys())
		v.autoCaptionLangs << k;
	v.subtitleLangs.sort();
	v.autoCaptionLangs.sort();

	// Qualities: one row per height, the highest frame rate at that height,
	// the size of the stream it would pick. Audio-only streams are not a
	// quality; storyboards (mhtml) and formats with no height are skipped.
	struct Best {
		double fps = 0;
		QString ext;
		qint64 size = -1;
	};
	QMap<int, Best> byHeight;
	for (const QJsonValue &fv : o.value(QStringLiteral("formats")).toArray()) {
		const QJsonObject f = fv.toObject();
		const QString vcodec = f.value(QStringLiteral("vcodec")).toString();
		const int h = f.value(QStringLiteral("height")).toInt();
		const QString ext = f.value(QStringLiteral("ext")).toString();
		if (h <= 0 || vcodec == QLatin1String("none") || ext == QLatin1String("mhtml"))
			continue;
		const double fps = f.value(QStringLiteral("fps")).toDouble();
		qint64 size = -1;
		if (f.contains(QStringLiteral("filesize")) && !f.value(QStringLiteral("filesize")).isNull())
			size = qint64(f.value(QStringLiteral("filesize")).toDouble());
		else if (f.contains(QStringLiteral("filesize_approx")) && !f.value(QStringLiteral("filesize_approx")).isNull())
			size = qint64(f.value(QStringLiteral("filesize_approx")).toDouble());
		Best &b = byHeight[h];
		if (fps > b.fps || b.ext.isEmpty()) {
			if (fps >= b.fps) {
				b.fps = fps;
				b.ext = ext;
				b.size = size;
			}
		}
	}
	YtQuality best;
	best.label = QStringLiteral("Best available");
	best.height = 0;
	v.qualities.append(best);
	QList<int> heights = byHeight.keys();
	std::sort(heights.begin(), heights.end(), std::greater<int>());
	for (int h : heights) {
		const Best &b = byHeight[h];
		YtQuality q;
		q.height = h;
		q.fps = b.fps;
		q.ext = b.ext;
		q.sizeBytes = b.size;
		q.label = QStringLiteral("%1p").arg(h);
		if (b.fps >= 49)
			q.label += QString::number(int(std::lround(b.fps)));
		v.qualities.append(q);
	}
	if (!v.valid() && err && err->isEmpty()) {
		const QJsonObject e = o;
		*err = e.contains(QStringLiteral("error")) ? e.value(QStringLiteral("error")).toString()
							    : QStringLiteral("no video information in the reply");
	}
	return v;
}

bool YtDlp::parseProgress(const QString &line, double *percent, QString *detail)
{
	static const QRegularExpression re(
		QStringLiteral("\\[download\\]\\s+([0-9]+(?:\\.[0-9]+)?)%(?:\\s+of\\s+~?\\s*(\\S+))?(?:\\s+at\\s+(\\S+))?(?:\\s+ETA\\s+(\\S+))?"));
	const QRegularExpressionMatch m = re.match(line);
	if (!m.hasMatch())
		return false;
	if (percent)
		*percent = m.captured(1).toDouble();
	if (detail) {
		QStringList parts;
		if (!m.captured(2).isEmpty())
			parts << QStringLiteral("of %1").arg(m.captured(2));
		if (!m.captured(3).isEmpty() && m.captured(3) != QLatin1String("Unknown"))
			parts << QStringLiteral("at %1").arg(m.captured(3));
		if (!m.captured(4).isEmpty() && m.captured(4) != QLatin1String("Unknown"))
			parts << QStringLiteral("ETA %1").arg(m.captured(4));
		*detail = parts.join(QStringLiteral("  "));
	}
	return true;
}

} // namespace harpia
