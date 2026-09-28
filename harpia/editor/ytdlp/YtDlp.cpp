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
#include <QThread>
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

QString browserLabel(const QString &id)
{
	if (id == QLatin1String("chrome"))
		return QStringLiteral("Google Chrome");
	if (id == QLatin1String("firefox"))
		return QStringLiteral("Firefox");
	if (id == QLatin1String("edge"))
		return QStringLiteral("Microsoft Edge");
	if (id == QLatin1String("brave"))
		return QStringLiteral("Brave");
	if (id == QLatin1String("chromium"))
		return QStringLiteral("Chromium");
	if (id == QLatin1String("opera"))
		return QStringLiteral("Opera");
	if (id == QLatin1String("vivaldi"))
		return QStringLiteral("Vivaldi");
	if (id == QLatin1String("safari"))
		return QStringLiteral("Safari");
	return id;
}

QStringList browserProfileDirs(const QString &id, const QString &os, const QString &home,
			       const QString &localAppData, const QString &roamingAppData)
{
	QStringList d;
	const bool win = os == QLatin1String("windows");
	const bool mac = os == QLatin1String("macos");
	const QString macApp = home + QStringLiteral("/Library/Application Support");
	auto chromium = [&](const QString &winSub, const QString &macSub, const QString &linuxSub) {
		if (win)
			d << localAppData + QLatin1Char('/') + winSub + QStringLiteral("/User Data");
		else if (mac)
			d << macApp + QLatin1Char('/') + macSub;
		else
			d << home + QStringLiteral("/.config/") + linuxSub
			  << home + QStringLiteral("/snap/") + linuxSub.section(QLatin1Char('/'), 0, 0) +
				     QStringLiteral("/current/.config/") + linuxSub
			  << home + QStringLiteral("/.var/app/") + linuxSub; // flatpak, loosely
	};
	if (id == QLatin1String("chrome"))
		chromium(QStringLiteral("Google/Chrome"), QStringLiteral("Google/Chrome"), QStringLiteral("google-chrome"));
	else if (id == QLatin1String("edge"))
		chromium(QStringLiteral("Microsoft/Edge"), QStringLiteral("Microsoft Edge"), QStringLiteral("microsoft-edge"));
	else if (id == QLatin1String("brave"))
		chromium(QStringLiteral("BraveSoftware/Brave-Browser"), QStringLiteral("BraveSoftware/Brave-Browser"),
			 QStringLiteral("BraveSoftware/Brave-Browser"));
	else if (id == QLatin1String("chromium"))
		chromium(QStringLiteral("Chromium"), QStringLiteral("Chromium"), QStringLiteral("chromium"));
	else if (id == QLatin1String("vivaldi"))
		chromium(QStringLiteral("Vivaldi"), QStringLiteral("Vivaldi"), QStringLiteral("vivaldi"));
	else if (id == QLatin1String("opera")) {
		if (win)
			d << roamingAppData + QStringLiteral("/Opera Software/Opera Stable")
			  << roamingAppData + QStringLiteral("/Opera Software/Opera GX Stable");
		else if (mac)
			d << macApp + QStringLiteral("/com.operasoftware.Opera");
		else
			d << home + QStringLiteral("/.config/opera");
	} else if (id == QLatin1String("firefox")) {
		if (win)
			d << roamingAppData + QStringLiteral("/Mozilla/Firefox/Profiles");
		else if (mac)
			d << macApp + QStringLiteral("/Firefox/Profiles");
		else
			d << home + QStringLiteral("/.mozilla/firefox")
			  << home + QStringLiteral("/snap/firefox/common/.mozilla/firefox");
	} else if (id == QLatin1String("safari")) {
		if (mac)
			d << home + QStringLiteral("/Library/Cookies") << home + QStringLiteral("/Library/Containers/com.apple.Safari");
	}
	return d;
}

QVector<YtBrowser> detectBrowsers()
{
#if defined(_WIN32)
	const QString os = QStringLiteral("windows");
#elif defined(__APPLE__)
	const QString os = QStringLiteral("macos");
#else
	const QString os = QStringLiteral("linux");
#endif
	const QString home = QDir::homePath();
	const QString local = qEnvironmentVariable("LOCALAPPDATA");
	const QString roaming = qEnvironmentVariable("APPDATA");
	QVector<YtBrowser> out;
	for (const QString &id : YtDlpSettings::cookieBrowsers()) {
		YtBrowser b;
		b.id = id;
		b.label = browserLabel(id);
		for (const QString &dir : browserProfileDirs(id, os, home, local, roaming))
			if (!dir.isEmpty() && QDir(dir).exists()) {
				b.found = true;
				break;
			}
		out.append(b);
	}
	// The ones on this machine first, so the pick is the first row or two.
	std::stable_sort(out.begin(), out.end(), [](const YtBrowser &a, const YtBrowser &b) {
		return a.found && !b.found;
	});
	return out;
}

QStringList browserProcessNames(const QString &id, const QString &os)
{
	const bool win = os == QLatin1String("windows");
	const bool mac = os == QLatin1String("macos");
	auto pick = [&](const char *w, const char *m, const char *l) {
		return QStringList{QString::fromLatin1(win ? w : mac ? m : l)};
	};
	if (id == QLatin1String("chrome"))
		return pick("chrome.exe", "Google Chrome", "chrome");
	if (id == QLatin1String("edge"))
		return pick("msedge.exe", "Microsoft Edge", "msedge");
	if (id == QLatin1String("brave"))
		return pick("brave.exe", "Brave Browser", "brave");
	if (id == QLatin1String("chromium"))
		return pick("chromium.exe", "Chromium", "chromium");
	if (id == QLatin1String("vivaldi"))
		return pick("vivaldi.exe", "Vivaldi", "vivaldi-bin");
	if (id == QLatin1String("opera"))
		return pick("opera.exe", "Opera", "opera");
	if (id == QLatin1String("firefox"))
		return pick("firefox.exe", "firefox", "firefox");
	if (id == QLatin1String("safari"))
		return mac ? QStringList{QStringLiteral("Safari")} : QStringList{};
	return {};
}

bool browserLocksCookies(const QString &id)
{
	// Firefox and Safari read fine while open; the Chromium family holds an
	// exclusive lock on its cookie database (and Windows will not let anyone
	// else copy a locked file).
	return id == QLatin1String("chrome") || id == QLatin1String("edge") || id == QLatin1String("brave") ||
	       id == QLatin1String("chromium") || id == QLatin1String("vivaldi") || id == QLatin1String("opera");
}

namespace {
QString hostOs()
{
#if defined(_WIN32)
	return QStringLiteral("windows");
#elif defined(__APPLE__)
	return QStringLiteral("macos");
#else
	return QStringLiteral("linux");
#endif
}
} // namespace

bool browserRunning(const QString &id)
{
	for (const QString &name : browserProcessNames(id, hostOs())) {
		QProcess p;
#if defined(_WIN32)
		p.setProgram(QStringLiteral("tasklist"));
		p.setArguments({QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq %1").arg(name), QStringLiteral("/NH"),
				QStringLiteral("/FO"), QStringLiteral("CSV")});
		p.start();
		if (!p.waitForFinished(4000))
			continue;
		// tasklist prints the rows as CSV, or "INFO: No tasks are running..."
		const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
		if (out.contains(QLatin1Char('"') + name, Qt::CaseInsensitive))
			return true;
#else
		p.setProgram(QStringLiteral("pgrep"));
		p.setArguments({QStringLiteral("-x"), name});
		p.start();
		if (p.waitForFinished(4000) && p.exitCode() == 0)
			return true;
#endif
	}
	return false;
}

bool closeBrowser(const QString &id)
{
	for (const QString &name : browserProcessNames(id, hostOs())) {
		QProcess p;
#if defined(_WIN32)
		// /F: Chrome keeps helper processes that ignore a polite close, and
		// it restores the tabs on its next start anyway.
		p.setProgram(QStringLiteral("taskkill"));
		p.setArguments({QStringLiteral("/IM"), name, QStringLiteral("/F"), QStringLiteral("/T")});
#else
		p.setProgram(QStringLiteral("pkill"));
		p.setArguments({QStringLiteral("-x"), name});
#endif
		p.start();
		p.waitForFinished(8000);
	}
	// The lock goes when the last process has gone; give it a moment.
	for (int i = 0; i < 20; ++i) {
		if (!browserRunning(id))
			return true;
		QThread::msleep(150);
	}
	return false;
}

bool isCookieReadError(const QString &error)
{
	const bool cookie = error.contains(QLatin1String("cookie"), Qt::CaseInsensitive);
	return (cookie && error.contains(QLatin1String("Could not copy"), Qt::CaseInsensitive)) ||
	       (cookie && error.contains(QLatin1String("could not find"), Qt::CaseInsensitive)) ||
	       error.contains(QLatin1String("Failed to decrypt"), Qt::CaseInsensitive) ||
	       error.contains(QLatin1String("DPAPI"), Qt::CaseInsensitive) ||
	       error.contains(QLatin1String("app-bound"), Qt::CaseInsensitive) ||
	       error.contains(QLatin1String("app_bound"), Qt::CaseInsensitive);
}

QString downloadErrorHint(const QString &error, const QString &cookiesBrowser, bool haveCookiesFile)
{
	const QString label = cookiesBrowser.isEmpty() ? QString() : browserLabel(cookiesBrowser);
	// yt-dlp could not read the browser's cookies at all: the file is locked
	// (browser open) or, in Chrome 127 and later, encrypted so that only
	// Chrome itself can open it. Either way a cookies.txt always works.
	if (error.contains(QLatin1String("Could not copy"), Qt::CaseInsensitive) &&
	    error.contains(QLatin1String("cookie"), Qt::CaseInsensitive)) {
		return QStringLiteral("%1 is open and locks its cookie file, so yt-dlp cannot read it.\n"
				      "Close %1 completely (also its background apps in the system tray) and try again, "
				      "or choose \"cookies.txt file…\" under Cookies: export the file with the "
				      "\"Get cookies.txt LOCALLY\" browser extension while you are signed in, then point at it. "
				      "That works with the browser open.")
			.arg(label.isEmpty() ? QStringLiteral("The browser") : label);
	}
	if ((error.contains(QLatin1String("decrypt"), Qt::CaseInsensitive) ||
	     error.contains(QLatin1String("DPAPI"), Qt::CaseInsensitive) ||
	     error.contains(QLatin1String("app-bound"), Qt::CaseInsensitive) ||
	     error.contains(QLatin1String("app_bound"), Qt::CaseInsensitive)) &&
	    !cookiesBrowser.isEmpty()) {
		return QStringLiteral("%1 encrypts its cookies so that only it can read them (Chrome 127 and later), "
				      "and yt-dlp cannot decrypt them.\nChoose \"cookies.txt file…\" under Cookies: export the "
				      "file with the \"Get cookies.txt LOCALLY\" extension while signed in, then point at it. "
				      "Firefox cookies read directly without this trouble.")
			.arg(label);
	}
	if (error.contains(QLatin1String("could not find"), Qt::CaseInsensitive) &&
	    error.contains(QLatin1String("cookies"), Qt::CaseInsensitive)) {
		return QStringLiteral("yt-dlp found no cookie database for %1. Is that the browser you use, and "
				      "has it a profile on this computer? Pick another under Cookies.")
			.arg(label.isEmpty() ? QStringLiteral("that browser") : label);
	}
	if (error.contains(QLatin1String("403")) || error.contains(QLatin1String("Sign in"), Qt::CaseInsensitive) ||
	    error.contains(QLatin1String("login"), Qt::CaseInsensitive) ||
	    error.contains(QLatin1String("cookies"), Qt::CaseInsensitive)) {
		if (cookiesBrowser.isEmpty() && !haveCookiesFile)
			return QStringLiteral("The site wants a login. Pick the browser you are signed in with under "
					      "Cookies and try again.");
		if (!cookiesBrowser.isEmpty())
			return QStringLiteral("The site still wants a login. Make sure you are signed in in %1 and "
					      "try again, or use \"cookies.txt file…\" exported while signed in. yt-dlp "
					      "itself may also need updating.")
				.arg(label);
		return QStringLiteral("The site still wants a login. Export the cookies.txt again while you are "
				      "signed in (they expire), and check that yt-dlp is up to date.");
	}
	return QString();
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
	// Audio only: the best audio stream, an .m4a when the site has one (it
	// plays everywhere); a combined file as the last resort.
	if (o.audioOnly)
		return QStringLiteral("ba[ext=m4a]/ba/b");
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
	if (s.preferMp4 && !o.audioOnly) // nothing to merge into a video container
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
