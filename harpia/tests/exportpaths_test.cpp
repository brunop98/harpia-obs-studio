// All four export paths still produce a playable file after the encoder/muxer
// setup was pulled into one place -- and all four now put the MP4 index at the
// front, which only three of them used to do.
#include "editor/ClipExporter.hpp"
#include "editor/timeline/TimelineModel.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w) {
    std::printf("  %s %s\n", c ? "PASS" : "FAIL", w); if (!c) ++failures;
}
static QString g_mediaDir = QStringLiteral(".");
static QString RED  = g_mediaDir + QStringLiteral("/av_red.mp4");
static QString BLUE = g_mediaDir + QStringLiteral("/av_blue.mp4");

static double durationOf(const QString &f) {
    QProcess p; p.start(QStringLiteral("ffprobe"),
        {"-v","error","-show_entries","format=duration","-of","csv=p=0", f});
    p.waitForFinished(20000);
    return p.readAllStandardOutput().trimmed().toDouble();
}
// +faststart puts `moov` before `mdat`. Reading the box order is the only way to
// tell; ffprobe will happily play either.
static bool moovFirst(const QString &f) {
    QFile fh(f); if (!fh.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = fh.read(4 * 1024 * 1024);
    const int moov = head.indexOf("moov"), mdat = head.indexOf("mdat");
    return moov >= 0 && (mdat < 0 || moov < mdat);
}
static bool runExport(const QString &in, const QString &out, ClipExporter::Options o, QString *err) {
    ClipExporter ex; bool done = false, good = false;
    QObject::connect(&ex, &ClipExporter::finished, [&](bool g, bool c, const QString &e) {
        done = true; good = g && !c; if (err) *err = e; });
    ex.run(in, out, o);
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < 180000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50); QThread::msleep(20);
    }
    return good;
}
static void checkFile(const char *name, const QString &f, double wantSecs) {
    const double d = durationOf(f);
    const bool dur = std::abs(d - wantSecs) < 0.35;
    std::printf("  %s %-22s %.2fs (want %.2f)\n", dur ? "PASS" : "FAIL", name, d, wantSecs);
    if (!dur) ++failures;
    const bool ff = moovFirst(f);
    std::printf("  %s %-22s moov before mdat (+faststart)\n", ff ? "PASS" : "FAIL", name);
    if (!ff) ++failures;
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        g_mediaDir = QString::fromLocal8Bit(argv[1]);
        RED = g_mediaDir + QStringLiteral("/av_red.mp4");
        BLUE = g_mediaDir + QStringLiteral("/av_blue.mp4");
    }
    if (!QFile::exists(RED)) { std::printf("missing %s\n", qPrintable(RED)); return 2; }
    QTemporaryDir dir;
    QString err;

    std::printf("\n-- runVideo (simple trim) --\n");
    {
        ClipExporter::Options o; o.format = ClipExporter::Format::Mp4;
        o.startMs = 500; o.endMs = 2500; o.keepAudio = true;
        const QString f = dir.path() + "/trim.mp4";
        ok(runExport(RED, f, o, &err), "exports");
        if (!err.isEmpty()) std::printf("     err: %s\n", qPrintable(err));
        checkFile("trim", f, 2.0);
    }

    std::printf("\n-- runVideoCuts (multi-cut, one source) --\n");
    {
        ClipExporter::Options o; o.format = ClipExporter::Format::Mp4; o.keepAudio = true;
        ClipExporter::Cut c1; c1.startMs = 0;    c1.endMs = 1000; c1.speed = 1.0;
        ClipExporter::Cut c2; c2.startMs = 2000; c2.endMs = 3000; c2.speed = 1.0;
        o.cuts = {c1, c2};
        const QString f = dir.path() + "/cuts.mp4";
        ok(runExport(RED, f, o, &err), "exports");
        if (!err.isEmpty()) std::printf("     err: %s\n", qPrintable(err));
        checkFile("cuts", f, 2.0);
    }

    std::printf("\n-- runVideoCutsMulti (cuts across two sources) --\n");
    {
        ClipExporter::Options o; o.format = ClipExporter::Format::Mp4; o.keepAudio = true;
        o.inputs = {RED.toStdString(), BLUE.toStdString()};
        ClipExporter::Cut c1; c1.startMs = 0; c1.endMs = 1000; c1.speed = 1.0; c1.source = 0;
        ClipExporter::Cut c2; c2.startMs = 0; c2.endMs = 1000; c2.speed = 1.0; c2.source = 1;
        o.cuts = {c1, c2};
        const QString f = dir.path() + "/multi.mp4";
        ok(runExport(RED, f, o, &err), "exports");
        if (!err.isEmpty()) std::printf("     err: %s\n", qPrintable(err));
        checkFile("multi-source", f, 2.0);
    }

    std::printf("\n-- runTimeline (Full editing) --\n");
    {
        ClipExporter::Options o; o.format = ClipExporter::Format::Mp4; o.keepAudio = true;
        TlTrack t; t.kind = TlTrack::Kind::Video;
        TlClip a; a.type = TlClip::Type::Video; a.sourceId = 1;
        a.srcStartMs = 0; a.srcEndMs = 1000; a.outStartMs = 0;
        TlClip b = a; b.sourceId = 2; b.outStartMs = 1000;
        t.clips.append(a); t.clips.append(b);
        o.timeline.tracks.append(t);
        o.timelineSources = {{1, RED.toStdString()}, {2, BLUE.toStdString()}};
        o.canvasW = 320; o.canvasH = 180; o.timelineFps = 30.0;
        const QString f = dir.path() + "/timeline.mp4";
        ok(runExport(RED, f, o, &err), "exports");
        if (!err.isEmpty()) std::printf("     err: %s\n", qPrintable(err));
        // This is the path that never asked for +faststart before.
        checkFile("timeline", f, 2.0);
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
    return failures ? 1 : 0;
}
