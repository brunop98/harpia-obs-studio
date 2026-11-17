#pragma once

#include <QList>
#include <QSslCertificate>
#include <QSslError>

class QSslConfiguration;
class QUrl;

namespace harpia {

bool verifyAudioLibRemoteServerCertificate(const QUrl &url, const QSslConfiguration &ssl,
					   const QList<QSslCertificate> &authorities, const QList<QSslError> &errors = {});

} // namespace harpia
