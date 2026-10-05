// The editor window's layout, checked in the real window.
//
//   * The Inspector fits its narrowest width: in every tab, for a caption, a
//     Random text and a video clip, the content needs no more width than the
//     panel has -- no horizontal scrollbar, nothing cut off on the right.
//   * The Clip tab leads with the clip's own content: a caption's Text
//     section above the components, Animation and Tags below them.
//   * Sections fold, and stay folded in the next window.
//   * One toolbar: a single line in a wide window, wrapped in a narrow one,
//     and never a button squeezed below its own label (the "le ti Ed" bug).
//   * The preview/timeline divider is remembered per editing mode.
#include "editor/InspectorSection.hpp"
#include "editor/ToolbarLayout.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}

static void openFullEditing(VideoEditorWindow &w)
{
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Full Editing")) {
			b->click();
			settle(300);
			break;
		}
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->isCheckable() && b->text() == QStringLiteral("Inspector") && !b->isChecked()) {
			b->click();
			settle(300);
			break;
		}
}

static QTabWidget *inspectorTabs(VideoEditorWindow &w)
{
	for (QTabWidget *t : w.findChildren<QTabWidget *>())
		if (t->count() >= 4 && t->tabText(0) == QStringLiteral("Project"))
			return t;
	return nullptr;
}

static void trigger(VideoEditorWindow &w, const QString &action)
{
	for (QAction *a : w.findChildren<QAction *>())
		if (a->text() == action) {
			a->trigger();
			break;
		}
	settle(400);
}

// Every tab's content fits the panel at the panel's narrowest.
static bool inspectorFits(VideoEditorWindow &w, const char *what)
{
	QTabWidget *tabs = inspectorTabs(w);
	if (!tabs)
		return false;
	bool all = true;
	const int was = tabs->currentIndex();
	for (int i = 0; i < tabs->count(); ++i) {
		tabs->setCurrentIndex(i);
		settle(120);
		auto *sa = qobject_cast<QScrollArea *>(tabs->widget(i));
		if (!sa)
			continue;
		const int need = sa->widget()->minimumSizeHint().width();
		const int have = sa->viewport()->width();
		if (need > have) {
			std::printf("     %s: %s tab needs %d px, has %d\n", what, qPrintable(tabs->tabText(i)), need, have);
			all = false;
		}
	}
	const int tabsNeed = tabs->tabBar()->minimumSizeHint().width();
	if (tabsNeed > tabs->width()) {
		std::printf("     %s: the tab strip needs %d px, has %d\n", what, tabsNeed, tabs->width());
		all = false;
	}
	tabs->setCurrentIndex(was);
	settle(80);
	return all;
}

static InspectorSection *section(VideoEditorWindow &w, const QString &title)
{
	// No Q_OBJECT on InspectorSection, so findChildren<InspectorSection *>
	// would match every widget: look at each one properly.
	for (QWidget *x : w.findChildren<QWidget *>())
		if (auto *s = dynamic_cast<InspectorSection *>(x))
			if (s->isVisible() && s->title() == title)
				return s;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString video = media + QStringLiteral("/av_red.mp4");
	if (!QFile::exists(video)) {
		std::printf("missing %s\n", qPrintable(video));
		return 2;
	}

	std::printf("\n-- the Inspector fits its narrowest width --\n");
	{
		VideoEditorWindow w(video);
		w.resize(1000, 760);
		w.show();
		settle(300);
		openFullEditing(w);
		// The panel at the narrowest the splitter allows.
		auto *hsplit = qobject_cast<QSplitter *>(inspectorTabs(w)->parentWidget()->parentWidget());
		if (hsplit && hsplit->sizes().size() == 2) {
			const int total = hsplit->sizes()[0] + hsplit->sizes()[1];
			hsplit->setSizes({total - 10, 10}); // clamped to the panel's minimum
			settle(200);
		}
		std::printf("     panel width %d\n", inspectorTabs(w)->width());

		trigger(w, QStringLiteral("Text"));
		ok(inspectorFits(w, "caption"), "a caption: every tab fits");
		trigger(w, QStringLiteral("Random text"));
		ok(inspectorFits(w, "random text"), "a Random text: every tab fits");

		// The video clip the window opened with.
		TimelineView *tv = w.findChild<TimelineView *>();
		int vt = -1;
		for (int t = 0; t < tv->model().tracks.size(); ++t)
			for (const TlClip &c : tv->model().tracks[t].clips)
				if (c.type == TlClip::Type::Video)
					vt = t;
		if (vt >= 0) {
			const QPoint at = tv->clipRectForTest(vt, 0).center();
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), tv->mapToGlobal(QPointF(at)),
					  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QApplication::sendEvent(tv, &press);
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), tv->mapToGlobal(QPointF(at)),
					    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(tv, &release);
			settle(300);
		}
		ok(tv->selectedClipPtr() && tv->selectedClipPtr()->type == TlClip::Type::Video, "the video clip selected");
		ok(inspectorFits(w, "video"), "a video clip: every tab fits");
	}

	std::printf("\n-- the Clip tab leads with the clip's own content --\n");
	{
		VideoEditorWindow w(video);
		w.resize(1000, 760);
		w.show();
		settle(300);
		openFullEditing(w);
		inspectorTabs(w)->setCurrentIndex(1);
		trigger(w, QStringLiteral("Text"));
		InspectorSection *text = section(w, QStringLiteral("Text"));
		InspectorSection *anim = section(w, QStringLiteral("Animation"));
		InspectorSection *tags = section(w, QStringLiteral("Tags"));
		QWidget *addComponent = nullptr;
		for (QPushButton *b : w.findChildren<QPushButton *>())
			if (b->isVisible() && b->text() == QStringLiteral("Add Component"))
				addComponent = b;
		ok(text && anim && tags && addComponent, "Text, Animation, Tags and the components are all there");
		if (text && anim && tags && addComponent) {
			const auto y = [](QWidget *x) { return x->mapToGlobal(QPoint(0, 0)).y(); };
			std::printf("     y: Text %d, components end %d, Animation %d, Tags %d\n", y(text), y(addComponent),
				    y(anim), y(tags));
			ok(y(text) < y(addComponent), "the Text section comes before the components");
			ok(y(addComponent) < y(anim) && y(anim) < y(tags), "Animation, then Tags, after them");
		}

		// Fold Tags; a new window opens it folded.
		if (tags) {
			tags->header()->click();
			settle(100);
			ok(!tags->expanded() && tags->body()->isHidden(), "clicking the header folds a section");
		}
	}
	{
		VideoEditorWindow w(video);
		w.resize(1000, 760);
		w.show();
		settle(300);
		openFullEditing(w);
		inspectorTabs(w)->setCurrentIndex(1);
		trigger(w, QStringLiteral("Text"));
		InspectorSection *tags = section(w, QStringLiteral("Tags"));
		ok(tags && !tags->expanded(), "and it is still folded in the next window");
		if (tags)
			tags->setExpanded(true); // leave the settings as found
	}

	std::printf("\n-- one toolbar, wrapping rather than squeezing --\n");
	{
		VideoEditorWindow w(video);
		w.show();
		settle(300);
		ToolbarLayout *bar = nullptr;
		for (QWidget *x : w.findChildren<QWidget *>())
			if (auto *l = dynamic_cast<ToolbarLayout *>(x->layout()))
				bar = l;
		ok(bar != nullptr, "the toolbar is there");
		if (!bar)
			return 1;
		QWidget *host = bar->parentWidget();
		const auto noneSqueezed = [&]() {
			bool fine = true;
			for (QPushButton *b : host->findChildren<QPushButton *>())
				if (b->isVisible() && b->width() < b->sizeHint().width()) {
					std::printf("     '%s' is %d px for a %d px label\n", qPrintable(b->text()), b->width(),
						    b->sizeHint().width());
					fine = false;
				}
			return fine;
		};
		for (int mode = 0; mode < 3; ++mode) {
			const char *names[] = {"Simple Trim", "Multi-Cut", "Full Editing"};
			for (QPushButton *b : w.findChildren<QPushButton *>())
				if (b->text() == QString::fromLatin1(names[mode]))
					b->click();
			settle(250);
			const int lineH = bar->sizeHint().height();
			ok(bar->fitsOneLine(1900), qPrintable(QStringLiteral("%1: one line at 1900 px").arg(names[mode])));
			ok(!bar->fitsOneLine(560) && bar->heightForWidth(560) > lineH,
			   qPrintable(QStringLiteral("%1: wraps onto more lines at 560 px").arg(names[mode])));
			host->resize(560, bar->heightForWidth(560));
			bar->setGeometry(QRect(0, 0, 560, bar->heightForWidth(560)));
			settle(50);
			ok(noneSqueezed(), qPrintable(QStringLiteral("%1: no button cut below its label").arg(names[mode])));
		}
	}

	std::printf("\n-- the divider is remembered per mode --\n");
	{
		VideoEditorWindow w(video);
		w.resize(1000, 760);
		w.show();
		settle(400);
		QSplitter *v = nullptr;
		for (QSplitter *s : w.findChildren<QSplitter *>())
			if (s->orientation() == Qt::Vertical)
				v = s;
		ok(v != nullptr, "the preview/timeline divider");
		if (v) {
			const auto click = [&](const char *name) {
				for (QPushButton *b : w.findChildren<QPushButton *>())
					if (b->text() == QString::fromLatin1(name))
						b->click();
				settle(300);
			};
			click("Full Editing");
			const int total = v->sizes()[0] + v->sizes()[1];
			v->setSizes({total - 330, 330});
			emit v->splitterMoved(total - 330, 1); // as a drag would report it
			settle(50);
			click("Simple Trim");
			const int trimBottom = v->sizes()[1];
			click("Full Editing");
			std::printf("     Full Editing bottom %d -> Simple Trim %d -> Full Editing %d\n", 330, trimBottom,
				    v->sizes()[1]);
			ok(std::abs(v->sizes()[1] - 330) <= 2, "back in Full Editing, the height it was dragged to");
			ok(trimBottom < 330, "Simple Trim kept its own, shorter split");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
