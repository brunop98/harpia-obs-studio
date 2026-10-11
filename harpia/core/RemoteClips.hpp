#pragma once

// The recordings side of the remote control (core/RemoteControl.hpp): what
// GET /recordings lists, and which paths a client may name afterwards.
//
// Pure, tested without a window. MainWindow supplies the clips (its presets'
// output folders, scanned by ClipLibrary -- the same list as the Recent strip
// and the Clip Library), the durations it has probed and the recycle bin.
//
// The rule that matters: a path is acted on only when it is one of those
// clips. A client names a file to delete or open by path, and without this
// check that would be any file on the disk.

#include "core/RemoteControl.hpp"
#include "library/ClipLibrary.hpp"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace harpia::remote_clips {

// One clip as a client sees it: path, name, folder, sizeBytes, size,
// modified (ISO 8601, UTC), modifiedMs, age ("5 minutes ago"), date,
// durationMs (0 = not known yet), duration, preset, kind (video|gif|audio).
QJsonObject toJson(const ClipInfo &clip);

// {"recordings":[the first `limit` clips], "total":all of them,
//  "pending":some durations are still being probed -- ask again shortly}
QJsonObject listJson(const QVector<ClipInfo> &clips, int limit, bool durationsPending);

// The same file? Cleaned, either separator; without regard to case on
// Windows, where the file system has none.
bool samePath(const QString &a, const QString &b);

// Where `path` is in `clips`, or -1.
int find(const QVector<ClipInfo> &clips, const QString &path);

// Deletes each named clip with `remove` (the recycle bin, in the app; true
// when the file is gone). A path that is not one of `clips`, or that
// `refuse` gives a reason for, is left alone. The answer lists both:
//   {"ok":true, "deleted":[..], "failed":[]}
//   {"ok":false, "error":<the first problem>, "deleted":[..], "failed":[{"path","error"}]}
// (404 when nothing named was a recording, 409 otherwise).
RemoteReply deleteClips(const QVector<ClipInfo> &clips, const QStringList &paths,
			const std::function<bool(const QString &)> &remove,
			const std::function<QString(const QString &)> &refuse = {});

// A thumbnail answer: PNG, which every Qt build can write (JPEG needs a
// plugin that a deployment can leave out).
QByteArray png(const QImage &img);

} // namespace harpia::remote_clips
