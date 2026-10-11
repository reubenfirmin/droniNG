#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <quazip.h>
#include <quazipfile.h>

class ArchiveCompatibilityTests : public QObject
{
    Q_OBJECT
    const QByteArray payload = QByteArray("binary\0settings\xff", 16)
            + QString::fromUtf8("Información de vuelo 日本語").toUtf8().repeated(100);

    void writeArchive(const QString &filename, const char *codec, const QString &entryName)
    {
        QuaZip zip(filename);
        zip.setFileNameCodec(codec);
        zip.setCommentCodec(codec);
        zip.setComment(QString::fromUtf8("Información"));
        QVERIFY(zip.open(QuaZip::mdCreate));
        QuaZipFile entry(&zip);
        QuaZipNewInfo info(entryName);
        info.comment = QString::fromUtf8("Configuración");
        info.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup);
        QVERIFY(entry.open(QIODevice::WriteOnly, info));
        QCOMPARE(entry.write(payload), qint64(payload.size()));
        entry.close();
        QCOMPARE(entry.getZipError(), ZIP_OK);
        zip.close();
        QCOMPARE(zip.getZipError(), ZIP_OK);
    }

    void readArchive(const QString &filename, const char *codec, const QString &entryName)
    {
        QuaZip zip(filename);
        zip.setFileNameCodec(codec);
        zip.setCommentCodec(codec);
        QVERIFY(zip.open(QuaZip::mdUnzip));
        QCOMPARE(zip.getFileNameList(), QStringList{entryName});
        QCOMPARE(zip.getComment(), QString::fromUtf8("Información"));
        QVERIFY(zip.setCurrentFile(entryName));
        QuaZipFileInfo64 info;
        QVERIFY(zip.getCurrentFileInfo(&info));
        QCOMPARE(info.name, entryName);
        QCOMPARE(info.comment, QString::fromUtf8("Configuración"));
        QCOMPARE(info.getPermissions(), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup);
        QCOMPARE(info.uncompressedSize, quint64(payload.size()));
        QVERIFY(info.compressedSize < info.uncompressedSize);
        QuaZipFile entry(&zip);
        QVERIFY(entry.open(QIODevice::ReadOnly));
        QCOMPARE(entry.readAll(), payload);
        entry.close();
        QCOMPARE(entry.getZipError(), UNZ_OK);
        zip.close();
        QCOMPARE(zip.getZipError(), UNZ_OK);
    }

private slots:
    void roundTrip_data()
    {
        QTest::addColumn<QByteArray>("codec");
        QTest::addColumn<QString>("entryName");
        QTest::newRow("utf8") << QByteArray("UTF-8")
                             << QString::fromUtf8("vuelo/café-日本語.xml");
        QTest::newRow("legacy-codepage") << QByteArray("Windows-1252")
                                        << QString::fromUtf8("vuelo/café.xml");
    }

    void roundTrip()
    {
        QFETCH(QByteArray, codec);
        QFETCH(QString, entryName);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filename = temporary.filePath("settings.zip");
        writeArchive(filename, codec.constData(), entryName);
        readArchive(filename, codec.constData(), entryName);
        const QString referenceDirectory = qEnvironmentVariable("GCS_ZIP_REFERENCE_DIR");
        QVERIFY(!referenceDirectory.isEmpty());
        const QString referenceFile = referenceDirectory + "/" + QString::fromLatin1(codec) + ".zip";
        // Read the preserved archive bytes from the previous release.
        readArchive(referenceFile, codec.constData(), entryName);
    }

    void malformedArchive()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile file(temporary.filePath("invalid.zip"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("PK\x03\x04truncated") > 0);
        file.close();
        QuaZip zip(file.fileName());
        QVERIFY(!zip.open(QuaZip::mdUnzip));
        QVERIFY(zip.getZipError() != UNZ_OK);
    }

    void corruptChecksum()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filename = temporary.filePath("corrupt.zip");
        const QString entryName = QStringLiteral("settings.xml");
        writeArchive(filename, "UTF-8", entryName);
        QFile file(filename);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray bytes = file.readAll();
        file.close();
        const int directory = bytes.indexOf(QByteArray("PK\x01\x02", 4));
        QVERIFY(directory >= 0 && directory + 20 <= bytes.size());
        // Keep compressed data intact but change the checksum advertised by
        // the central directory. Reading succeeds; close must reject the CRC.
        char *checksum = bytes.data() + directory + 16;
        qToLittleEndian(qFromLittleEndian<quint32>(checksum) ^ quint32(1), checksum);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        QuaZip zip(filename);
        QVERIFY(zip.open(QuaZip::mdUnzip));
        QVERIFY(zip.setCurrentFile(entryName));
        QuaZipFile entry(&zip);
        QVERIFY(entry.open(QIODevice::ReadOnly));
        QCOMPARE(entry.readAll(), payload);
        entry.close();
        QCOMPARE(entry.getZipError(), UNZ_CRCERROR);
        zip.close();
    }
};

QTEST_GUILESS_MAIN(ArchiveCompatibilityTests)
#include "archive_compat.moc"
