#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

class QWidget;

namespace harpia::window_screen {

// Screen order changes when a display is connected or made primary. Keep
// identity and logical desktop coordinates instead of a screen-list index.
struct Screen {
	QString name;
	QString manufacturer;
	QString model;
	QString serial;
	QRect geometry;
};

// Returns -1 if the saved display is no longer connected. Geometry only
// breaks ties between displays with the same reported identity.
int findScreen(const Screen &saved, const QVector<Screen> &screens);
QPoint centeredTopLeft(const QSize &frameSize, const QRect &available);
// Explicit popup actions follow the click, rather than remembered placement.
void placeAtClick(QWidget &window, const QPoint &globalClick, QWidget *fallback = nullptr);

// Separate keys let the recorder and editor remember different monitors.
// Restore on the first show, after QDialog's parent-relative positioning but
// before maximising. Save also accepts hidden windows (QDialog::finished).
bool restore(QWidget &window, const QString &key);
void save(QWidget &window, const QString &key);

} // namespace harpia::window_screen
