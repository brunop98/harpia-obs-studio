#include "AudioLibRemoteServerWindowsTrust.hpp"

#ifdef Q_OS_WIN
#include <QHostAddress>
#include <QSslConfiguration>
#include <QUrl>
#include <QtEndian>

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace harpia {
namespace {

template<typename T>
std::unique_ptr<T, decltype(&LocalFree)> decodeExtension(const CERT_EXTENSION &extension, LPCSTR type)
{
	T *decoded = nullptr;
	DWORD size = 0;
	const BOOL valid = CryptDecodeObjectEx(X509_ASN_ENCODING, type, extension.Value.pbData, extension.Value.cbData,
					      CRYPT_DECODE_ALLOC_FLAG, nullptr, &decoded, &size);
	return {valid ? decoded : nullptr, &LocalFree};
}

bool allowsServerAuth(PCCERT_CONTEXT certificate, bool required)
{
	const auto *info = certificate->pCertInfo;
	const auto *extension = CertFindExtension(szOID_ENHANCED_KEY_USAGE, info->cExtension, info->rgExtension);
	if (!extension)
		return !required;

	const auto usage = decodeExtension<CERT_ENHKEY_USAGE>(*extension, X509_ENHANCED_KEY_USAGE);
	if (!usage)
		return false;
	for (DWORD i = 0; i < usage->cUsageIdentifier; ++i) {
		if (std::strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH) == 0)
			return true;
	}
	return false;
}

bool matchesIp(const CERT_ALT_NAME_ENTRY &name, const QHostAddress &address)
{
	if (name.dwAltNameChoice != CERT_ALT_NAME_IP_ADDRESS)
		return false;
	const auto &ip = name.IPAddress;
	if (ip.cbData == 4)
		return address.isEqual(QHostAddress(qFromBigEndian<quint32>(ip.pbData)), QHostAddress::StrictConversion);
	return ip.cbData == 16 && address.isEqual(QHostAddress(ip.pbData), QHostAddress::StrictConversion);
}

bool verifyDirectChain(const QByteArray &peerDer, const QByteArray &rootDer, const QHostAddress &address,
		       bool requireNameConstraintError)
{
	const auto closeStore = [](HCERTSTORE store) { CertCloseStore(store, 0); };
	const std::unique_ptr<void, decltype(closeStore)> roots(
		CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr), closeStore);
	if (!roots || !CertAddEncodedCertificateToStore(roots.get(), X509_ASN_ENCODING,
							reinterpret_cast<const BYTE *>(rootDer.constData()),
							DWORD(rootDer.size()), CERT_STORE_ADD_NEW, nullptr))
		return false;

	CERT_CHAIN_ENGINE_CONFIG config{};
	config.cbSize = sizeof(config);
	config.hExclusiveRoot = roots.get();
	HCERTCHAINENGINE engineHandle = nullptr;
	if (!CertCreateCertificateChainEngine(&config, &engineHandle))
		return false;
	const std::unique_ptr<void, decltype(&CertFreeCertificateChainEngine)> engine(engineHandle,
										    &CertFreeCertificateChainEngine);
	const std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> leaf(
		CertCreateCertificateContext(X509_ASN_ENCODING, reinterpret_cast<const BYTE *>(peerDer.constData()),
					     DWORD(peerDer.size())),
		&CertFreeCertificateContext);
	if (!leaf)
		return false;

	const auto *info = leaf->pCertInfo;
	const auto *extension = CertFindExtension(szOID_SUBJECT_ALT_NAME2, info->cExtension, info->rgExtension);
	if (!extension)
		return false;
	const auto names = decodeExtension<CERT_ALT_NAME_INFO>(*extension, X509_ALTERNATE_NAME);
	if (!names || names->cAltEntry == 0 ||
	    !std::any_of(names->rgAltEntry, names->rgAltEntry + names->cAltEntry,
			 [&address](const CERT_ALT_NAME_ENTRY &name) { return matchesIp(name, address); }))
		return false;

	// Qt has already run its revocation checks. The direct chain can be built locally.
	constexpr DWORD chainFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AIA;
	const auto buildChain = [&](LPSTR *requestedUsage) {
		CERT_CHAIN_PARA params{};
		params.cbSize = sizeof(params);
		if (requestedUsage) {
			params.RequestedUsage.Usage.cUsageIdentifier = 1;
			params.RequestedUsage.Usage.rgpszUsageIdentifier = requestedUsage;
		}
		PCCERT_CHAIN_CONTEXT context = nullptr;
		const BOOL built = CertGetCertificateChain(engine.get(), leaf.get(), nullptr, roots.get(), &params,
							   chainFlags, nullptr, &context);
		return std::unique_ptr<const CERT_CHAIN_CONTEXT, decltype(&CertFreeCertificateChain)>(
			built ? context : nullptr, &CertFreeCertificateChain);
	};

	// For serverAuth, Schannel also applies DNS constraints to XA1's descriptive CN.
	// The generic chain checks constraints on XA1's IP SAN. Check the TLS purpose
	// of both certificates separately.
	char serverAuth[] = szOID_PKIX_KP_SERVER_AUTH;
	LPSTR requestedUsage = serverAuth;
	auto tlsChain = buildChain(&requestedUsage);
	if (!tlsChain)
		return false;
	const DWORD tlsStatus = tlsChain->TrustStatus.dwErrorStatus;
	const bool nameConstraintError = tlsStatus == CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT;
	if (!nameConstraintError && (requireNameConstraintError || tlsStatus != CERT_TRUST_NO_ERROR))
		return false;
	// XA1 issues a leaf with exactly one SAN for its current LAN IP.
	if (nameConstraintError && (names->cAltEntry != 1 || !allowsServerAuth(leaf.get(), true)))
		return false;
	const auto chain = nameConstraintError ? buildChain(nullptr) : std::move(tlsChain);
	if (!chain || chain->TrustStatus.dwErrorStatus != CERT_TRUST_NO_ERROR || chain->cChain != 1 ||
	    chain->rgpChain[0]->cElement != 2)
		return false;

	const auto *root = chain->rgpChain[0]->rgpElement[1]->pCertContext;
	if (root->cbCertEncoded != DWORD(rootDer.size()) ||
	    std::memcmp(root->pbCertEncoded, rootDer.constData(), rootDer.size()) != 0 ||
	    (nameConstraintError && !allowsServerAuth(root, false)))
		return false;

	CERT_CHAIN_POLICY_PARA policy{};
	policy.cbSize = sizeof(policy);
	policy.dwFlags = BASIC_CONSTRAINTS_CERT_CHAIN_POLICY_END_ENTITY_FLAG;
	CERT_CHAIN_POLICY_STATUS status{};
	status.cbSize = sizeof(status);
	return CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASIC_CONSTRAINTS, chain.get(), &policy, &status) &&
	       status.dwError == 0;
}

} // namespace

bool verifyAudioLibRemoteServerCertificate(const QUrl &url, const QSslConfiguration &ssl,
					   const QList<QSslCertificate> &authorities, const QList<QSslError> &errors)
{
	const QHostAddress address(url.host());
	const QSslCertificate leaf = ssl.peerCertificate();
	if (address.isNull() || leaf.isNull())
		return false;

	const QByteArray peerDer = leaf.toDer();
	if (!errors.isEmpty()) {
		const QSslCertificate root = errors.constFirst().certificate();
		return root != leaf && root.isSelfSigned() && authorities.contains(root) &&
		       std::all_of(errors.cbegin(), errors.cend(), [&root](const QSslError &error) {
			       return error.error() == QSslError::UnspecifiedError && error.certificate() == root;
		       }) &&
		       verifyDirectChain(peerDer, root.toDer(), address, true);
	}
	return std::any_of(authorities.cbegin(), authorities.cend(), [&](const QSslCertificate &root) {
		return root != leaf && root.isSelfSigned() && verifyDirectChain(peerDer, root.toDer(), address, false);
	});
}

} // namespace harpia
#endif
