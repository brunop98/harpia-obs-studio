// The recorder window's Developer Panel: Copy JSON / Paste JSON.
//
// Every size is in the table (so every one is copied), the copy reads back to
// exactly the same values, and a paste applies only what it names, ignores
// what it does not know, and says so when it is not JSON at all.
#include "ui/MainLayoutParams.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::printf("\n-- recorder window layout as JSON --\n");

	// Every int in the struct is in the table: sizeof counts them.
	ok(mainLayoutFields().size() == int(sizeof(MainLayoutParams) / sizeof(int)),
	   "every size in MainLayoutParams is in the table");

	MainLayoutParams p;
	int k = 1;
	for (const MainLayoutField &f : mainLayoutFields())
		p.*(f.member) = 100 + k++;
	const QByteArray json = mainLayoutToJson(p);
	const QJsonObject root = QJsonDocument::fromJson(json).object();
	ok(root.value(QStringLiteral("harpiaDevLayout")).toInt() == 1 &&
		   root.value(QStringLiteral("values")).toObject().size() == mainLayoutFields().size(),
	   "Copy JSON writes every value, in the editor panel's shape");
	ok(root.value(QStringLiteral("values")).toObject().value(QStringLiteral("webcamPreviewW")).toInt() ==
		   p.webcamPreviewW,
	   "under the names the settings use");

	MainLayoutParams back;
	ok(mainLayoutFromJson(json, back) == mainLayoutFields().size(), "Paste JSON applies them all");
	bool same = true;
	for (const MainLayoutField &f : mainLayoutFields())
		same = same && back.*(f.member) == p.*(f.member);
	ok(same, "and gets back exactly what was copied");

	MainLayoutParams partial;
	const int before = partial.recordBtnH;
	ok(mainLayoutFromJson(R"({"recordBtnW": 222, "someoneElses": 5})", partial) == 1 &&
		   partial.recordBtnW == 222 && partial.recordBtnH == before,
	   "a bare object: only the named size changes, unknown names are ignored");
	QString err;
	MainLayoutParams untouched;
	ok(mainLayoutFromJson("not json", untouched, &err) == 0 && !err.isEmpty() &&
		   untouched.recordBtnW == MainLayoutParams().recordBtnW,
	   "not JSON: nothing changes, and it says why");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
