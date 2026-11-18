#include "include/EditorNS/editor.h"
#include "include/editoruicontroller.h"
#include "include/mainwindow.h"
#include "include/notepadqq.h"

#include <QBuffer>
#include <QCloseEvent>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QtTest>

class NotepadqqTest : public QObject {
    Q_OBJECT

public:
    NotepadqqTest();

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void editorPathIsHtml();
    void acceptedCloseDisconnectsEditorUiController();
    void directWindowDeletionDestroysEditorUiControllerFirst();
    void openingFileReplacesUntouchedNewTab();
    void openingFileKeepsTabThatIsStillLoading();
    void readToStringDetectsEncoding_data();
    void readToStringDetectsEncoding();
    void writeFromStringKeepsUtf16WithoutBom_data();
    void writeFromStringKeepsUtf16WithoutBom();

private:
    QTemporaryDir m_settingsDirectory;
};

NotepadqqTest::NotepadqqTest() {}

void NotepadqqTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QCoreApplication::setOrganizationName("Notepadqq-tests");
    QCoreApplication::setApplicationName("Notepadqq-tests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDirectory.path());
}

void NotepadqqTest::cleanupTestCase()
{
    EditorNS::Editor::invalidateEditorBuffer();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

void NotepadqqTest::editorPathIsHtml()
{ QVERIFY(Notepadqq::editorPath().endsWith(".html")); }

// Returns the real editor UI controller owned directly by the window.
static EditorUiController* editorUiController(MainWindow* window)
{
    for (QObject* child : window->children()) {
        if (auto* controller = dynamic_cast<EditorUiController*>(child))
            return controller;
    }
    return nullptr;
}

// Ensures accepted close disconnects container and direct-editor callbacks before deferred deletion.
void NotepadqqTest::acceptedCloseDisconnectsEditorUiController()
{
    auto* window = new MainWindow(QStringList());
    window->setAttribute(Qt::WA_DeleteOnClose, false);

    QPointer<EditorUiController> controller = editorUiController(window);
    QVERIFY(controller);
    EditorNS::Editor* editor = window->currentEditor();
    EditorTabWidget* tabWidget = window->topEditorContainer()->currentTabWidget();
    const int tab = tabWidget->currentIndex();
    QCloseEvent closeEvent;
    QCoreApplication::sendEvent(window, &closeEvent);
    QVERIFY(closeEvent.isAccepted());
    QVERIFY(controller);

    const QString sentinelTitle("closed-window-sentinel");
    window->setWindowTitle(sentinelTitle);
    QVERIFY(QMetaObject::invokeMethod(window->topEditorContainer(),
        "currentEditorChanged",
        Qt::DirectConnection,
        Q_ARG(EditorTabWidget*, tabWidget),
        Q_ARG(int, tab)));
    QCOMPARE(window->windowTitle(), sentinelTitle);

    QVERIFY(QMetaObject::invokeMethod(editor,
        "currentLanguageChanged",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("plaintext")),
        Q_ARG(QString, QStringLiteral("Plain Text"))));
    QCOMPARE(window->windowTitle(), sentinelTitle);

    delete window;
}

// Ensures direct deletion tears down the controller before MainWindow's QObject destruction phase.
void NotepadqqTest::directWindowDeletionDestroysEditorUiControllerFirst()
{
    auto* window = new MainWindow(QStringList());
    EditorUiController* controller = editorUiController(window);
    QVERIFY(controller);

    QStringList destructionOrder;
    connect(controller, &QObject::destroyed, this, [&destructionOrder] { destructionOrder.append("controller"); });
    connect(window, &QObject::destroyed, this, [&destructionOrder] { destructionOrder.append("window"); });

    delete window;

    QCOMPARE(destructionOrder, QStringList({"controller", "window"}));
}

// Creates an empty file in the given directory and returns its URL.
static QUrl createEmptyFile(const QTemporaryDir& directory, const QString& fileName)
{
    QFile file(directory.filePath(fileName));
    if (!file.open(QIODevice::WriteOnly))
        return QUrl();
    return QUrl::fromLocalFile(file.fileName());
}

// Guards the intended behaviour: the untouched "new 1" tab of a fresh window gives way to the opened file.
void NotepadqqTest::openingFileReplacesUntouchedNewTab()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QUrl openedUrl = createEmptyFile(files, "opened.txt");
    QVERIFY(!openedUrl.isEmpty());

    auto* window = new MainWindow(QStringList());
    EditorTabWidget* tabWidget = window->topEditorContainer()->currentTabWidget();
    QCOMPARE(tabWidget->count(), 1);

    window->getDocEngine()->getDocumentLoader().setUrl(openedUrl).setTabWidget(tabWidget).execute().wait();

    QCOMPARE(tabWidget->count(), 1);
    QCOMPARE(tabWidget->editor(0)->filePath(), openedUrl);

    delete window;
}

// Ensures opening a file does not close a tab whose background load has not finished yet.
//
// On startup the session restores tabs with DocEngine::loadDocumentsInBackground(), which marks each editor as
// isLoading until its content has been read. An unsaved tab restored from an empty cache file looks exactly like
// the untouched "new 1" tab meanwhile: no file path and clean. When a file passed on the command line was opened
// in that window, the foreground loader closed the restored tab, and the pending session callback then crashed on
// a tab index of -1 (Editor::tabName() called on a null editor).
void NotepadqqTest::openingFileKeepsTabThatIsStillLoading()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QUrl openedUrl = createEmptyFile(files, "opened.txt");
    QVERIFY(!openedUrl.isEmpty());

    auto* window = new MainWindow(QStringList());
    EditorTabWidget* tabWidget = window->topEditorContainer()->currentTabWidget();
    QCOMPARE(tabWidget->count(), 1);

    // Put the only tab into the state of a restored tab whose background load is still running. Setting the flag
    // directly keeps the test deterministic instead of depending on the timing of a real background load.
    QPointer<EditorNS::Editor> loadingEditor = tabWidget->editor(0);
    QVERIFY(loadingEditor->filePath().isEmpty());
    loadingEditor->isLoading = true;

    window->getDocEngine()->getDocumentLoader().setUrl(openedUrl).setTabWidget(tabWidget).execute().wait();

    QVERIFY(loadingEditor);
    QCOMPARE(tabWidget->count(), 2);
    QCOMPARE(tabWidget->indexOf(loadingEditor), 0);
    QCOMPARE(tabWidget->editor(1)->filePath(), openedUrl);

    loadingEditor->isLoading = false;
    delete window;
}

// Encodes text with the named codec, as an editor saving in that encoding would write it to disk.
static QByteArray encodeAs(const char* codecName, const QString& text)
{ return QTextCodec::codecForName(codecName)->fromUnicode(text); }

static QByteArray encodeWithoutBom(const char* codecName, const QString& text)
{
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
    return QTextCodec::codecForName(codecName)->fromUnicode(text.constData(), text.size(), &state);
}

// Every row is a file content and the text a user expects to see when opening it. The expected codec is only
// checked where it is unambiguous: for single-byte encodings uchardet may pick any codec that decodes the
// sample correctly, e.g. ISO-8859-3 instead of ISO-8859-1 for German text.
void NotepadqqTest::readToStringDetectsEncoding_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<QString>("expectedText");
    QTest::addColumn<QByteArray>("expectedCodec");

    // A long ASCII part pushes the non-ASCII bytes beyond the 64 KiB uchardet used to look at.
    const QString asciiFiller = QStringLiteral("This line is plain ASCII text used as filler.\n").repeated(1600);
    // Source code with a single umlaut in a comment: uchardet took this for ISO-8859-1.
    const QString code = QStringLiteral("int add(int a, int b)\n{\n    return a + b;\n}\n").repeated(3);
    const QString codeWithUmlaut = code + QStringLiteral("// Prüfung\n") + code;

    // UTF-8 that uchardet guessed wrong, shown as "Ã„rger", "StraÃŸe" or "PrÃ¼fung" before.
    QTest::newRow("utf8-capital-umlaut") << QStringLiteral("Ärger\n").toUtf8() << QStringLiteral("Ärger\n")
                                         << QByteArray("UTF-8");
    QTest::newRow("utf8-sharp-s") << QStringLiteral("Straße\n").toUtf8() << QStringLiteral("Straße\n")
                                  << QByteArray("UTF-8");
    QTest::newRow("utf8-single-umlaut-in-code") << codeWithUmlaut.toUtf8() << codeWithUmlaut << QByteArray("UTF-8");

    // Behaviour that must not change.
    QTest::newRow("utf8-sentence") << QStringLiteral("Die Größe der Straße ändert sich.\n").toUtf8()
                                   << QStringLiteral("Die Größe der Straße ändert sich.\n") << QByteArray("UTF-8");
    QTest::newRow("ascii-is-utf8") << QByteArray("Just ASCII.\n") << QStringLiteral("Just ASCII.\n")
                                   << QByteArray("UTF-8");
    QTest::newRow("empty-is-utf8") << QByteArray() << QString() << QByteArray("UTF-8");
    QTest::newRow("latin1-sentence") << encodeAs("ISO-8859-1", QStringLiteral("Die Größe der Straße ändert sich.\n"))
                                     << QStringLiteral("Die Größe der Straße ändert sich.\n") << QByteArray();
    QTest::newRow("windows1251-russian") << encodeAs("windows-1251",
                                                QStringLiteral("Привет, это тестовый текст на русском языке.\n"))
                                         << QStringLiteral("Привет, это тестовый текст на русском языке.\n")
                                         << QByteArray("windows-1251");

    // Latin-1 after a long ASCII part was decoded as UTF-8, turning every umlaut into U+FFFD.
    QTest::newRow("latin1-after-64k") << encodeAs("ISO-8859-1", asciiFiller + QStringLiteral("Grüße\n"))
                                      << asciiFiller + QStringLiteral("Grüße\n") << QByteArray();

    const QString german = QStringLiteral("Die Größe der Straße ändert sich.\n");
    const QString japanese = QStringLiteral("1. これは日本語のテキストです。\n");
    QTest::newRow("utf16le-german") << encodeWithoutBom("UTF-16LE", german) << german << QByteArray("UTF-16LE");
    QTest::newRow("utf16be-german") << encodeWithoutBom("UTF-16BE", german) << german << QByteArray("UTF-16BE");
    QTest::newRow("utf16le-japanese") << encodeWithoutBom("UTF-16LE", japanese) << japanese << QByteArray("UTF-16LE");
    QTest::newRow("ascii-with-nul-is-not-utf16")
        << QByteArray("Hello\0World!", 12) << QString::fromLatin1("Hello\0World!", 12) << QByteArray("UTF-8");
    QTest::newRow("aligned-nul-is-not-utf16")
        << QByteArray("A\0\0\0B\0", 6) << QString::fromLatin1("A\0\0\0B\0", 6) << QByteArray("UTF-8");
}

// Ensures files are decoded with the encoding they were written in when no BOM tells it.
void NotepadqqTest::readToStringDetectsEncoding()
{
    QFETCH(QByteArray, contents);
    QFETCH(QString, expectedText);
    QFETCH(QByteArray, expectedCodec);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath("sample.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), contents.size());
    file.close();

    const DocEngine::DecodedText decoded = DocEngine::readToString(&file);

    QVERIFY(!decoded.error);
    QVERIFY(decoded.codec != nullptr);
    QVERIFY(!decoded.bom);
    QCOMPARE(decoded.text, expectedText);
    if (!expectedCodec.isEmpty()) {
        QCOMPARE(decoded.codec->name(), expectedCodec);
    }
}

void NotepadqqTest::writeFromStringKeepsUtf16WithoutBom_data()
{
    QTest::addColumn<QByteArray>("codecName");
    QTest::addColumn<bool>("bom");

    QTest::newRow("utf16le") << QByteArray("UTF-16LE") << false;
    QTest::newRow("utf16be") << QByteArray("UTF-16BE") << false;
    QTest::newRow("utf16le-bom") << QByteArray("UTF-16LE") << true;
    QTest::newRow("utf16be-bom") << QByteArray("UTF-16BE") << true;
}

// Ensures saving writes a BOM exactly when the document has one, so a file opened without BOM is saved unchanged.
void NotepadqqTest::writeFromStringKeepsUtf16WithoutBom()
{
    QFETCH(QByteArray, codecName);
    QFETCH(bool, bom);

    const QString text = QStringLiteral("Größe\n");
    const QByteArray expected =
        bom ? encodeAs(codecName.constData(), text) : encodeWithoutBom(codecName.constData(), text);

    DocEngine::DecodedText document;
    document.text = text;
    document.codec = QTextCodec::codecForName(codecName);
    document.bom = bom;

    QBuffer buffer;
    QVERIFY(DocEngine::writeFromString(&buffer, document));

    QCOMPARE(buffer.data(), expected);
}

QTEST_MAIN(NotepadqqTest)

#include "tst_notepadqqtest.moc"
