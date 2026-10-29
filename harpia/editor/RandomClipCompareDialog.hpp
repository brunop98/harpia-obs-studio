#pragma once

#include "timeline/TimelineModel.hpp"

#include <QDialog>
#include <QImage>
#include <QMap>
#include <QThread>

#include <memory>

namespace harpia {

// A read-only snapshot of one Random Clip. Each tile renders the same output
// instant with its own footage and framing, using the real timeline compositor.
class RandomClipCompareDialog : public QDialog {
public:
	struct Source {
		int id = 0;
		QString name;
		QString decodePath;
		QSize originalSize;
		bool missing = false;
	};
	struct Input {
		TlClip clip;
		QVector<Source> sources;
		QSize canvas{1920, 1080};
		double fps = 30.0;
		QMap<QString, QString> scripts;
	};

	explicit RandomClipCompareDialog(const Input &input, QWidget *parent = nullptr);
	~RandomClipCompareDialog() override;
	void done(int result) override;
	void setPlaying(bool playing);
	void seekTo(qint64 localMs);
	bool isPlaying() const;
	qint64 playheadMs() const;
	qint64 durationMs() const;
	int optionCount() const;
	QSize renderSize() const;
	QImage previewImage(int option) const;
	QString optionError(int option) const;

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void requestFrame(qint64 localMs);
	void acceptBatch();
	void arrangeTiles();
	void stopRendering();
	struct Impl;
	std::unique_ptr<Impl> d_;
};

// The thread owns all decoders and script engines. One outstanding batch and
// one latest scrub request keep work bounded, even when rendering is slow.
class RandomClipCompareRenderer final : public QThread {
	Q_OBJECT
public:
	struct Batch {
		qint64 localMs = 0;
		quint64 revision = 0;
		QVector<QImage> images;
		QStringList errors;
	};
	explicit RandomClipCompareRenderer(const RandomClipCompareDialog::Input &input, QSize renderSize);
	~RandomClipCompareRenderer() override;
	void request(qint64 localMs, quint64 revision);
	void cancel();
	Batch takeBatch();

signals:
	void batchReady();

protected:
	void run() override;

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

} // namespace harpia
