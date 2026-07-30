#include "MonitorMatch.hpp"

#include <algorithm>

namespace harpia {
namespace {
const char *g_method = "none";
}

const char *lastMatchMethod()
{
	return g_method;
}

int clampMonitorIndex(int index, int obsMonitorCount)
{
	if (obsMonitorCount <= 0)
		return 0;
	return (index < 0 || index >= obsMonitorCount) ? 0 : index;
}

int matchQtScreen(const QVector<QtScreenDesc> &screens, const GdiDisplayDesc &target,
		  const QVector<GdiDisplayDesc> &allGdi)
{
	g_method = "none";
	if (screens.isEmpty() || target.gdiName.isEmpty())
		return -1;

	// 1) Qt 5 style: QScreen::name() IS the GDI name. Accept it with or without
	//    the "\\.\" prefix, since the two sides do not always carry it.
	for (int i = 0; i < screens.size(); ++i) {
		const QString &qn = screens[i].name;
		if (qn.isEmpty())
			continue;
		if (qn.compare(target.gdiName, Qt::CaseInsensitive) == 0 ||
		    target.gdiName.endsWith(qn, Qt::CaseInsensitive) ||
		    qn.endsWith(target.gdiName, Qt::CaseInsensitive)) {
			g_method = "GDI name";
			return i;
		}
	}

	// 2) Qt 6 style: QScreen::name() is the monitor's friendly name. Only
	//    trusted when it identifies exactly one screen -- two identical
	//    monitors report the same friendly name, and picking either at random
	//    is how the overlay lands on the wrong one of a matched pair.
	if (!target.friendly.isEmpty()) {
		int hit = -1, count = 0;
		for (int i = 0; i < screens.size(); ++i)
			if (screens[i].name.compare(target.friendly, Qt::CaseInsensitive) == 0) {
				hit = i;
				++count;
			}
		if (count == 1) {
			g_method = "friendly name";
			return hit;
		}
	}

	// 3) Arrangement rank: Qt preserves the physical left-to-right,
	//    top-to-bottom layout, so the Nth display by native position is the Nth
	//    QScreen by logical position. That holds under mixed DPI, where the
	//    coordinates differ but the ORDER does not.
	//
	//    Only when the two lists describe the same number of displays. With
	//    different counts the ranks address different things, so the answer
	//    would be a coincidence -- and it is also what keeps `rank` inside
	//    `screens`: with three displays and two screens the rank can be 2, and
	//    indexing the screen list with it is an out-of-bounds read, which the
	//    negative control for this line reaches as an abort rather than as a
	//    merely wrong monitor.
	if (allGdi.size() != screens.size())
		return -1;

	QVector<int> gdiOrder(allGdi.size());
	for (int i = 0; i < allGdi.size(); ++i)
		gdiOrder[i] = i;
	std::sort(gdiOrder.begin(), gdiOrder.end(), [&](int a, int b) {
		const QPoint pa = allGdi[a].topLeft, pb = allGdi[b].topLeft;
		return pa.x() != pb.x() ? pa.x() < pb.x() : pa.y() < pb.y();
	});
	int rank = -1;
	for (int i = 0; i < gdiOrder.size(); ++i)
		if (allGdi[gdiOrder[i]].gdiName.compare(target.gdiName, Qt::CaseInsensitive) == 0)
			rank = i;
	if (rank < 0)
		return -1;

	QVector<int> screenOrder(screens.size());
	for (int i = 0; i < screens.size(); ++i)
		screenOrder[i] = i;
	std::sort(screenOrder.begin(), screenOrder.end(), [&](int a, int b) {
		const QPoint pa = screens[a].topLeft, pb = screens[b].topLeft;
		return pa.x() != pb.x() ? pa.x() < pb.x() : pa.y() < pb.y();
	});
	g_method = "arrangement rank";
	return screenOrder[rank];
}

} // namespace harpia
