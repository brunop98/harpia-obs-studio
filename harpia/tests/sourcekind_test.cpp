// Which reader a source file needs.
//
// This was a one-line ternary inside the project loader, and it was wrong for
// audio: everything that was not an image went to the video reader. An
// audio-only file has no video stream, so FrameSeeker refuses it outright --
// which meant a project could CREATE an audio source (a voiceover, an imported
// track) and never read it back. Relinking failed the same way: you were asked
// to find the file, you found it, and you were told it could not be opened.
//
// The rule is three lines long and impossible to check by looking at it, which
// is exactly the kind of thing that stays broken. Pinned here.
#include "editor/MediaFiles.hpp"

#include <QCoreApplication>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static const char *kindName(SourceKind k)
{
	switch (k) {
	case SourceKind::Image: return "Image";
	case SourceKind::Audio: return "Audio";
	default: return "Video";
	}
}
static void kind(const char *path, SourceKind want, const char *w)
{
	const SourceKind got = sourceKindFor(QString::fromLatin1(path));
	const bool good = got == want;
	std::printf("  %s %s (%s -> %s)\n", good ? "PASS" : "FAIL", w, path, kindName(got));
	if (!good)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- audio goes to the audio reader --\n");
	{
		// The bug, in one line. Every one of these used to be handed to
		// FrameSeeker, which cannot open a file with no video stream.
		for (const char *p : {"/m/voice.mp3", "/m/import_1.wav", "/m/take.m4a",
				      "/m/song.flac", "/m/pod.ogg", "/m/clip.opus", "/m/old.wma",
				      "/m/ring.aiff", "/m/track.mka", "/m/beep.aac"})
			kind(p, SourceKind::Audio, "audio-only file");
	}

	std::printf("\n-- and everything else where it went before --\n");
	{
		kind("/m/screen.mp4", SourceKind::Video, "a recording");
		kind("/m/take.mov", SourceKind::Video, "a QuickTime");
		kind("/m/cap.mkv", SourceKind::Video, "a Matroska");
		kind("/m/shot.png", SourceKind::Image, "a still");
		kind("/m/photo.JPG", SourceKind::Image, "case does not matter");
		kind("/m/logo.webp", SourceKind::Image, "a WebP");
	}

	std::printf("\n-- the overlaps, which is why the order is fixed --\n");
	{
		// A GIF is deliberately in both the video and image lists, and the
		// video reader is what people mean by one. If this ever flips, dropping
		// an animated GIF silently gives you its first frame.
		kind("/m/loop.gif", SourceKind::Video, "a GIF is a VIDEO, not a still");
		// .mka is audio-only; .mkv is not. Two Matroska extensions, two answers.
		kind("/m/audio.mka", SourceKind::Audio, "and Matroska audio is audio");
	}

	std::printf("\n-- and something it has never heard of --\n");
	{
		// Not a guess dressed up as a decision: hand it to the video reader,
		// which is what happened before this rule was written down, and let it
		// fail with a real error.
		kind("/m/thing.xyz", SourceKind::Video, "an unknown extension");
		kind("/m/noextension", SourceKind::Video, "and a file with no extension at all");
	}

	std::printf("\n-- the three questions still agree with each other --\n");
	{
		// sourceKindFor is built on these, so a change to one that did not
		// reach the others would show up here rather than in a project that
		// will not open.
		ok(isMediaFile(QStringLiteral("/m/voice.mp3")), "an mp3 is media");
		ok(isMediaFile(QStringLiteral("/m/loop.gif")), "so is a gif");
		ok(!isMediaFile(QStringLiteral("/m/notes.txt")), "a text file is not");
		ok(audioOpenFilter().contains(QStringLiteral("*.wav")),
		   "and the Locate dialog for a missing audio source offers .wav");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
