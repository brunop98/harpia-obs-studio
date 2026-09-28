#pragma once

// Editor problems, in the error log.
//
// Every warning the editor puts on screen, every console error, every failed
// download or transcription also goes into the session log that Help > Error
// logs shows, with where it happened, so "it said something went wrong" can
// be looked up afterwards.
//
// Header-only with a pluggable output: the app points it at the Logger at
// startup (main.cpp). Code built into a unit test without the Logger still
// compiles and falls back to qWarning / qCritical.
//
//   editorLog(EditorLogLevel::Error, "Subtitles", "the API key was rejected (401)");
//   warnAndLog(this, "Add audio", "Could not read any audio from that file.");

#include <QMessageBox>
#include <QString>
#include <QtGlobal>

#include <utility>

namespace harpia {

enum class EditorLogLevel { Error, Warning, Info };

using EditorLogSink = void (*)(EditorLogLevel level, const QString &line);

inline EditorLogSink &editorLogSink()
{
	static EditorLogSink sink = nullptr;
	return sink;
}

inline void editorLog(EditorLogLevel level, const QString &where, const QString &message)
{
	const QString line = QStringLiteral("[Editor] %1: %2").arg(where, message);
	if (EditorLogSink s = editorLogSink()) {
		s(level, line);
		return;
	}
	if (level == EditorLogLevel::Error)
		qCritical("%s", qUtf8Printable(line));
	else if (level == EditorLogLevel::Warning)
		qWarning("%s", qUtf8Printable(line));
	else
		qInfo("%s", qUtf8Printable(line));
}

// QMessageBox::warning / critical that also write what they say to the log.
template <typename... Rest>
QMessageBox::StandardButton warnAndLog(QWidget *parent, const QString &title, const QString &text, Rest &&...rest)
{
	editorLog(EditorLogLevel::Warning, title, QString(text).replace(QLatin1Char('\n'), QLatin1Char(' ')));
	return QMessageBox::warning(parent, title, text, std::forward<Rest>(rest)...);
}

template <typename... Rest>
QMessageBox::StandardButton criticalAndLog(QWidget *parent, const QString &title, const QString &text, Rest &&...rest)
{
	editorLog(EditorLogLevel::Error, title, QString(text).replace(QLatin1Char('\n'), QLatin1Char(' ')));
	return QMessageBox::critical(parent, title, text, std::forward<Rest>(rest)...);
}

} // namespace harpia
