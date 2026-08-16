// The Inspector, while the pointer is reading a frame off the timeline.
//
// Hovering the timeline previews that frame, and now the Inspector's values
// follow it -- so a keyframed move can be read AT 4.2 s without dragging the
// playhead there and losing where you were.
//
// The values shown are then an INTERPOLATED READ of another instant, not the
// clip's editable state, and the panel has to say so in both the ways that
// matter:
//
//   - read-only, because the number under the cursor is replaced on the next
//     mouse-move; a field you can type into while its content is rewritten
//     thirty times a second is a field that will eat an edit;
//   - and visibly different, because "0.42" interpolated between two keys is
//     not the same fact as "0.42" stored on the clip.
//
// What is NOT disabled matters just as much: a component's enable box, its
// arrows and its remove button belong to the clip whatever frame you are
// looking at, and greying the whole panel would read as "the Inspector has
// stopped working".
#include "editor/component/ComponentPanel.hpp"

#include "editor/ParamSlider.hpp"
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// A selection of one clip carrying a Blur, which has a Float property -- so the
// panel builds a real slider to disable.
static ComponentPanel::View viewWithBlur(bool withSecond)
{
	ComponentPanel::View v;
	v.clipCount = 1;

	ComponentPanel::PinnedRow pose;
	pose.typeId = QStringLiteral("harpia.transform");
	pose.values = {{QStringLiteral("posX"), {0.5, false}},
		       {QStringLiteral("posY"), {0.5, false}},
		       {QStringLiteral("scale"), {1.0, false}},
		       {QStringLiteral("rotation"), {0.0, false}},
		       {QStringLiteral("opacity"), {1.0, false}}};
	v.pinned.append(pose);

	ComponentPanel::SharedComponent blur;
	blur.typeId = QStringLiteral("harpia.blur");
	blur.enabled = {true, false};
	blur.values = {{QStringLiteral("radius"), {0.4, false}}};
	v.shared.append(blur);

	if (withSecond) {
		ComponentPanel::SharedComponent rot;
		rot.typeId = QStringLiteral("harpia.alwaysRotate");
		rot.enabled = {true, false};
		rot.values = {{QStringLiteral("degreesPerSecond"), {30.0, false}}};
		v.shared.append(rot);
	}
	return v;
}

static int enabledSliders(ComponentPanel *p)
{
	int n = 0;
	for (ParamSlider *s : p->findChildren<ParamSlider *>())
		if (s->isEnabled())
			++n;
	return n;
}
static int italicLabels(ComponentPanel *p)
{
	int n = 0;
	for (QLabel *l : p->findChildren<QLabel *>())
		if (l->font().italic())
			++n;
	return n;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	ComponentPanel panel(ComponentRegistry::instance());
	panel.setView(viewWithBlur(false));
	panel.show();
	QApplication::processEvents();

	const int sliders = panel.findChildren<ParamSlider *>().size();
	std::printf("     %d value sliders built\n", sliders);
	ok(sliders > 0, "the panel built controls to speak about");

	std::printf("\n-- normally the values are editable --\n");
	ok(!panel.previewing(), "the panel starts on the clip's own state");
	ok(enabledSliders(&panel) == sliders, "every value control is live");
	ok(italicLabels(&panel) == 0, "and nothing is marked as a read");

	std::printf("\n-- while the timeline is hovered they are a read-only read --\n");
	panel.setPreviewing(true);
	QApplication::processEvents();
	ok(panel.previewing(), "the panel knows it is previewing");
	ok(enabledSliders(&panel) == 0,
	   "no value can be typed into while it is being rewritten by the pointer");
	ok(italicLabels(&panel) > 0, "and the rows read as interpolated, not stored");

	// The control that keeps this from being "the panel was switched off".
	{
		int liveButtons = 0;
		for (QCheckBox *c : panel.findChildren<QCheckBox *>())
			if (c->isEnabled())
				++liveButtons;
		ok(liveButtons > 0,
		   "a component's enable box still works — it belongs to the clip, not to "
		   "the frame under the pointer");
	}

	std::printf("\n-- and a rebuild underneath does not lose it --\n");
	{
		// A hover can cross a clip boundary, which changes the SHAPE of the
		// view and rebuilds every widget. The new widgets are born enabled, so
		// without re-applying the state the panel silently becomes editable
		// again mid-hover.
		panel.setView(viewWithBlur(true));
		QApplication::processEvents();
		ok(panel.findChildren<ParamSlider *>().size() > sliders, "the panel really rebuilt");
		ok(enabledSliders(&panel) == 0, "and the new controls are read-only too");
	}

	std::printf("\n-- leaving the timeline gives them back --\n");
	panel.setPreviewing(false);
	QApplication::processEvents();
	ok(enabledSliders(&panel) == panel.findChildren<ParamSlider *>().size(),
	   "every value is editable again");
	ok(italicLabels(&panel) == 0, "and the marking is gone");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
