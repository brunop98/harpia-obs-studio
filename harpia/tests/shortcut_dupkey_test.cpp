// The panel refuses to assign a key that another command already owns. Nothing
// else does: importing a profile, or a hand-edited settings file, can land the
// same key on two commands. Qt calls that an ambiguous shortcut and fires
// NEITHER -- so two commands go dead and the panel shows both as bound.
#include "editor/ShortcutRegistry.hpp"
#include <QApplication>
#include <QTest>
#include <QSettings>
#include <QSpinBox>
#include <QWidget>
#include <cstdio>
using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w) { std::printf("  %s %s\n", c?"PASS":"FAIL", w); if(!c) ++failures; }

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    // ShortcutRegistry reads and writes QSettings("Harpia", "Recorder")
    // explicitly, NOT the application's own scope -- so that is the group to
    // clear, or the test reads whatever a previous run left behind.
    { QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
      st.remove(QStringLiteral("editorShortcuts")); }
    QWidget host; host.resize(200,100); host.show();
    host.activateWindow();
    host.setFocus();
    QApplication::processEvents();

    int firedA = 0, firedB = 0;
    ShortcutRegistry reg(&host);
    ShortcutCommand a; a.id="cmd.a"; a.label="A"; a.category="T";
    a.defaults = {QKeySequence(QStringLiteral("J"))};
    a.run = [&]{ ++firedA; };
    ShortcutCommand b; b.id="cmd.b"; b.label="B"; b.category="T";
    b.defaults = {QKeySequence(QStringLiteral("K"))};
    b.run = [&]{ ++firedB; };
    reg.addCommand(a); reg.addCommand(b);
    // load() is what builds the live QShortcut objects; addCommand only records
    // the command. (setBindings would no-op here -- the values already ARE the
    // defaults -- so it never triggers a rebuild.)
    reg.load();
    QApplication::processEvents();

    host.setFocus();
    QTest::keyClick(&host, Qt::Key_J); QApplication::processEvents();
    host.setFocus();
    QTest::keyClick(&host, Qt::Key_K); QApplication::processEvents();
    std::printf("     bindings: A=\"%s\" B=\"%s\"\n", qPrintable(reg.displayText("cmd.a")), qPrintable(reg.displayText("cmd.b")));
    std::printf("     baseline: A=%d B=%d\n", firedA, firedB);
    ok(firedA == 1 && firedB == 1, "each command fires on its own key");

    // Now import a profile that gives BOTH commands Ctrl+1 -- something the
    // panel would never let you do, but a file can say.
    const QByteArray profile = R"({
      "format": "harpia-shortcuts", "version": 1,
      "bindings": { "cmd.a": ["J"], "cmd.b": ["J"] }
    })";
    QString err;
    ok(reg.importProfile(profile, &err), "the profile imports");
    QApplication::processEvents();

    firedA = firedB = 0;
    host.setFocus();
    QTest::keyClick(&host, Qt::Key_J);
    QApplication::processEvents();
    std::printf("     after importing a duplicate: A=%d B=%d\n", firedA, firedB);
    // Exactly one command should act. Zero means Qt dropped the ambiguous
    // shortcut and BOTH commands are dead.
    const int total = firedA + firedB;
    ok(total == 1, "exactly one command runs (0 = both went dead, 2 = both ran)");

    // And the second command must not still be advertising a key it cannot use.
    std::printf("     registry says: A=\"%s\"  B=\"%s\"\n",
                qPrintable(reg.displayText("cmd.a")), qPrintable(reg.displayText("cmd.b")));
    ok(reg.conflict(QKeySequence("J"), "cmd.a").isEmpty(),
       "no command other than A still claims J");

    // A command with a `when` gate. While the gate is closed its key must not
    // merely do nothing -- it must not be TAKEN, so a spin box that has focus
    // still gets its Up. That is the whole point of the gate: Multi-Cut's
    // Up / Down would otherwise steal the arrows from every other mode.
    std::printf("\n-- a gated command lets its key through while the gate is closed --\n");
    bool gate = false;
    int firedUp = 0;
    ShortcutCommand up; up.id="cmd.up"; up.label="Up"; up.category="T";
    up.defaults = {QKeySequence(Qt::Key_Up)};
    up.run = [&]{ ++firedUp; };
    up.when = [&]{ return gate; };
    reg.addCommand(up);
    reg.load(); // rebuilds the live shortcuts (setBindings would no-op on defaults)
    QApplication::processEvents();

    QSpinBox spin(&host);
    spin.setRange(0, 100);
    spin.setValue(10);
    spin.show();
    spin.setFocus();
    QApplication::processEvents();
    QTest::keyClick(&spin, Qt::Key_Up); QApplication::processEvents();
    std::printf("     gate closed: fired=%d spin=%d\n", firedUp, spin.value());
    ok(firedUp == 0, "the gated command does not run while its gate is closed");
    ok(spin.value() == 11, "and the spin box still got its Up");

    gate = true;
    reg.refreshEnabled();
    spin.setFocus();
    QTest::keyClick(&spin, Qt::Key_Up); QApplication::processEvents();
    std::printf("     gate open: fired=%d spin=%d\n", firedUp, spin.value());
    ok(firedUp == 1, "with the gate open the command runs");
    ok(spin.value() == 11, "and the key no longer reaches the spin box");

    std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
    return failures ? 1 : 0;
}
