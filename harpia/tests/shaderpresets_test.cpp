// Stock fixes reach existing installations without overwriting user shaders.
// This is deliberately GL-free and independent of compiled application resources.
#include "editor/shader/ShaderPresets.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool okay, const char *description)
{
	std::printf("%s %s\n", okay ? "PASS" : "FAIL", description);
	failures += !okay;
}
QByteArray read(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return file.readAll();
}
bool write(const QString &path, const QByteArray &contents)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const QString fixture = QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir()
		.filePath(QStringLiteral("fixtures/crt-v1.frag"));
	const QByteArray legacy = read(fixture);
	check(!legacy.isEmpty() && QCryptographicHash::hash(legacy, QCryptographicHash::Sha256).toHex() ==
	      QByteArrayLiteral("aa99007a0b3fb941144300504ee0ce2682de37a20efc96d2dd226f2c40400e52"),
	      "the legacy fixture is the exact CRT distributed before the update");
	const QByteArray current = QByteArrayLiteral("// New bundled CRT version\nvoid mainImage(out vec4 c, in vec2 p){c=vec4(1.0);}\n");
	const QByteArray adjust = QByteArrayLiteral("// Bundled adjustments\n");
	const QMap<QString, QByteArray> bundle{{QStringLiteral("crt.frag"), current}, {QStringLiteral("adjust.frag"), adjust}};
	QTemporaryDir dir;
	const QString crtPath = dir.filePath(QStringLiteral("crt.frag"));
	const QString adjustPath = dir.filePath(QStringLiteral("adjust.frag"));
	const auto seeded = ensureDefaultShaderPresets(dir.path(), bundle);
	check(seeded.created == 2 && seeded.updated == 0 && seeded.errors.isEmpty() &&
	      read(crtPath) == current && read(adjustPath) == adjust,
	      "a new install receives every available bundled shader");
	const auto again = ensureDefaultShaderPresets(dir.path(), bundle);
	check(again.created == 0 && again.updated == 0 && again.errors.isEmpty() && read(crtPath) == current,
	      "current stock shaders are a no-op and seeding is idempotent");
	check(write(crtPath, legacy), "the existing-install fixture can be installed");
	const auto upgraded = ensureDefaultShaderPresets(dir.path(), bundle);
	check(upgraded.updated == 1 && upgraded.created == 0 && upgraded.errors.isEmpty() && read(crtPath) == current,
	      "an untouched legacy CRT upgrades to the new bundled bytes");
	QByteArray windowsLegacy = legacy;
	windowsLegacy.replace("\n", "\r\n");
	check(write(crtPath, windowsLegacy), "the Windows stock line-ending fixture can be installed");
	const auto windowsUpgrade = ensureDefaultShaderPresets(dir.path(), bundle);
	check(windowsUpgrade.updated == 1 && read(crtPath) == current,
	      "stock CRT with CRLF line endings is recognized without treating different shader text as stock");
	const QByteArray custom = legacy + QByteArrayLiteral("\n// My own CRT modifications\n");
	check(write(crtPath, custom), "the edited CRT fixture can be installed");
	const auto preserved = ensureDefaultShaderPresets(dir.path(), bundle);
	check(preserved.updated == 0 && preserved.created == 0 && preserved.errors.isEmpty() && read(crtPath) == custom,
	      "a custom CRT is preserved byte-for-byte despite retaining all old controls");
	check(write(adjustPath, legacy), "a differently named shader can contain legacy CRT code");
	ensureDefaultShaderPresets(dir.path(), bundle);
	check(read(adjustPath) == legacy, "legacy fingerprints are scoped to crt.frag and never overwrite other user shaders");
	check(write(crtPath, legacy), "the missing-resource fixture can be installed");
	const auto absent = ensureDefaultShaderPresets(dir.path(), {});
	check(absent.created == 0 && absent.updated == 0 && absent.errors.isEmpty() && read(crtPath) == legacy,
	      "a build without shader resources leaves existing stock and custom files untouched");
	const auto empty = ensureDefaultShaderPresets(dir.path(), {{QStringLiteral("crt.frag"), QByteArray()}});
	check(!empty.errors.isEmpty() && read(crtPath) == legacy,
	      "an empty resource cannot erase an existing shader");
	QTemporaryDir failure;
	const QString protectedPath = failure.filePath(QStringLiteral("crt.frag"));
	check(QDir().mkpath(protectedPath), "the unreadable destination fixture can be created");
	const auto failed = ensureDefaultShaderPresets(failure.path(), {{QStringLiteral("crt.frag"), current}});
	check(!failed.errors.isEmpty() && failed.updated == 0 && QFileInfo(protectedPath).isDir(),
	      "a failed update reports the error and preserves the existing destination");
	const QString invalidFolder = failure.filePath(QStringLiteral("not-a-folder"));
	check(write(invalidFolder, custom), "the blocked-folder fixture can be created");
	const auto blocked = ensureDefaultShaderPresets(invalidFolder, bundle);
	check(!blocked.errors.isEmpty() && read(invalidFolder) == custom,
	      "a failed folder creation cannot damage an existing file");
	QTemporaryDir links;
	const QString target = links.filePath(QStringLiteral("user-managed.frag"));
	const QString link = links.filePath(QStringLiteral("crt.frag"));
	write(target, legacy);
	if (QFile::link(target, link)) {
		const auto linked = ensureDefaultShaderPresets(links.path(), {{QStringLiteral("crt.frag"), current}});
		check(linked.updated == 0 && QFileInfo(link).isSymLink() && read(target) == legacy,
		      "a user-managed shader symlink and its target remain untouched");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
