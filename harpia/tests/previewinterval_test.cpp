// A cached preview frame is exact only for its real timestamp interval. FPS
// estimates and integer frame durations cannot identify fractional/VFR frames.
#include "editor/FrameSeeker.hpp"
#include "editor/PreviewDecoder.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <thread>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace harpia;

static int failures = 0;
static void ok(bool value, const char *why)
{
	std::printf("  %s %s\n", value ? "PASS" : "FAIL", why);
	if (!value)
		++failures;
}

template<typename Pred> static bool pump(Pred pred, int timeout = 5000)
{
	QElapsedTimer clock;
	clock.start();
	while (!pred() && clock.elapsed() < timeout) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return pred();
}

static bool ffmpeg(const QStringList &args)
{
	QProcess process;
	process.start(QStringLiteral("ffmpeg"), QStringList{"-hide_banner", "-loglevel", "error", "-y"} + args);
	return process.waitForFinished(20000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

static bool makeCounter(const QString &path, const QString &rate, bool variable = false, bool delayed = false)
{
	QStringList args{"-f", "lavfi", "-i",
		QStringLiteral("nullsrc=s=64x64:r=%1:d=1.5,format=yuv420p,"
			"geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128").arg(rate)};
	if (variable)
		args << "-vf" << "select='eq(n,0)+eq(n,2)+eq(n,11)+eq(n,14)+eq(n,28)'" << "-fps_mode" << "vfr";
	args << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-bf" << "0" << "-g" << "300" << "-crf" << "8";
	if (delayed)
		args << "-output_ts_offset" << "0.2";
	args << path;
	return ffmpeg(args);
}

static int counterOf(const QImage &image)
{
	if (image.isNull())
		return -1;
	int value = 0;
	for (int bit = 0; bit < 8; ++bit)
		if (qGray(image.pixelColor(bit * 8 + 4, 32).rgb()) > 128)
			value |= 1 << bit;
	return value;
}

static void checkRequests(const QString &path, const QVector<qint64> &requests, const char *why)
{
	FrameSeeker reference;
	PreviewDecoder preview;
	preview.setSource(1, path);
	bool correct = reference.open(path);
	for (qint64 ms : requests) {
		const QImage expected = reference.frameAt(ms, 64, 64);
		bool exact = false;
		QImage shown = preview.frame(1, ms, 64, 64, &exact);
		if (exact && shown != expected)
			correct = false;
		if (!exact) {
			correct = pump([&] { return preview.has(1, ms, 64, 64); }) && correct;
			shown = preview.frame(1, ms, 64, 64, &exact);
		}
		if (!exact || shown != expected) {
			std::printf("     %lld ms: frame %d, expected %d; exact=%d\n",
				(long long)ms, counterOf(shown), counterOf(expected), exact);
			correct = false;
		}
	}
	ok(correct, why);
}

#ifdef Q_OS_UNIX
// Opening the FIFO proves the decoder has started that particular request.
// Its data stays gated until the test changes the registered source/proxy.
class GatedInput {
public:
	GatedInput(const QString &path, const QByteArray &bytes) : path_(path), bytes_(bytes)
	{
		valid_ = ::mkfifo(QFile::encodeName(path).constData(), 0600) == 0;
		if (!valid_)
			return;
		writer_ = std::thread([this] {
			int fd = -1;
			while (!stop_.load() && fd < 0) {
				fd = ::open(QFile::encodeName(path_).constData(), O_WRONLY | O_NONBLOCK);
				if (fd < 0)
					QThread::msleep(1);
			}
			if (fd < 0)
				return;
			opened_.store(true);
			while (!release_.load() && !stop_.load())
				QThread::msleep(1);
			qsizetype sent = 0;
			while (sent < bytes_.size() && !stop_.load()) {
				const ssize_t wrote = ::write(fd, bytes_.constData() + sent, bytes_.size() - sent);
				if (wrote > 0)
					sent += wrote;
				else
					QThread::msleep(1);
			}
			::close(fd);
		});
	}
	~GatedInput()
	{
		stop_.store(true);
		release_.store(true);
		if (writer_.joinable())
			writer_.join();
	}
	bool opened() const { return valid_ && opened_.load(); }
	void release() { release_.store(true); }

private:
	QString path_;
	QByteArray bytes_;
	bool valid_ = false;
	std::atomic<bool> opened_{false}, release_{false}, stop_{false};
	std::thread writer_;
};

static void checkInvalidation(const QString &folder)
{
	const QString red = folder + "/red.ts", green = folder + "/green.ts";
	const auto makeColor = [](const QString &out, const char *color) {
		return ffmpeg({"-f", "lavfi", "-i", QStringLiteral("color=c=%1:s=64x64:r=30:d=0.4").arg(color),
			"-c:v", "libx264", "-preset", "ultrafast", "-bf", "0", "-f", "mpegts", out});
	};
	if (!makeColor(red, "red") || !makeColor(green, "green")) {
		ok(false, "created source/proxy invalidation fixtures");
		return;
	}
	QFile redFile(red), greenFile(green);
	if (!redFile.open(QIODevice::ReadOnly))
		qFatal("Could not open the test fixture file.");
	if (!greenFile.open(QIODevice::ReadOnly))
		qFatal("Could not open the test fixture file.");
	const QByteArray oldBytes = redFile.readAll(), newBytes = greenFile.readAll();
	for (bool proxy : {false, true}) {
		const QString oldPath = folder + (proxy ? "/old-proxy.ts" : "/old-source.ts");
		const QString newPath = folder + (proxy ? "/new-proxy.ts" : "/new-source.ts");
		GatedInput oldInput(oldPath, oldBytes), newInput(newPath, newBytes);
		PreviewDecoder preview;
		preview.setSource(1, oldPath);
		preview.frame(1, 0, 64, 64);
		const bool oldStarted = pump([&] { return oldInput.opened(); });
		if (proxy)
			preview.setProxy(1, newPath);
		else {
			preview.removeSource(1);
			preview.setSource(1, newPath);
		}
		preview.frame(1, 0, 64, 64);
		oldInput.release();
		const bool newStarted = pump([&] { return newInput.opened(); });
		ok(oldStarted && newStarted && !preview.has(1, 0, 64, 64), proxy
			? "an in-flight old proxy cannot refill the cache after proxy replacement"
			: "an in-flight removed source cannot refill a reused source id");
		newInput.release();
		const bool arrived = pump([&] { return preview.has(1, 0, 64, 64); });
		const QImage actual = preview.frame(1, 0, 64, 64);
		const QColor pixel = actual.isNull() ? QColor() : actual.pixelColor(32, 32);
		ok(arrived && pixel.green() > pixel.red() * 2, "the replacement source supplies the final exact frame");
	}
}
#endif

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	const QString sixty = scratch.filePath("sixty.mp4"), fraction = scratch.filePath("fraction.mp4");
	const QString variable = scratch.filePath("variable.mp4"), late = scratch.filePath("late.mp4");
	if (!makeCounter(sixty, "60") || !makeCounter(fraction, "30000/1001") ||
	    !makeCounter(variable, "100", true) || !makeCounter(late, "100", true, true)) {
		ok(false, "created timestamp fixtures");
		return 1;
	}
	{
		PreviewDecoder preview;
		preview.setSource(1, sixty);
		preview.frame(1, 64, 64, 64);
		ok(pump([&] { return preview.has(1, 64, 64, 64); }), "the first 60 fps frame is decoded");
		bool exact = false;
		const QImage same = preview.frame(1, 65, 64, 64, &exact);
		ok(exact && counterOf(same) == 3, "a nearby request inside the decoded frame stays a cache hit");
		preview.frame(1, 67, 64, 64, &exact);
		ok(!exact, "crossing the real 66.67 ms boundary is a cache miss");
		ok(pump([&] { return preview.has(1, 67, 64, 64); }) && counterOf(preview.frame(1, 67, 64, 64)) == 4,
		   "the correct next frame arrives instead of a false exact hit");
	}
	checkRequests(fraction, {955, 960, 966, 970, 990, 1000, 1001, 1020, 1033, 1040},
		"fractional-rate scrubbing agrees with exact decoded timestamps");
	checkRequests(variable, {0, 10, 19, 20, 21, 50, 109, 110, 130, 140, 220, 279, 280, 500},
		"variable-rate frames retain their real individual intervals");
	checkRequests(late, {0, 100, 199, 200, 210, 220, 230, 300, 310, 400, 1000},
		"late first frames and EOF stay consistent with source timestamps");
	{
		FrameSeeker seeker;
		seeker.open(late);
		FrameSeeker::FrameInterval interval;
		ok(counterOf(seeker.frameAt(0, 64, 64, &interval)) == 0,
		   "a request before the first frame shows that first frame");
		ok(counterOf(seeker.frameAt(230, 64, 64, &interval)) == 2,
		   "an unknown first-frame duration cannot hide an early VFR next frame");
		seeker.frameAt(1000, 64, 64, &interval);
		ok(interval.contains(2000), "an exhausted source's final frame has an open-ended interval");
	}
	{
		PreviewDecoder bounded(nullptr, 2 * 64 * 64 * 4);
		for (int source : {1, 2, 3})
			bounded.setSource(source, sixty);
		const auto request = [&](int source) {
			bounded.frame(source, 0, 64, 64);
			return pump([&] { return bounded.has(source, 0, 64, 64); });
		};
		bool arrived = request(1) && request(2);
		bounded.frame(1, 0, 64, 64); // source 1 was used more recently than 2
		arrived = request(3) && arrived;
		ok(arrived && bounded.has(1, 0, 64, 64) && bounded.has(3, 0, 64, 64) && !bounded.has(2, 0, 64, 64),
		   "the shared byte budget evicts the least recently used frame across source boundaries");
		bounded.removeSource(1);
		ok(request(2) && bounded.has(3, 0, 64, 64), "removing a source releases its share of the global cache budget");
		bounded.clear();
		bounded.setSource(1, sixty);
		bounded.setSource(2, sixty);
		ok(request(1) && request(2) && bounded.has(1, 0, 64, 64), "clearing the cache resets its memory accounting");
	}
	{
		PreviewDecoder tiny(nullptr, 1);
		tiny.setSource(1, sixty);
		tiny.setSource(2, sixty);
		tiny.frame(1, 0, 64, 64);
		ok(pump([&] { return tiny.has(1, 0, 64, 64); }),
		   "one frame larger than the budget is retained so the preview can converge");
		tiny.frame(2, 0, 64, 64);
		ok(pump([&] { return tiny.has(2, 0, 64, 64); }) && !tiny.has(1, 0, 64, 64),
		   "an oversized replacement frame evicts the previous source's frame");
	}
#ifdef Q_OS_UNIX
	checkInvalidation(scratch.path());
#endif
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
