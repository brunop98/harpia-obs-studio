#include "ComponentPanel.hpp"

#include "../ParamSlider.hpp"
#include "../../ui/UiIcons.hpp"
#include "../../ui/UiText.hpp"
#include "ComponentRegistry.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

// One component's widgets. Kept so pushValues() can update them in place rather
// than rebuilding and stealing the focus from whoever is typing.
struct ComponentPanel::Row {
	QFrame *box = nullptr;
	QCheckBox *enabled = nullptr;
	QVector<QString> keys;          // property key per control, parallel to the two below
	QVector<ParamSlider *> sliders; // null for a non-float property
	QVector<QCheckBox *> checks;    // null for a non-bool property
	QVector<QPushButton *> keyBtns;
	QVector<QLabel *> labels;
};

namespace {

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

bool hasKeyAt(const ComponentInstance &c, const QString &key, qint64 tMs)
{
	const auto it = c.keys.find(key);
	if (it == c.keys.end())
		return false;
	for (const PropKey &k : *it)
		if (std::abs(k.tMs - tMs) <= 1)
			return true;
	return false;
}

} // namespace

ComponentPanel::ComponentPanel(const ComponentRegistry &reg, QWidget *parent)
	: QWidget(parent), reg_(reg)
{
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(0, 6, 0, 0);
	v->setSpacing(5);

	auto *hdr = new QLabel(QStringLiteral("Components"), this);
	hdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	v->addWidget(hdr);

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

	empty_ = new QLabel(QStringLiteral("No components. Add one to change how this clip "
					   "plays, moves or looks."),
			    this);
	empty_->setWordWrap(true);
	empty_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(empty_);

	pinnedLayout_ = new QVBoxLayout;
	pinnedLayout_->setSpacing(6);
	v->addLayout(pinnedLayout_);

	listLayout_ = new QVBoxLayout;
	listLayout_->setSpacing(6);
	v->addLayout(listLayout_);

	auto *add = new QPushButton(QStringLiteral("Add Component"), this);
	add->setToolTip(QStringLiteral("Everything a clip does is a component — built-in or one "
				       "you wrote yourself."));
	connect(add, &QPushButton::clicked, this, &ComponentPanel::addComponentMenu);
	v->addWidget(add);
}

void ComponentPanel::setPinnedTransform(const TlTransform &xf, bool present,
					const QStringList &drivenKeys, const QString &drivenTip)
{
	const bool structural = present != pinnedPresent_ || drivenKeys != drivenKeys_;
	pinnedXf_ = xf;
	pinnedPresent_ = present;
	drivenKeys_ = drivenKeys;
	drivenTip_ = drivenTip;
	if (structural)
		buildPinnedRow();
	else
		pushPinnedValues();
}

// A value out of the pose row, by property key. One place, so the row's order
// and the struct's fields cannot drift apart.
static double poseGet(const TlTransform &t, const QString &key)
{
	if (key == QLatin1String("posX"))
		return t.posX;
	if (key == QLatin1String("posY"))
		return t.posY;
	if (key == QLatin1String("scale"))
		return t.scale;
	if (key == QLatin1String("rotation"))
		return t.rotation;
	return t.opacity;
}
static void poseSet(TlTransform &t, const QString &key, double v)
{
	if (key == QLatin1String("posX"))
		t.posX = v;
	else if (key == QLatin1String("posY"))
		t.posY = v;
	else if (key == QLatin1String("scale"))
		t.scale = v;
	else if (key == QLatin1String("rotation"))
		t.rotation = v;
	else
		t.opacity = v;
}

void ComponentPanel::buildPinnedRow()
{
	if (pinned_) {
		pinned_->box->deleteLater();
		delete pinned_;
		pinned_ = nullptr;
	}
	const ComponentType *type = reg_.find(QStringLiteral("harpia.transform"));
	if (!pinnedPresent_ || !type)
		return;

	auto *row = new Row;
	row->box = new QFrame(this);
	row->box->setFrameShape(QFrame::StyledPanel);
	row->box->setStyleSheet(
		QStringLiteral("QFrame{border:1px solid #3a3f47; border-radius:4px;}"));
	auto *bv = new QVBoxLayout(row->box);
	bv->setContentsMargins(6, 4, 6, 6);
	bv->setSpacing(4);

	auto *head = new QHBoxLayout;
	head->setSpacing(4);
	auto *name = new QLabel(type->displayName, row->box);
	name->setStyleSheet(QStringLiteral("border:none; font-weight:bold; color:#e8eaed;"));
	// No enable box, no move arrows, no remove: a clip without a pose is not a
	// thing, so offering to take it away would be offering nonsense.
	name->setToolTip(QStringLiteral("Where the clip sits on the canvas. Every clip has one, "
					"so this row cannot be removed or reordered."));
	head->addWidget(name, 1);
	head->addWidget(stageBadge(type->stage, row->box));
	bv->addLayout(head);

	auto *form = new QFormLayout;
	form->setHorizontalSpacing(8);
	form->setVerticalSpacing(3);
	for (const PropDef &d : type->props) {
		row->keys.append(d.key);
		auto *sl = new ParamSlider(d.min, d.max, 3, row->box);
		sl->setValue(poseGet(pinnedXf_, d.key));
		if (!d.help.isEmpty())
			sl->setToolTip(d.help);
		connect(sl, &ParamSlider::valueChanged, this, [this, key = d.key](double v) {
			if (syncing_)
				return;
			poseSet(pinnedXf_, key, v);
			emit transformEdited(pinnedXf_);
		});
		row->sliders.append(sl);
		row->checks.append(nullptr);
		row->keyBtns.append(nullptr);

		// A script that computes this channel makes the box a starting point
		// (it reads it as ctx.base), not the framing on screen. Saying so on the
		// row beats leaving the mismatch to be discovered.
		const bool driven = drivenKeys_.contains(d.key);
		auto *lbl = new QLabel(driven ? d.label + QStringLiteral("  (script)") : d.label,
				       row->box);
		lbl->setStyleSheet(driven ? QStringLiteral("border:none; color:#ffd44f;")
					  : QStringLiteral("border:none;"));
		if (driven)
			lbl->setToolTip(drivenTip_);
		row->labels.append(lbl);
		form->addRow(lbl, sl);
	}
	bv->addLayout(form);
	pinnedLayout_->addWidget(row->box);
	pinned_ = row;
}

void ComponentPanel::pushPinnedValues()
{
	if (!pinned_)
		return;
	syncing_ = true;
	for (int i = 0; i < pinned_->keys.size(); ++i)
		if (pinned_->sliders[i])
			pinned_->sliders[i]->setValue(poseGet(pinnedXf_, pinned_->keys[i]));
	syncing_ = false;
}

void ComponentPanel::setLoadErrors(const QStringList &errors)
{
	loadErrors_->setText(errors.join(QChar('\n')));
	loadErrors_->setVisible(!errors.isEmpty());
}

bool ComponentPanel::shapeChanged(const QVector<ComponentInstance> &next) const
{
	if (next.size() != list_.size())
		return true;
	for (int i = 0; i < next.size(); ++i)
		if (next[i].typeId != list_[i].typeId || next[i].instanceId != list_[i].instanceId)
			return true;
	return false;
}

void ComponentPanel::setComponents(const QVector<ComponentInstance> &list, qint64 clipTimeMs)
{
	const bool structural = shapeChanged(list);
	list_ = list;
	timeMs_ = clipTimeMs;
	if (structural)
		rebuild();
	else
		pushValues();

	QStringList w;
	{
		// Order and conflicts are recomputed here rather than cached, because the
		// panel is the only place the user can be told about them.
		QStringList orderWarnings;
		resolveOrder(list_, reg_, &orderWarnings);
		w = orderWarnings + findConflicts(list_, reg_);
	}
	warnings_->setText(w.join(QChar('\n')));
	warnings_->setVisible(!w.isEmpty());
	// The empty-state hint is about ADDABLE components; the pose row is always
	// there and would make "no components" read as a lie.
	empty_->setVisible(list_.isEmpty());
}

void ComponentPanel::rebuild()
{
	for (Row *r : rows_) {
		r->box->deleteLater();
		delete r;
	}
	rows_.clear();

	for (int i = 0; i < list_.size(); ++i) {
		const ComponentInstance &c = list_[i];
		const ComponentType *type = reg_.find(c.typeId);

		auto *row = new Row;
		row->box = new QFrame(this);
		row->box->setFrameShape(QFrame::StyledPanel);
		row->box->setStyleSheet(
			QStringLiteral("QFrame{border:1px solid #3a3f47; border-radius:4px;}"));
		auto *bv = new QVBoxLayout(row->box);
		bv->setContentsMargins(6, 4, 6, 6);
		bv->setSpacing(4);

		auto *head = new QHBoxLayout;
		head->setSpacing(4);
		row->enabled = new QCheckBox(row->box);
		row->enabled->setChecked(c.enabled);
		row->enabled->setToolTip(QStringLiteral("Turn this component off without losing its "
						       "settings."));
		connect(row->enabled, &QCheckBox::toggled, this, [this, i](bool on) {
			if (syncing_ || i >= list_.size())
				return;
			list_[i].enabled = on;
			emitEdit();
		});
		head->addWidget(row->enabled);

		// A component the project names but this build has no plugin for. Its
		// settings are intact and will be written back out; saying so is the
		// difference between "your work is safe" and "something vanished".
		auto *name = new QLabel(type ? type->displayName
					     : QStringLiteral("Missing: %1").arg(c.typeId),
					row->box);
		name->setStyleSheet(QStringLiteral("border:none; font-weight:bold; color:%1;")
					    .arg(type ? QStringLiteral("#e8eaed")
						      : QStringLiteral("#e2a03f")));
		if (!type)
			name->setToolTip(QStringLiteral(
				"This project uses a component that is not installed here. Its "
				"settings are kept and saved back unchanged, so nothing is lost "
				"— it just cannot render on this machine."));
		else if (!type->help.isEmpty())
			name->setToolTip(type->help);
		head->addWidget(name, 1);
		if (type)
			head->addWidget(stageBadge(type->stage, row->box));

		auto *up = iconButton(Glyph::ArrowUp, QStringLiteral("Move up"), row->box);
		up->setEnabled(i > 0);
		connect(up, &QPushButton::clicked, this, [this, i] {
			if (i <= 0 || i >= list_.size())
				return;
			list_.swapItemsAt(i, i - 1);
			emitEdit();
		});
		auto *down = iconButton(Glyph::ArrowDown, QStringLiteral("Move down"), row->box);
		down->setEnabled(i + 1 < list_.size());
		connect(down, &QPushButton::clicked, this, [this, i] {
			if (i < 0 || i + 1 >= list_.size())
				return;
			list_.swapItemsAt(i, i + 1);
			emitEdit();
		});
		auto *del = iconButton(Glyph::Cross, QStringLiteral("Remove this component"),
				       row->box);
		connect(del, &QPushButton::clicked, this, [this, i] {
			if (i < 0 || i >= list_.size())
				return;
			list_.remove(i);
			emitEdit();
		});
		head->addWidget(up);
		head->addWidget(down);
		head->addWidget(del);
		bv->addLayout(head);

		if (type && !type->props.isEmpty()) {
			auto *form = new QFormLayout;
			form->setHorizontalSpacing(8);
			form->setVerticalSpacing(3);
			for (const PropDef &d : type->props) {
				row->keys.append(d.key);
				QWidget *control = nullptr;
				ParamSlider *slider = nullptr;
				QCheckBox *check = nullptr;

				if (d.type == PropType::Bool) {
					check = new QCheckBox(row->box);
					check->setChecked(propAt(*type, c, d.key, timeMs_).toBool());
					connect(check, &QCheckBox::toggled, this,
						[this, i, key = d.key](bool on) {
							if (syncing_ || i >= list_.size())
								return;
							list_[i].props.insert(key, on);
							emitEdit();
						});
					control = check;
				} else {
					slider = new ParamSlider(d.min, d.max, 3, row->box);
					slider->setValue(
						propAt(*type, c, d.key, timeMs_).toDouble());
					connect(slider, &ParamSlider::valueChanged, this,
						[this, i, key = d.key](double v) {
							if (syncing_ || i >= list_.size())
								return;
							// Editing an ANIMATED property writes to the
							// key at the playhead, not to the static
							// value -- which the keys would override on
							// the next frame, leaving the control
							// looking dead.
							auto k = list_[i].keys.find(key);
							if (k != list_[i].keys.end() && !k->isEmpty()) {
								for (PropKey &pk : *k)
									if (std::abs(pk.tMs - timeMs_) <= 1) {
										pk.v = v;
										emitEdit();
										return;
									}
								PropKey pk;
								pk.tMs = timeMs_;
								pk.v = v;
								k->append(pk);
								std::sort(k->begin(), k->end(),
									  [](const PropKey &a,
									     const PropKey &b) {
										  return a.tMs < b.tMs;
									  });
							} else {
								list_[i].props.insert(key, v);
							}
							emitEdit();
						});
					control = slider;
				}
				row->sliders.append(slider);
				row->checks.append(check);
				if (!d.help.isEmpty())
					control->setToolTip(d.help);

				QPushButton *kb = nullptr;
				if (d.keyframeable && d.type != PropType::Bool) {
					kb = iconButton(Glyph::Diamond,
							QStringLiteral("Key this value at the "
								       "playhead (again to remove)"),
							row->box);
					connect(kb, &QPushButton::clicked, this,
						[this, i, key = d.key] {
							if (i < 0 || i >= list_.size())
								return;
							auto &keys = list_[i].keys[key];
							for (int n = 0; n < keys.size(); ++n)
								if (std::abs(keys[n].tMs - timeMs_) <= 1) {
									keys.remove(n);
									// A property with no keys left is
									// not animated; leaving an empty
									// list behind would keep the
									// control reading as keyed.
									if (keys.isEmpty())
										list_[i].keys.remove(key);
									emitEdit();
									return;
								}
							PropKey pk;
							pk.tMs = timeMs_;
							const ComponentType *t =
								reg_.find(list_[i].typeId);
							pk.v = t ? propAt(*t, list_[i], key, timeMs_)
									   .toDouble()
								 : 0.0;
							keys.append(pk);
							std::sort(keys.begin(), keys.end(),
								  [](const PropKey &a, const PropKey &b) {
									  return a.tMs < b.tMs;
								  });
							emitEdit();
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
				auto *lbl = new QLabel(d.label, row->box);
				lbl->setStyleSheet(QStringLiteral("border:none;"));
				form->addRow(lbl, cell);
			}
			bv->addLayout(form);
		}
		listLayout_->addWidget(row->box);
		rows_.append(row);
	}
	pushValues();
}

void ComponentPanel::pushValues()
{
	syncing_ = true;
	for (int i = 0; i < rows_.size() && i < list_.size(); ++i) {
		Row *r = rows_[i];
		const ComponentInstance &c = list_[i];
		const ComponentType *type = reg_.find(c.typeId);
		r->enabled->setChecked(c.enabled);
		if (!type)
			continue;
		for (int k = 0; k < r->keys.size(); ++k) {
			const QVariant v = propAt(*type, c, r->keys[k], timeMs_);
			if (r->sliders[k])
				r->sliders[k]->setValue(v.toDouble());
			if (r->checks[k])
				r->checks[k]->setChecked(v.toBool());
			if (r->keyBtns[k]) {
				// Filled when there is a key exactly here, dim when the
				// property is animated but this instant is between keys.
				const bool here = hasKeyAt(c, r->keys[k], timeMs_);
				const bool animated = c.keys.contains(r->keys[k]);
				r->keyBtns[k]->setIcon(uiIcon(Glyph::Diamond, 11,
							      here ? QColor(0xe5, 0x48, 0x4d)
								   : animated ? QColor(0xc8, 0xa0, 0x60)
									      : QColor()));
			}
		}
	}
	syncing_ = false;
}

void ComponentPanel::emitEdit()
{
	emit componentsEdited(list_);
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
			sub = menu.addMenu(t.category.isEmpty() ? QStringLiteral("Other")
								: t.category);
		}
		QAction *a = (sub ? sub : &menu)->addAction(t.displayName);
		a->setToolTip(t.help);
		const QString id = t.id;
		connect(a, &QAction::triggered, this, [this, id] {
			ComponentInstance c;
			c.typeId = id;
			// A stable identity, so another component can refer to this one and
			// so the panel can tell two of the same type apart.
			c.instanceId =
				QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
			list_.append(c);
			emitEdit();
		});
	}
	if (menu.isEmpty())
		menu.addAction(QStringLiteral("No components registered"))->setEnabled(false);
	menu.exec(QCursor::pos());
}

} // namespace harpia
