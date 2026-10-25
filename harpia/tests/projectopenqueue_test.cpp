#include "ui/ProjectOpenQueue.hpp"
#include <QApplication>
#include <QDialog>
#include <QFileOpenEvent>
#include <QTimer>
#include <QtTest/QTest>
#include <cstdio>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	int failed = 0;
	const auto check = [&](bool yes, const char *text) {
		std::printf("  %s %s\n", yes ? "PASS" : "FAIL", text);
		failed += !yes;
	};
	harpia::ProjectOpenQueue queue;
	QFileOpenEvent early(QStringLiteral("early.harpiaproj"));
	QApplication::sendEvent(&app, &early);
	QStringList opened;
	QObject receiver;
	queue.setReceiver(&receiver, [&](const QString &path) { opened << QFileInfo(path).fileName(); });
	QTest::qWait(20);
	check(opened == QStringList{QStringLiteral("early.harpiaproj")}, "Finder documents received before startup are opened when the receiver is ready");
	check(!queue.enqueue(QStringLiteral("movie.mp4")), "unassociated media is not mistaken for a project");
	QDialog busy;
	busy.setWindowModality(Qt::ApplicationModal);
	busy.show();
	queue.enqueue(QStringLiteral("later.harpiaproj"));
	queue.enqueue(QStringLiteral("later.harpiaproj"));
	QTest::qWait(30);
	check(opened.size() == 1, "a loading dialog or modal editor cannot be reentered by a document event");
	busy.hide();
	QTest::qWait(120);
	check(opened.size() == 2 && opened.back() == QStringLiteral("later.harpiaproj"), "a pending project opens once after the modal operation finishes");
	auto *temporary = new QObject;
	queue.setReceiver(temporary, [&](const QString &) { ++failed; });
	queue.enqueue(QStringLiteral("destroyed.harpiaproj"));
	delete temporary;
	QTest::qWait(20);
	check(failed == 0, "destroying the receiver cancels pending document callbacks");
	return failed ? 1 : 0;
}
