#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

#include <atomic>
#include <memory>

class QLabel;
class QLineEdit;

namespace harpia {

// File metadata appears immediately; container metadata is read on a worker so
// a large recording never blocks the details window or the editor behind it.
class MediaFileDetailsDialog : public QDialog {
	Q_OBJECT
public:
	explicit MediaFileDetailsDialog(const QString &filePath, QWidget *parent = nullptr);
	~MediaFileDetailsDialog() override;

private:
	QLineEdit *duration_ = nullptr;
	QLineEdit *resolution_ = nullptr;
	QLineEdit *codec_ = nullptr;
	QLineEdit *frameRate_ = nullptr;
	QLineEdit *audio_ = nullptr;
	QLabel *status_ = nullptr;
	QList<QPair<QString, QLineEdit *>> fields_;
	std::shared_ptr<std::atomic<bool>> canceled_;
};

} // namespace harpia
