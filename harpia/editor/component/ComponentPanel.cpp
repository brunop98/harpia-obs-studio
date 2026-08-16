#include "ComponentPanel.hpp"

#include "../../ui/UiIcons.hpp"
#include "../../ui/UiText.hpp"
#include "../ParamSlider.hpp"
#include "ComponentRegistry.hpp"

#include <QCheckBox>
#include "ui/ColorField.hpp"

#include <QColorDialog>
#include <QSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

// One component's widgets. Kept so pushValues() can update them in place rather
// than rebuilding and stealing the focus from whoever is typing.
struct ComponentPanel::Row {
	QFrame *box = nullptr;
	QCheckBox *enabled = nullptr;
	QString typeId;
	int ordinal = 0;
	bool pinned = false;
	QVector<QString> keys;
	QVector<ParamSlider *> sliders; // null for a non-float property
	QVector<QCheckBox *> checks;    // null for a non-bool property
	QVector<QPushButton *> swatches; // null for a non-colour property
	QVector<QPushButton *> keyBtns;
	QVector<QLabel *> labels;
	QSpinBox *inMs = nullptr;  // ramp-in time; null on pinned rows
	QSpinBox *outMs = nullptr;
};

namespace {

// Paint a swatch button with the colour it stands for, and remember that colour
// on the widget so the picker can open on it. A mixed selection -- two clips
// with different colours -- shows a dash rather than one of them, matching what
// the sliders do.
void setSwatchColor(QPushButton *b, const ComponentPanel::Mixed &m)
{
	if (!b)
		return;
	if (m.mixed) {
		paintMixedSwatch(b);
		return;
	}
	const QColor c = m.value.canConvert<QColor>()
				 ? m.value.value<QColor>()
				 : QColor::fromRgba(QRgb(quint32(m.value.toUInt())));
	// No hex on the face: these rows are narrow and already labelled. (A
	// chequer under a translucent colour would be better still, but the panel
	// is dark and flat and that is one more thing to keep in step with it.)
	paintColorSwatch(b, c, /*showHex=*/false);
}

// How wide a property's name may get before it wraps. Narrow on purpose: the
// controls are what the eye needs to land on, and a component's own //@param
// text can be arbitrarily long.
constexpr int kPropLabelW = 116;

// A component's stage, shown small beside its name. Not decoration: the order
// things run in is the question people ask first, and the answer is otherwise
// invisible.
QLabel *stageBadge(Stage s, QWidget *parent)
{
	auto *l = new QLabel(QString::fromLatin1(stageName(s)), parent);
	l->setStyleSheet(QStringLiteral("color:#7f858e; font-size:%1px;").arg(uiCaptionPx()));
	l->setToolTip(QStringLiteral("Stages always run in this order: Time, Source, Transform, "
				     "Pixel, Composite, Audio — whatever order the list is in."));
	return l;
}

QPushButton *iconButton(Glyph g, const QString &tip, QWidget *parent)
{
	auto *b = new QPushButton(parent);
	b->setIcon(uiIcon(g, 11));
	b->setToolTip(tip);
	b->setFixedSize(22, 20);
	b->setFlat(true);
	return b;
}

Qt::CheckState stateOf(const ComponentPanel::Mixed &m)
{
	// Qt's third state exists for exactly this. Picking checked or unchecked
	// would be reporting one clip's answer as everyone's.
	return m.mixed ? Qt::PartiallyChecked : (m.value.toBool() ? Qt::Checked : Qt::Unchecked);
}

} // namespace

ComponentPanel::ComponentPanel(const ComponentRegistry &reg, QWidget *parent)
	: QWidget(parent), reg_(reg)
{
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(0, 6, 0, 0);
	v->setSpacing(5);

	header_ = new QLabel(QStringLiteral("Components"), this);
	header_->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	v->addWidget(header_);

	loadErrors_ = new QLabel(this);
	loadErrors_->setWordWrap(true);
	loadErrors_->setStyleSheet(
		QStringLiteral("color:#e2a03f; font-size:%1px;").arg(uiCaptionPx()));
	loadErrors_->setVisible(false);
	v->addWidget(loadErrors_);

	warnings_ = new QLabel(this);
	warnings_->setWordWrap(true);
	warnings_->setStyleSheet(
		QStringLiteral("color:#e2a03f; font-size:%1px;").arg(uiCaptionPx()));
	warnings_->setVisible(false);
	v->addWidget(warnings_);

	empty_ = new QLabel(this);
	empty_->setWordWrap(true);
	empty_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(empty_);

	listLayout_ = new QVBoxLayout;
	listLayout_->setSpacing(6);
	v->addLayout(listLayout_);

	// Components on SOME of the selection. Named rather than drawn, because
	// there is nothing coherent to edit — but omitting them silently would let
	// the list read as the whole truth about the clips.
	notShared_ = new QLabel(this);
	notShared_->setWordWrap(true);
	notShared_->setStyleSheet(
		QStringLiteral("color:#7f858e; font-size:%1px;").arg(uiCaptionPx()));
	notShared_->setVisible(false);
	v->addWidget(notShared_);

	auto *add = new QPushButton(QStringLiteral("Add Component"), this);
	add->setToolTip(QStringLiteral("Everything a clip does is a component — built-in or one "
				       "you wrote yourself. Goes on every selected clip."));
	connect(add, &QPushButton::clicked, this, &ComponentPanel::addComponentMenu);
	v->addWidget(add);

	setView(View{});
}

bool ComponentPanel::shapeChanged(const View &next) const
{
	if (next.clipCount != view_.clipCount || next.pinned.size() != view_.pinned.size() ||
	    next.shared.size() != view_.shared.size() || next.notShared != view_.notShared)
		return true;
	for (int i = 0; i < next.pinned.size(); ++i) {
		if (next.pinned[i].typeId != view_.pinned[i].typeId ||
		    next.pinned[i].driven != view_.pinned[i].driven)
			return true;
		// Whether a property is MIXED changes the control, not just its value:
		// an em dash is a different thing on screen from a number, so the row
		// has to be rebuilt rather than refreshed.
		for (auto it = next.pinned[i].values.cbegin(); it != next.pinned[i].values.cend(); ++it)
			if (it->mixed != view_.pinned[i].values.value(it.key()).mixed)
				return true;
	}
	for (int i = 0; i < next.shared.size(); ++i) {
		const SharedComponent &a = next.shared[i];
		const SharedComponent &b = view_.shared[i];
		if (a.typeId != b.typeId || a.ordinal != b.ordinal || a.keyedHere != b.keyedHere ||
		    a.enabled.mixed != b.enabled.mixed || a.inMs.mixed != b.inMs.mixed ||
		    a.outMs.mixed != b.outMs.mixed)
			return true;
		for (auto it = a.values.cbegin(); it != a.values.cend(); ++it)
			if (it->mixed != b.values.value(it.key()).mixed)
				return true;
	}
	return false;
}

void ComponentPanel::setView(const View &v)
{
	const bool structural = shapeChanged(v);
	view_ = v;
	if (structural)
		rebuild();
	else
		pushValues();

	header_->setText(v.clipCount > 1
				 ? QStringLiteral("Components  ·  %1 clips selected").arg(v.clipCount)
				 : QStringLiteral("Components"));
	loadErrors_->setText(v.loadErrors.join(QChar('\n')));
	loadErrors_->setVisible(!v.loadErrors.isEmpty());
	warnings_->setText(v.warnings.join(QChar('\n')));
	warnings_->setVisible(!v.warnings.isEmpty());
	empty_->setText(v.clipCount > 1
				? QStringLiteral("These clips have no components in common. Adding "
						 "one puts it on all of them.")
				: QStringLiteral("No components. Add one to change how this clip "
						 "plays, moves or looks."));
	empty_->setVisible(v.shared.isEmpty() && v.notShared.isEmpty() && v.clipCount > 0);
	notShared_->setText(
		v.notShared.isEmpty()
			? QString()
			: QStringLiteral("Not shared — on some of these clips but not all, so "
					 "there is nothing coherent to edit here: %1")
				  .arg(v.notShared.join(QStringLiteral(", "))));
	notShared_->setVisible(!v.notShared.isEmpty());
}

ComponentPanel::Row *ComponentPanel::makeRow(const QString &typeId, int ordinal,
					     const QMap<QString, Mixed> &values,
					     const Mixed *enabled, const QStringList &driven,
					     const QString &drivenTip, const QStringList &keyedHere,
					     bool pinned, bool canUp, bool canDown, const Mixed *inMs,
					     const Mixed *outMs)
{
	const ComponentType *type = reg_.find(typeId);

	auto *row = new Row;
	row->typeId = typeId;
	row->ordinal = ordinal;
	row->pinned = pinned;
	row->box = new QFrame(this);
	row->box->setFrameShape(QFrame::StyledPanel);
	row->box->setStyleSheet(
		QStringLiteral("QFrame{border:1px solid #3a3f47; border-radius:4px;}"));
	auto *bv = new QVBoxLayout(row->box);
	bv->setContentsMargins(6, 4, 6, 6);
	bv->setSpacing(4);

	auto *head = new QHBoxLayout;
	head->setSpacing(4);

	if (!pinned && enabled) {
		row->enabled = new QCheckBox(row->box);
		row->enabled->setTristate(enabled->mixed);
		row->enabled->setCheckState(stateOf(*enabled));
		row->enabled->setToolTip(
			enabled->mixed
				? QStringLiteral("Some of these clips have it on and some off. "
						 "Clicking sets them all the same.")
				: QStringLiteral("Turn this off without losing its settings."));
		connect(row->enabled, &QCheckBox::clicked, this, [this, typeId, ordinal](bool on) {
			if (syncing_)
				return;
			emit componentEnableChanged(typeId, ordinal, on);
		});
		head->addWidget(row->enabled);
	}

	auto *name = new QLabel(type ? type->displayName : QStringLiteral("Missing: %1").arg(typeId),
				row->box);
	name->setStyleSheet(QStringLiteral("border:none; font-weight:bold; color:%1;")
				    .arg(type ? QStringLiteral("#e8eaed") : QStringLiteral("#e2a03f")));
	if (!type)
		name->setToolTip(QStringLiteral(
			"This project uses a component that is not installed here. Its settings "
			"are kept and saved back unchanged, so nothing is lost — it just cannot "
			"render on this machine."));
	else if (pinned)
		name->setToolTip(type->help + QStringLiteral("\n\nEvery clip has one, so this row "
							     "cannot be removed or reordered."));
	else if (!type->help.isEmpty())
		name->setToolTip(type->help);
	head->addWidget(name, 1);
	if (type)
		head->addWidget(stageBadge(type->stage, row->box));

	if (!pinned) {
		auto *up = iconButton(Glyph::ArrowUp, QStringLiteral("Move up"), row->box);
		up->setEnabled(canUp);
		connect(up, &QPushButton::clicked, this,
			[this, typeId, ordinal] { emit componentMoved(typeId, ordinal, -1); });
		auto *down = iconButton(Glyph::ArrowDown, QStringLiteral("Move down"), row->box);
		down->setEnabled(canDown);
		connect(down, &QPushButton::clicked, this,
			[this, typeId, ordinal] { emit componentMoved(typeId, ordinal, 1); });
		auto *more = iconButton(Glyph::ChevronDown,
					QStringLiteral("Reset, copy, paste, duplicate…"), row->box);
		connect(more, &QPushButton::clicked, this,
			[this, typeId, ordinal] { componentMenu(typeId, ordinal); });
		auto *del = iconButton(Glyph::Cross, QStringLiteral("Remove this component"), row->box);
		connect(del, &QPushButton::clicked, this,
			[this, typeId, ordinal] { emit componentRemoved(typeId, ordinal); });
		head->addWidget(up);
		head->addWidget(down);
		head->addWidget(more);
		head->addWidget(del);
	}
	bv->addLayout(head);

	if (type && !type->props.isEmpty()) {
		auto *form = new QFormLayout;
		form->setHorizontalSpacing(8);
		form->setVerticalSpacing(3);
		for (const PropDef &d : type->props) {
			row->keys.append(d.key);
			const Mixed m = values.value(d.key);
			QWidget *control = nullptr;
			ParamSlider *slider = nullptr;
			QCheckBox *check = nullptr;
			QPushButton *swatch = nullptr;

			if (d.type == PropType::Color) {
				// One swatch instead of three sliders somebody has to convert
				// in their head. The picker is opened NON-modal and wired to
				// currentColorChanged, so the frame behind it repaints while
				// the mouse is still down in the colour wheel -- which is the
				// only way to choose a colour against the actual footage
				// rather than against a memory of it.
				swatch = new QPushButton(row->box);
				swatch->setFixedHeight(20);
				swatch->setCursor(Qt::PointingHandCursor);
				setSwatchColor(swatch, m);
				const QString tid = typeId;
				connect(swatch, &QPushButton::clicked, this,
					[this, tid, ordinal, key = d.key, swatch]() {
						const QColor start =
							swatch->property("harpiaColor").value<QColor>();
						auto *dlg = new QColorDialog(start.isValid() ? start
											     : Qt::white,
									     this);
						dlg->setAttribute(Qt::WA_DeleteOnClose);
						dlg->setOption(QColorDialog::ShowAlphaChannel, true);
						dlg->setModal(false);
						// Always the property signal, never the pinned one:
						// pinnedEdited carries a double because the pinned
						// rows are the clip's own pose and speed, and a
						// colour is not one of those.
						const auto push = [this, tid, ordinal, key](const QColor &c) {
							if (syncing_ || !c.isValid())
								return;
							emit propertyEdited(tid, ordinal, key, c);
						};
						connect(dlg, &QColorDialog::currentColorChanged, this, push);
						// Cancel puts back what was there: a live preview that
						// leaves the last hovered colour behind is a preview
						// that edits by accident.
						connect(dlg, &QColorDialog::rejected, this,
							[push, start]() { push(start); });
						dlg->show();
					});
				control = swatch;
			} else if (d.type == PropType::Bool) {
				check = new QCheckBox(row->box);
				check->setTristate(m.mixed);
				check->setCheckState(stateOf(m));
				connect(check, &QCheckBox::clicked, this,
					[this, typeId, ordinal, key = d.key](bool on) {
						if (syncing_)
							return;
						emit propertyEdited(typeId, ordinal, key, on);
					});
				control = check;
			} else {
				// An Int is a count or a choice; three decimals on one reads
				// as a broken float control.
				slider = new ParamSlider(d.min, d.max,
							 d.type == PropType::Int ? 0 : 3, row->box);
				if (m.mixed)
					slider->setMixed();
				else
					slider->setValue(m.value.toDouble());
				const QString tid = typeId;
				connect(slider, &ParamSlider::valueChanged, this,
					[this, tid, ordinal, key = d.key, pinned](double v) {
						if (syncing_)
							return;
						if (pinned)
							emit pinnedEdited(tid, key, v);
						else
							emit propertyEdited(tid, ordinal, key, v);
					});
				control = slider;
			}
			row->sliders.append(slider);
			row->checks.append(check);
			row->swatches.append(swatch);
			if (!d.help.isEmpty())
				control->setToolTip(d.help);

			QPushButton *kb = nullptr;
			// Bools included. A switch that turns on partway through a clip is a
		// normal thing to want -- an invert that kicks in, a mask that stops
		// cutting -- and it holds between keys rather than interpolating, so
		// there is nothing ambiguous about keying one.
		//
		// Pinned rows are still excluded, but not because they cannot animate:
		// they are the clip's OWN pose and speed, which have their own keyframe
		// editor. Two ways to key the same channel would be two sets of keys.
		if (!pinned && d.keyframeable) {
				const bool here = keyedHere.contains(d.key);
				kb = iconButton(Glyph::Diamond,
						QStringLiteral("Key this value at the playhead "
							       "(again to remove)"),
						row->box);
				kb->setIcon(uiIcon(Glyph::Diamond, 11,
						   here ? QColor(0xe5, 0x48, 0x4d) : QColor()));
				connect(kb, &QPushButton::clicked, this,
					[this, typeId, ordinal, key = d.key] {
						emit keyframeToggled(typeId, ordinal, key);
					});
			}
			row->keyBtns.append(kb);

			auto *cell = new QWidget(row->box);
			auto *ch = new QHBoxLayout(cell);
			ch->setContentsMargins(0, 0, 0, 0);
			ch->setSpacing(3);
			ch->addWidget(control, 1);
			if (kb)
				ch->addWidget(kb);

			// A script that computes this channel makes the box a starting
			// point (it reads it as ctx.base), not the framing on screen.
			const bool isDriven = driven.contains(d.key);
			auto *lbl = new QLabel(isDriven ? d.label + QStringLiteral("  (script)") : d.label,
					       row->box);
			// Small, and bounded. A script or shader writes its own //@param
			// labels and they can be a sentence -- "Focus X (0 = left, 1 =
			// right)" -- which at body size pushed the label column wider than
			// the controls it names. Wrapped to a narrow column instead of
			// elided, so nothing is hidden behind a tooltip nobody hovers.
			lbl->setWordWrap(true);
			lbl->setMaximumWidth(kPropLabelW);
			lbl->setStyleSheet(
				QStringLiteral("border:none; font-size:%1px; color:%2;")
					.arg(uiCaptionPx())
					.arg(isDriven ? QStringLiteral("#ffd44f")
						      : QStringLiteral("#c8ccd4")));
			// The tooltip carries the label AND its help, so a wrapped or
			// abbreviated name is still readable in full somewhere.
			QString tip = d.label;
			if (!d.help.isEmpty())
				tip += QStringLiteral("\n\n") + d.help;
			if (isDriven)
				tip = drivenTip;
			lbl->setToolTip(tip);
			row->labels.append(lbl);
			form->addRow(lbl, cell);
		}
		bv->addLayout(form);
	}

	// In / Out ramp times, under the properties and above the actions. Every
	// component gets these -- they are a property of BEING a component, like
	// the enable box, not something a type opts into. Both 0 (the default)
	// means "just on", which is what every component did before they existed.
	if (!pinned && inMs && outMs) {
		auto *th = new QHBoxLayout;
		th->setContentsMargins(0, 2, 0, 0);
		th->setSpacing(4);
		const auto makeBox = [&](const Mixed &m, const QString &tip) {
			auto *sp = new QSpinBox(row->box);
			sp->setRange(0, 600000); // up to ten minutes of ramp
			sp->setSuffix(QStringLiteral(" ms"));
			sp->setSpecialValueText(QStringLiteral("0 — instant"));
			sp->setSingleStep(50);
			sp->setToolTip(tip);
			if (m.mixed) {
				// The em-dash convention the rest of this panel uses for a
				// value the selection disagrees about.
				sp->setSpecialValueText(QStringLiteral("—"));
				sp->setValue(0);
			} else {
				sp->setValue(m.value.toInt());
			}
			return sp;
		};
		auto *inLbl = new QLabel(QStringLiteral("In"), row->box);
		inLbl->setStyleSheet(QStringLiteral("border:none; color:#9aa0a8;"));
		row->inMs = makeBox(*inMs,
				    QStringLiteral("Fade this component in over this long, from the "
						   "start of the clip. 0 = on immediately."));
		auto *outLbl = new QLabel(QStringLiteral("Out"), row->box);
		outLbl->setStyleSheet(QStringLiteral("border:none; color:#9aa0a8;"));
		row->outMs = makeBox(*outMs,
				     QStringLiteral("Fade it back out over this long, ending at the "
						    "end of the clip. 0 = on until the end."));
		th->addWidget(inLbl);
		th->addWidget(row->inMs, 1);
		th->addWidget(outLbl);
		th->addWidget(row->outMs, 1);
		bv->addLayout(th);

		// -1 for the box that did not change: the window then leaves that side
		// alone, which is the only way a mixed selection can have one side
		// edited without the other being flattened to whatever this box shows.
		connect(row->inMs, QOverload<int>::of(&QSpinBox::valueChanged), this,
			[this, typeId, ordinal](int v) {
				if (!syncing_)
					emit componentTimingChanged(typeId, ordinal, v, -1);
			});
		connect(row->outMs, QOverload<int>::of(&QSpinBox::valueChanged), this,
			[this, typeId, ordinal](int v) {
				if (!syncing_)
					emit componentTimingChanged(typeId, ordinal, -1, v);
			});
	}

	// The component's own buttons, under its properties. A component declares
	// these; the panel does not know what any of them mean, which is what lets a
	// user-written one have them too.
	if (type && !type->actions.isEmpty()) {
		auto *ah = new QHBoxLayout;
		ah->setContentsMargins(0, 2, 0, 0);
		ah->setSpacing(4);
		for (const ComponentAction &a : type->actions) {
			auto *b = new QPushButton(a.label, row->box);
			b->setStyleSheet(QStringLiteral("padding:3px 8px;"));
			if (!a.help.isEmpty())
				b->setToolTip(a.help);
			connect(b, &QPushButton::clicked, this,
				[this, typeId, ordinal, id = a.id] {
					emit actionInvoked(typeId, ordinal, id);
				});
			ah->addWidget(b);
		}
		ah->addStretch(1);
		bv->addLayout(ah);
	}

	listLayout_->addWidget(row->box);
	return row;
}

void ComponentPanel::rebuild()
{
	for (Row *r : rows_) {
		// Orphan it BEFORE queueing the delete. A rebuild is usually triggered
		// from inside one of these widgets' own signals, so it cannot be
		// deleted outright -- but a deleteLater'd widget stays a child until
		// the event loop gets round to it, and until then it is still found by
		// findChildren, still laid out, and still showing the value it had.
		r->box->setParent(nullptr);
		r->box->hide();
		r->box->deleteLater();
		delete r;
	}
	rows_.clear();

	for (const PinnedRow &p : view_.pinned)
		rows_.append(makeRow(p.typeId, 0, p.values, nullptr, p.driven, p.drivenTip, {},
				     /*pinned=*/true, false, false));
	for (int i = 0; i < view_.shared.size(); ++i) {
		const SharedComponent &c = view_.shared[i];
		rows_.append(makeRow(c.typeId, c.ordinal, c.values, &c.enabled, {}, QString(),
				     c.keyedHere, /*pinned=*/false, i > 0,
				     i + 1 < view_.shared.size(), &c.inMs, &c.outMs));
	}
	pushValues();
	applyPreviewStyle(); // a rebuild makes new widgets; the state outlives them
}

void ComponentPanel::setPreviewing(bool on)
{
	if (previewing_ == on)
		return;
	previewing_ = on;
	applyPreviewStyle();
}

void ComponentPanel::applyPreviewStyle()
{
	// The VALUE controls only. The component's own enable box, its arrows and
	// its remove button are not readings of an instant -- they belong to the
	// clip whatever frame you are looking at -- and greying the whole panel
	// would read as "the Inspector has stopped working" rather than as "these
	// numbers are from the frame under your pointer".
	for (Row *r : rows_) {
		if (!r)
			continue;
		for (ParamSlider *s : r->sliders)
			if (s)
				s->setEnabled(!previewing_);
		for (QCheckBox *c : r->checks)
			if (c)
				c->setEnabled(!previewing_);
		for (QPushButton *b : r->swatches)
			if (b)
				b->setEnabled(!previewing_);
		// The keyframe diamond writes AT THE PLAYHEAD, which is not the frame
		// being shown -- so while previewing it would key the wrong instant.
		for (QPushButton *b : r->keyBtns)
			if (b)
				b->setEnabled(!previewing_);
		for (QLabel *l : r->labels)
			if (l) {
				QFont f = l->font();
				f.setItalic(previewing_);
				l->setFont(f);
			}
	}
}

void ComponentPanel::pushValues()
{
	syncing_ = true;
	auto apply = [](Row *r, const QMap<QString, Mixed> &values, const Mixed *enabled,
			const Mixed *inMs = nullptr, const Mixed *outMs = nullptr) {
		if (r->enabled && enabled) {
			r->enabled->setTristate(enabled->mixed);
			r->enabled->setCheckState(stateOf(*enabled));
		}
		// Ramp times refresh in place like everything else here, so typing in
		// one box is not interrupted by a rebuild.
		const auto pushTime = [](QSpinBox *sp, const Mixed *m) {
			if (!sp || !m)
				return;
			sp->setSpecialValueText(m->mixed ? QStringLiteral("—")
							 : QStringLiteral("0 — instant"));
			sp->setValue(m->mixed ? 0 : m->value.toInt());
		};
		pushTime(r->inMs, inMs);
		pushTime(r->outMs, outMs);
		for (int i = 0; i < r->keys.size(); ++i) {
			const Mixed m = values.value(r->keys[i]);
			if (r->sliders[i]) {
				if (m.mixed)
					r->sliders[i]->setMixed();
				else
					r->sliders[i]->setValue(m.value.toDouble());
			}
			if (r->checks[i]) {
				r->checks[i]->setTristate(m.mixed);
				r->checks[i]->setCheckState(stateOf(m));
			}
			if (r->swatches[i])
				setSwatchColor(r->swatches[i], m);
		}
	};
	int n = 0;
	for (const PinnedRow &p : view_.pinned)
		if (n < rows_.size())
			apply(rows_[n++], p.values, nullptr);
	for (const SharedComponent &c : view_.shared)
		if (n < rows_.size())
			apply(rows_[n++], c.values, &c.enabled, &c.inMs, &c.outMs);
	syncing_ = false;
}

void ComponentPanel::componentMenu(const QString &typeId, int ordinal)
{
	QMenu menu(this);
	// Spelling out "on all 5 clips" rather than leaving it implied: a batch
	// operation that quietly touched more than you meant is not undoable in the
	// user's head even when it is in the program's.
	const QString all = view_.clipCount > 1
				    ? QStringLiteral(" on all %1 clips").arg(view_.clipCount)
				    : QString();
	QAction *reset = menu.addAction(QStringLiteral("Reset to defaults") + all);
	menu.addSeparator();
	QAction *copy = menu.addAction(QStringLiteral("Copy values"));
	QAction *paste = menu.addAction(QStringLiteral("Paste values") + all);
	QAction *dup = menu.addAction(QStringLiteral("Duplicate") + all);
	connect(reset, &QAction::triggered, this,
		[this, typeId, ordinal] { emit componentReset(typeId, ordinal); });
	connect(copy, &QAction::triggered, this,
		[this, typeId, ordinal] { emit componentCopied(typeId, ordinal); });
	connect(paste, &QAction::triggered, this,
		[this, typeId, ordinal] { emit componentPasted(typeId, ordinal); });
	connect(dup, &QAction::triggered, this,
		[this, typeId, ordinal] { emit componentDuplicated(typeId, ordinal); });
	menu.exec(QCursor::pos());
}

void ComponentPanel::addComponentMenu()
{
	QMenu menu(this);
	QString lastCategory;
	QMenu *sub = nullptr;
	for (const ComponentType &t : reg_.all()) {
		if (!t.addable)
			continue; // every clip already has one
		if (t.category != lastCategory) {
			lastCategory = t.category;
			sub = menu.addMenu(t.category.isEmpty() ? QStringLiteral("Other") : t.category);
		}
		QAction *a = (sub ? sub : &menu)->addAction(t.displayName);
		a->setToolTip(t.help);
		const QString id = t.id;
		connect(a, &QAction::triggered, this, [this, id] { emit componentAdded(id); });
	}
	if (menu.isEmpty())
		menu.addAction(QStringLiteral("No components registered"))->setEnabled(false);
	menu.exec(QCursor::pos());
}

} // namespace harpia
