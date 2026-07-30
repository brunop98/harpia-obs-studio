#include "StartupSplash.hpp"

#include <QApplication>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {
namespace {

struct PhaseInfo {
	StartupPhase phase;
	const char *key;
	const char *label;
	int defaultMs; // first-run weight, before any real measurement exists
};

// The default weights are a shape, not a prediction: plugin loading is the
// dominant term on every machine this has been looked at, the graphics device
// is second, and the rest is small. They are replaced by real numbers after one
// run, which is the point -- see the header.
constexpr PhaseInfo kPhases[] = {
	{StartupPhase::Backend, "backend", "Starting up", 60},
	{StartupPhase::Audio, "audio", "Audio engine", 80},
	{StartupPhase::Video, "video", "Graphics device", 400},
	{StartupPhase::Plugins, "plugins", "Loading plugins", 1200},
	{StartupPhase::Components, "components", "Checking components", 60},
	{StartupPhase::Presets, "presets", "Reading presets", 40},
	{StartupPhase::Window, "window", "Building the window", 200},
	{StartupPhase::Finishing, "finishing", "Looking for cameras and displays", 300},
};
constexpr int kPhaseCount = int(sizeof(kPhases) / sizeof(kPhases[0]));

const PhaseInfo *infoFor(StartupPhase p)
{
	for (const PhaseInfo &i : kPhases)
		if (i.phase == p)
			return &i;
	return nullptr;
}

} // namespace

int startupPhaseCount()
{
	return kPhaseCount;
}

StartupPhase startupPhaseAt(int index)
{
	if (index < 0 || index >= kPhaseCount)
		return StartupPhase::Done;
	return kPhases[index].phase;
}

const char *startupPhaseKey(StartupPhase p)
{
	const PhaseInfo *i = infoFor(p);
	return i ? i->key : "done";
}

QString startupPhaseLabel(StartupPhase p)
{
	const PhaseInfo *i = infoFor(p);
	return i ? QString::fromLatin1(i->label) : QStringLiteral("Ready");
}

StartupTimeline::StartupTimeline(const QMap<QString, int> &remembered)
{
	for (const PhaseInfo &i : kPhases) {
		const QString key = QString::fromLatin1(i.key);
		const int seen = remembered.value(key, -1);
		// A remembered zero is real (a phase that was already warm), but a
		// negative or absurd value is not: a machine that slept mid-startup
		// would otherwise poison the weights for every run afterwards.
		weights_.insert(key, (seen >= 0 && seen < 120000) ? seen : i.defaultMs);
	}
}

int StartupTimeline::weightOf(StartupPhase p) const
{
	// Every phase gets a floor of 1 ms of weight. Without it a run where three
	// phases measured 0 would give them no width at all, and the bar would jump
	// past them without ever naming what it was doing.
	return std::max(1, weights_.value(QString::fromLatin1(startupPhaseKey(p)), 1));
}

void StartupTimeline::begin(StartupPhase p, qint64 nowMs)
{
	if (started_)
		measured_.insert(QString::fromLatin1(startupPhaseKey(phase_)),
				 int(std::max<qint64>(0, nowMs - phaseStart_)));
	phase_ = p;
	phaseStart_ = nowMs;
	started_ = true;
}

void StartupTimeline::finish(qint64 nowMs)
{
	if (started_)
		measured_.insert(QString::fromLatin1(startupPhaseKey(phase_)),
				 int(std::max<qint64>(0, nowMs - phaseStart_)));
	phase_ = StartupPhase::Done;
	started_ = false;
}

int StartupTimeline::percent() const
{
	int total = 0;
	for (const PhaseInfo &i : kPhases)
		total += weightOf(i.phase);
	if (total <= 0)
		return 0;
	if (phase_ == StartupPhase::Done)
		return 100;

	int done = 0;
	for (const PhaseInfo &i : kPhases) {
		if (i.phase == phase_)
			break;
		done += weightOf(i.phase);
	}
	// Never 100 before finish(): the bar filling while the app is still working
	// is exactly the thing people learn not to trust.
	return std::clamp(int(std::lround(100.0 * done / total)), 0, 99);
}

int StartupTimeline::totalMs() const
{
	int t = 0;
	for (auto it = measured_.cbegin(); it != measured_.cend(); ++it)
		t += it.value();
	return t;
}

QString StartupTimeline::summary() const
{
	QVector<QPair<int, QString>> rows;
	for (const PhaseInfo &i : kPhases) {
		const QString key = QString::fromLatin1(i.key);
		if (measured_.contains(key))
			rows.append({measured_.value(key), key});
	}
	std::sort(rows.begin(), rows.end(),
		  [](const auto &a, const auto &b) { return a.first > b.first; });
	QStringList parts;
	for (const auto &r : rows)
		parts << QStringLiteral("%1 %2ms").arg(r.second).arg(r.first);
	return parts.join(QStringLiteral(", "));
}

StartupSplash::StartupSplash(const QString &versionText, QWidget *parent)
	: QWidget(parent, Qt::SplashScreen | Qt::FramelessWindowHint)
{
	setAttribute(Qt::WA_DeleteOnClose, false);
	setObjectName(QStringLiteral("startupSplash"));
	setFixedSize(380, 190);
	setStyleSheet(QStringLiteral(R"(
		QWidget#startupSplash { background: #1e1f22; border: 1px solid #3a3d42;
					border-radius: 10px; }
		QLabel { color: #e6e6e6; }
		QLabel#splashName { font-size: 20px; font-weight: 600; }
		QLabel#splashVersion, QLabel#splashStatus { color: #9a9fa8; }
		QProgressBar { background: #2b2d31; border: 1px solid #3a3d42;
			       border-radius: 5px; height: 8px; text-align: center; }
		QProgressBar::chunk { background: #e5484d; border-radius: 4px; }
	)"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(26, 22, 26, 20);
	root->setSpacing(6);

	auto *icon = new QLabel(this);
	icon->setPixmap(QIcon(QStringLiteral(":/harpia.png")).pixmap(48, 48));
	icon->setAlignment(Qt::AlignCenter);
	root->addWidget(icon);

	auto *name = new QLabel(QStringLiteral("Harpia Recorder"), this);
	name->setObjectName(QStringLiteral("splashName"));
	name->setAlignment(Qt::AlignCenter);
	root->addWidget(name);

	auto *ver = new QLabel(versionText, this);
	ver->setObjectName(QStringLiteral("splashVersion"));
	ver->setAlignment(Qt::AlignCenter);
	root->addWidget(ver);

	root->addStretch(1);

	bar_ = new QProgressBar(this);
	bar_->setRange(0, 100);
	bar_->setValue(0);
	bar_->setTextVisible(false);
	root->addWidget(bar_);

	status_ = new QLabel(QStringLiteral("Starting up"), this);
	status_->setObjectName(QStringLiteral("splashStatus"));
	status_->setAlignment(Qt::AlignCenter);
	root->addWidget(status_);

	if (QScreen *s = QGuiApplication::primaryScreen())
		move(s->geometry().center() - rect().center());
}

void StartupSplash::showPhase(const QString &label, int percent)
{
	status_->setText(label);
	bar_->setValue(percent);
	// Startup runs before app.exec(), so nothing repaints on its own. Without
	// this the splash is a grey rectangle for the whole of the slow part, which
	// is worse than not having one.
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

QString StartupSplash::labelText() const
{
	return status_->text();
}

int StartupSplash::percent() const
{
	return bar_->value();
}

} // namespace harpia
