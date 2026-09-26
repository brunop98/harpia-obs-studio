#pragma once

// yt-dlp, from the editor: paste a link, see what it is, choose a quality,
// download, and the file lands on the timeline.
//
// yt-dlp is not bundled and not required. It is the user's own copy, found on
// the PATH (or at a path they set), probed once at startup; the Add menu only
// offers "Video from URL…" when it was found. Everything the program does
// with it is an argument list and a parse of what it prints, and those two
// halves are pure functions here so a test can hold them still: the JSON of
// `-J` becomes a YtVideoInfo with the qualities actually on offer, an options
// struct becomes the exact argv, and a progress line becomes a percentage.
// The dialog (UrlDownloadDialog) only runs the process.
//
// First version. Deliberately small: one video at a time (no playlists), the
// subtitle file is downloaded beside the video but not yet placed on the
// timeline, and cookies are the only site-login knob.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace harpia {

struct YtDlpSettings {
	QString exePath;        // "" = look on the PATH
	QString cookiesBrowser; // "" or one of cookieBrowsers(): --cookies-from-browser
	QString cookiesFile;    // a cookies.txt: --cookies
	QString downloadDir;    // "" = the default download folder
	QString extraArgs;      // appended verbatim (split like a shell would)
	bool preferMp4 = true;  // --merge-output-format mp4

	static YtDlpSettings load();
	void save() const;
	static QStringList cookieBrowsers(); // the names yt-dlp accepts
	static QString defaultDownloadDir();
};

// One quality on offer, one row of the dropdown.
struct YtQuality {
	QString label;    // "1080p60", "720p", "Best available"
	int height = 0;   // 0 = best
	double fps = 0.0;
	QString ext;
	qint64 sizeBytes = -1; // -1 = unknown
};

struct YtVideoInfo {
	QString id;
	QString title;
	QString channel;
	QString webpage;
	QString thumbnailUrl;
	QString description;
	QString uploadDate; // "20260914" as yt-dlp gives it
	qint64 durationS = 0;
	qint64 viewCount = -1;
	QVector<YtQuality> qualities;     // best first; the first is "Best available"
	QStringList subtitleLangs;        // uploaded subtitles
	QStringList autoCaptionLangs;     // generated ones
	bool valid() const { return !title.isEmpty() || !id.isEmpty(); }
	QString durationText() const;     // "12:34" / "1:02:03"
};

struct YtDownloadOptions {
	int maxHeight = 0;         // 0 = best
	bool includeAudio = true;
	QString subtitleLang;      // "" = none
	QString outDir;
	QString printPathTo;       // a file yt-dlp writes the final path into
};

class YtDlp {
public:
	// The executable to run, or "" when there is none. `settings.exePath`
	// wins when it points at a file; otherwise the PATH is searched for
	// yt-dlp (yt-dlp.exe on Windows).
	static QString locate(const YtDlpSettings &settings);

	// Startup probe: locate, and run --version. Cached; available() answers
	// from the cache without a process. `force` re-probes (Settings "Test").
	static bool probe(bool force = false);
	static bool available();
	static QString version();  // "" until a probe found one
	static QString foundPath();

	// True when `text` reads as something yt-dlp could take: an http(s) URL.
	static bool looksLikeUrl(const QString &text);

	// Argument lists. Both take the settings so cookies and extras apply to
	// every call the same way.
	static QStringList infoArgs(const QString &url, const YtDlpSettings &s);
	static QStringList downloadArgs(const QString &url, const YtDlpSettings &s, const YtDownloadOptions &o);

	// The format selector downloadArgs uses, on its own for the test.
	static QString formatSelector(const YtDownloadOptions &o);

	// `-J` output -> info. *err carries yt-dlp's own message when the JSON is
	// an error object, or why the bytes were not usable.
	static YtVideoInfo parseInfo(const QByteArray &json, QString *err = nullptr);

	// One line of --newline progress. True when it carried a percentage.
	static bool parseProgress(const QString &line, double *percent, QString *detail);

private:
	static QStringList cookieArgs(const YtDlpSettings &s);
};

} // namespace harpia
