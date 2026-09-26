// yt-dlp from the editor: the two halves that can be held still.
//
// What yt-dlp prints for -J becomes the video's facts and the qualities the
// dropdown offers; the options become the exact argument list; a progress
// line becomes a percentage. The process itself is not run here -- there is
// no yt-dlp in the test container, which is also the case on a user's
// machine without it, and that case is checked too: locate() says no, and
// the menu entry is the caller's to hide.
#include "editor/ytdlp/YtDlp.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eqs(const QString &got, const char *want, const char *w)
{
	const bool good = got == QString::fromUtf8(want);
	std::printf("  %s %s (got \"%s\")\n", good ? "PASS" : "FAIL", w, qPrintable(got));
	if (!good)
		++failures;
}

static const char *kInfo = R"({
  "id": "dQw4w9WgXcQ", "title": "Never Gonna Give You Up", "channel": "Rick Astley",
  "uploader": "RickAstleyVEVO", "webpage_url": "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
  "thumbnail": "https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg",
  "duration": 213, "view_count": 1500000000, "upload_date": "20091025",
  "description": "The official video",
  "subtitles": {"en": [{"ext": "vtt"}], "pt": [{"ext": "vtt"}]},
  "automatic_captions": {"de": [{"ext": "vtt"}], "en": [{"ext": "vtt"}]},
  "formats": [
    {"format_id": "sb0", "ext": "mhtml", "height": 1080, "vcodec": "none"},
    {"format_id": "140", "ext": "m4a", "vcodec": "none", "acodec": "mp4a.40.2", "filesize": 3400000},
    {"format_id": "18",  "ext": "mp4", "height": 360, "fps": 30, "vcodec": "avc1", "acodec": "mp4a", "filesize": 12000000},
    {"format_id": "136", "ext": "mp4", "height": 720, "fps": 30, "vcodec": "avc1", "acodec": "none", "filesize": 40000000},
    {"format_id": "298", "ext": "mp4", "height": 720, "fps": 60, "vcodec": "avc1", "acodec": "none", "filesize_approx": 55000000},
    {"format_id": "137", "ext": "mp4", "height": 1080, "fps": 30, "vcodec": "avc1", "acodec": "none", "filesize": 90000000},
    {"format_id": "248", "ext": "webm", "height": 1080, "fps": 30, "vcodec": "vp9", "acodec": "none", "filesize": 80000000},
    {"format_id": "313", "ext": "webm", "height": 2160, "fps": 30, "vcodec": "vp9", "acodec": "none"}
  ]
})";

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- the facts of a video --\n");
	{
		QString err;
		const YtVideoInfo v = YtDlp::parseInfo(kInfo, &err);
		ok(v.valid() && err.isEmpty(), "the -J reply parses");
		eqs(v.title, "Never Gonna Give You Up", "title");
		eqs(v.channel, "Rick Astley", "channel (the channel field, not the uploader handle)");
		eqs(v.durationText(), "3:33", "duration as m:ss");
		ok(v.viewCount == 1500000000 && v.uploadDate == QStringLiteral("20091025"), "views and upload date");
		ok(v.thumbnailUrl.startsWith(QStringLiteral("https://i.ytimg.com")), "the thumbnail URL");
		eqs(v.subtitleLangs.join(","), "en,pt", "uploaded subtitle languages, sorted");
		eqs(v.autoCaptionLangs.join(","), "de,en", "and the generated ones");

		QStringList labels;
		for (const YtQuality &q : v.qualities)
			labels << q.label;
		eqs(labels.join(" | "), "Best available | 2160p | 1080p | 720p60 | 360p",
		    "qualities: best first, one per height, highest frame rate wins the label");
		ok(v.qualities[3].height == 720 && v.qualities[3].fps == 60 && v.qualities[3].sizeBytes == 55000000,
		   "720p60 carries the 60 fps stream's size");
		ok(v.qualities[1].sizeBytes == -1, "a size yt-dlp did not give is unknown, not zero");
		bool noAudioRow = true;
		for (const YtQuality &q : v.qualities)
			if (q.ext == QLatin1String("m4a") || q.ext == QLatin1String("mhtml"))
				noAudioRow = false;
		ok(noAudioRow, "audio-only and storyboard formats are not qualities");

		YtVideoInfo h;
		h.durationS = 3723;
		eqs(h.durationText(), "1:02:03", "an hour-long duration reads h:mm:ss");

		const YtVideoInfo bad = YtDlp::parseInfo("ERROR: Video unavailable", &err);
		ok(!bad.valid() && !err.isEmpty(), "a non-JSON reply is an error, not a crash");
		err.clear();
		const YtVideoInfo pl = YtDlp::parseInfo(R"({"_type":"playlist","entries":[{"id":"x1","title":"First","formats":[]}]})", &err);
		eqs(pl.title, "First", "a playlist reply yields its first video");
	}

	std::printf("\n-- the argument list --\n");
	{
		YtDlpSettings s;
		s.preferMp4 = true;
		YtDownloadOptions o;
		o.maxHeight = 1080;
		o.includeAudio = true;
		o.outDir = QStringLiteral("/tmp/dl");
		o.printPathTo = QStringLiteral("/tmp/dl/.path");
		const QStringList a = YtDlp::downloadArgs(QStringLiteral("https://youtu.be/abc"), s, o);
		const QString joined = a.join(QLatin1Char(' '));
		std::printf("     %s\n", qPrintable(joined));
		eqs(YtDlp::formatSelector(o), "bv*[height<=1080]+ba/b[height<=1080]/bv*+ba/b",
		    "1080p with audio: best video up to 1080 plus best audio, with fallbacks");
		ok(a.contains(QStringLiteral("--merge-output-format")) && a.contains(QStringLiteral("mp4")), "merged into mp4");
		ok(a.contains(QStringLiteral("--newline")) && a.contains(QStringLiteral("--progress")), "progress, a line at a time");
		ok(a.contains(QStringLiteral("--no-playlist")), "one video, never the playlist it is in");
		ok(a.contains(QStringLiteral("--print-to-file")) && a.contains(QStringLiteral("after_move:filepath")),
		   "the final path is written to a file for the caller");
		ok(a.last() == QStringLiteral("https://youtu.be/abc") && a[a.size() - 2] == QStringLiteral("--"),
		   "the URL comes last, after --, so a URL starting with a dash cannot be an option");
		ok(!a.contains(QStringLiteral("--write-subs")), "no subtitles unless asked");

		o.includeAudio = false;
		o.maxHeight = 0;
		eqs(YtDlp::formatSelector(o), "bv*/bv*/b", "best, video only");
		o.maxHeight = 720;
		eqs(YtDlp::formatSelector(o), "bv*[height<=720]/bv*/b[height<=720]", "720p, video only");

		o.subtitleLang = QStringLiteral("pt");
		const QStringList b = YtDlp::downloadArgs(QStringLiteral("https://x/y"), s, o);
		ok(b.contains(QStringLiteral("--sub-langs")) && b[b.indexOf(QStringLiteral("--sub-langs")) + 1] == QStringLiteral("pt") &&
			   b.contains(QStringLiteral("--convert-subs")),
		   "a subtitle language asks for that language, converted to srt");

		s.cookiesBrowser = QStringLiteral("firefox");
		const QStringList c = YtDlp::infoArgs(QStringLiteral("https://x/y"), s);
		ok(c.first() == QStringLiteral("-J") && c.contains(QStringLiteral("--cookies-from-browser")) &&
			   c[c.indexOf(QStringLiteral("--cookies-from-browser")) + 1] == QStringLiteral("firefox"),
		   "info: -J with the browser's cookies");
		s.cookiesBrowser.clear();
		s.cookiesFile = QStringLiteral("/home/me/cookies.txt");
		const QStringList d = YtDlp::infoArgs(QStringLiteral("https://x/y"), s);
		ok(d.contains(QStringLiteral("--cookies")) && !d.contains(QStringLiteral("--cookies-from-browser")),
		   "a cookies file when no browser is chosen");
		s.extraArgs = QStringLiteral("--proxy \"http://127.0.0.1:8080\" -4");
		const QStringList e = YtDlp::downloadArgs(QStringLiteral("https://x/y"), s, o);
		ok(e.contains(QStringLiteral("--proxy")) && e.contains(QStringLiteral("http://127.0.0.1:8080")) && e.contains(QStringLiteral("-4")),
		   "extra arguments are split like a shell would and passed through");
		s.preferMp4 = false;
		ok(!YtDlp::downloadArgs(QStringLiteral("https://x/y"), s, o).contains(QStringLiteral("--merge-output-format")),
		   "no merge format when mp4 is not preferred");
	}

	std::printf("\n-- progress lines --\n");
	{
		double pc = -1;
		QString detail;
		ok(YtDlp::parseProgress(QStringLiteral("[download]  45.3% of 12.34MiB at 1.20MiB/s ETA 00:05"), &pc, &detail) &&
			   std::abs(pc - 45.3) < 1e-9,
		   "a mid-download line gives its percentage");
		eqs(detail, "of 12.34MiB  at 1.20MiB/s  ETA 00:05", "and the rest as a detail");
		ok(YtDlp::parseProgress(QStringLiteral("[download] 100% of   12.34MiB in 00:00:10 at 1.2MiB/s"), &pc, &detail) && pc == 100.0,
		   "the final line is 100");
		ok(YtDlp::parseProgress(QStringLiteral("[download]   0.0% of ~ 55.00MiB at  Unknown B/s ETA Unknown"), &pc, &detail) &&
			   pc == 0.0 && !detail.contains(QStringLiteral("Unknown")),
		   "an approximate size parses, and Unknown rates are left out");
		ok(!YtDlp::parseProgress(QStringLiteral("[Merger] Merging formats into \"x.mp4\""), &pc, &detail),
		   "a merger line is not progress");
		ok(!YtDlp::parseProgress(QStringLiteral("[download] Destination: x.f137.mp4"), &pc, &detail),
		   "nor is the destination line");
	}

	std::printf("\n-- finding it, and URLs --\n");
	{
		QTemporaryDir dir;
		YtDlpSettings s;
		s.exePath = dir.filePath(QStringLiteral("nowhere/yt-dlp"));
		ok(YtDlp::locate(s).isEmpty(), "a set path that does not exist finds nothing (and does not fall back)");
		QFile f(dir.filePath(QStringLiteral("yt-dlp")));
		f.open(QIODevice::WriteOnly);
		f.write("#!/bin/sh\necho 2026.09.01\n");
		f.close();
		s.exePath = dir.filePath(QStringLiteral("yt-dlp"));
		eqs(YtDlp::locate(s), qPrintable(QFileInfo(s.exePath).absoluteFilePath()), "a set file path is used as is");
		s.exePath = dir.path();
		ok(YtDlp::locate(s) == QFileInfo(dir.filePath(QStringLiteral("yt-dlp"))).absoluteFilePath(),
		   "a folder is searched for the binary");
		s.exePath.clear();
		std::printf("     on the PATH here: \"%s\"\n", qPrintable(YtDlp::locate(s)));

		ok(YtDlp::looksLikeUrl(QStringLiteral("  https://www.youtube.com/watch?v=abc ")), "an https link is a URL");
		ok(YtDlp::looksLikeUrl(QStringLiteral("http://vimeo.com/123")), "http too");
		ok(!YtDlp::looksLikeUrl(QStringLiteral("youtube.com/watch")), "no scheme, no URL");
		ok(!YtDlp::looksLikeUrl(QStringLiteral("some text I copied")), "prose is not a URL");
		ok(!YtDlp::looksLikeUrl(QStringLiteral("https://")), "a bare scheme is not one either");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
