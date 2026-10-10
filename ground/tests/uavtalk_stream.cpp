/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <uavtalk/uavtalk.h>
#include <attitudeactual.h>
#include <waypoint.h>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <cstring>
#include <memory>

namespace {
constexpr quint32 attitudeId = 0x33dad5e6;
constexpr quint32 waypointId = 0xe02c8266;

// Encode board-side bytes independently of UAVObject::pack and UAVTalk's CRC table.
void appendLittleEndian(QByteArray &bytes, quint32 value, int count)
{
    for (int byte = 0; byte < count; ++byte)
        bytes.append(char(value >> (8 * byte)));
}

void appendFloat(QByteArray &bytes, float value)
{
    quint32 bits;
    static_assert(sizeof(bits) == sizeof(value), "32-bit wire floats required");
    std::memcpy(&bits, &value, sizeof(bits));
    appendLittleEndian(bytes, bits, 4);
}

quint8 checksum(const QByteArray &bytes)
{
    quint8 crc = 0;
    for (char byte : bytes) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? quint8((crc << 1) ^ 0x07) : quint8(crc << 1);
    }
    return crc;
}

QByteArray frame(quint8 type, quint32 objectId, const QByteArray &payload = {})
{
    QByteArray bytes;
    bytes.append(char(0x3c));
    bytes.append(char(type));
    appendLittleEndian(bytes, 8 + payload.size(), 2);
    appendLittleEndian(bytes, objectId, 4);
    bytes.append(payload);
    bytes.append(char(checksum(bytes)));
    return bytes;
}

QByteArray attitudePayload(float roll, float pitch = -12.5f, float yaw = 90.0f)
{
    QByteArray bytes;
    for (float value : {1.0f, 0.0f, 0.0f, 0.0f, roll, pitch, yaw})
        appendFloat(bytes, value);
    return bytes;
}

QByteArray instancePayload(quint16 instance, const QByteArray &payload = {})
{
    QByteArray bytes;
    appendLittleEndian(bytes, instance, 2);
    bytes.append(payload);
    return bytes;
}

QByteArray waypointPayload(float north)
{
    QByteArray bytes;
    for (float value : {north, 2.0f, -3.0f, 4.0f, 5.0f})
        appendFloat(bytes, value);
    bytes.append(char(1));
    return bytes;
}

// A real TCP stream provides both the input notifications and asynchronous writes.
class SocketLink
{
    QTcpServer server;
public:
    QTcpSocket board;
    std::unique_ptr<QTcpSocket> gcs;
    std::unique_ptr<UAVTalk> protocol;

    bool open(UAVObjectManager *objects)
    {
        if (!server.listen(QHostAddress::LocalHost))
            return false;
        board.connectToHost(QHostAddress::LocalHost, server.serverPort());
        if (!board.waitForConnected(2000))
            return false;
        if (!server.hasPendingConnections() && !server.waitForNewConnection(2000))
            return false;
        gcs.reset(server.nextPendingConnection());
        if (!gcs)
            return false;
        board.setSocketOption(QAbstractSocket::LowDelayOption, 1);
        gcs->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        protocol = std::make_unique<UAVTalk>(gcs.get(), objects);
        return true;
    }

    bool send(const QByteArray &bytes)
    {
        QSignalSpy arrival(gcs.get(), &QIODevice::readyRead);
        if (board.write(bytes) != bytes.size())
            return false;
        board.flush();
        return !arrival.isEmpty() || arrival.wait(2000);
    }

    QByteArray read(int expectedSize)
    {
        gcs->flush();
        QByteArray bytes;
        QElapsedTimer timer;
        timer.start();
        while (bytes.size() < expectedSize && timer.elapsed() < 2000) {
            bytes.append(board.readAll());
            if (bytes.size() < expectedSize)
                board.waitForReadyRead(qMax(1, 2000 - int(timer.elapsed())));
        }
        return bytes;
    }
};
}

class UAVTalkStreamTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<UAVObjectManager> objects;
    AttitudeActual *attitude = nullptr;
    Waypoint *waypoint = nullptr;

private slots:
    void initTestCase()
    {
        QCOMPARE(checksum(QByteArray("123456789")), quint8(0xf4));
        QCOMPARE(AttitudeActual::OBJID, attitudeId);
        QCOMPARE(Waypoint::OBJID, waypointId);
        qRegisterMetaType<UAVObject *>("UAVObject*");
    }

    void init()
    {
        objects = std::make_unique<UAVObjectManager>();
        auto own = [this](UAVObject *object) { object->setParent(objects.get()); };
        connect(objects.get(), &UAVObjectManager::newObject, objects.get(), own);
        connect(objects.get(), &UAVObjectManager::newInstance, objects.get(), own);
        attitude = new AttitudeActual;
        waypoint = new Waypoint;
        QVERIFY(objects->registerObject(attitude));
        QVERIFY(objects->registerObject(waypoint));
    }

    void fragmentedObject_data()
    {
        QTest::addColumn<int>("chunkSize");
        for (int size : {1, 2, 7, 8, 9, 17, 36})
            QTest::newRow(qPrintable(QString::number(size))) << size;
    }

    void fragmentedObject()
    {
        QFETCH(int, chunkSize);
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QSignalSpy updates(attitude, &UAVObject::objectUnpacked);
        const QByteArray packet = frame(0x20, attitudeId, attitudePayload(42.25f));
        quint32 received = 0;
        for (int offset = 0; offset < packet.size(); offset += chunkSize) {
            const QByteArray chunk = packet.mid(offset, chunkSize);
            QVERIFY(link.send(chunk));
            const auto stats = link.protocol->getStats();
            received += stats.rxBytes;
            QCOMPARE(stats.rxErrors, quint32(0));
            QCOMPARE(updates.count(), offset + chunk.size() == packet.size() ? 1 : 0);
        }
        QCOMPARE(received, quint32(packet.size()));
        QCOMPARE(attitude->getRoll(), 42.25f);
        QCOMPARE(attitude->getPitch(), -12.5f);
        QCOMPARE(attitude->getYaw(), 90.0f);
    }

    void burstExceedsReceiveBuffer()
    {
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QSignalSpy updates(attitude, &UAVObject::objectUnpacked);
        QByteArray burst;
        for (int index = 0; index < 256; ++index)
            burst.append(frame(0x20, attitudeId, attitudePayload(float(index))));
        QVERIFY(burst.size() > 3 * 1024);
        QVERIFY(link.send(burst));
        QTRY_COMPARE(updates.count(), 256);
        QCOMPARE(attitude->getRoll(), 255.0f);
        const auto stats = link.protocol->getStats();
        QCOMPARE(stats.rxBytes, quint32(burst.size()));
        QCOMPARE(stats.rxObjects, quint32(256));
        QCOMPARE(stats.rxErrors, quint32(0));
    }

    void invalidFrameRecovery_data()
    {
        QTest::addColumn<QByteArray>("invalid");
        const QByteArray valid = frame(0x20, attitudeId, attitudePayload(10.0f));
        QByteArray badCrc = valid;
        badCrc[badCrc.size() - 1] = char(quint8(badCrc.back()) ^ 1);
        QTest::newRow("crc") << badCrc;
        QTest::newRow("garbage") << QByteArray("garbage before sync");
        QTest::newRow("version") << frame(0x10, attitudeId, attitudePayload(10.0f));
        QTest::newRow("payload-length") << frame(0x20, attitudeId, QByteArray(27, char(0)));
        QTest::newRow("unsupported-type") << frame(0x25, attitudeId, attitudePayload(10.0f));
        QByteArray oversized = valid.left(valid.size() - 1);
        oversized[3] = char(1); // A 16-bit length greater than the supported packet limit.
        oversized.append(char(checksum(oversized)));
        QTest::newRow("oversized-length") << oversized;
    }

    void invalidFrameRecovery()
    {
        QFETCH(QByteArray, invalid);
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QSignalSpy updates(attitude, &UAVObject::objectUnpacked);
        const QByteArray valid = frame(0x20, attitudeId, attitudePayload(23.0f));
        QVERIFY(link.send(invalid + valid));
        QTRY_COMPARE(updates.count(), 1);
        QCOMPARE(attitude->getRoll(), 23.0f);
        const auto stats = link.protocol->getStats();
        QCOMPARE(stats.rxBytes, quint32(invalid.size() + valid.size()));
        QVERIFY(stats.rxErrors > 0);
        QCOMPARE(stats.rxObjects, quint32(1));
    }

    void malformedFileFrameDoesNotStallFollowingObject()
    {
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QSignalSpy updates(attitude, &UAVObject::objectUnpacked);
        const QByteArray invalid = frame(0x29, 0x12345678, QByteArray(4, char(0)));
        const QByteArray valid = frame(0x20, attitudeId, attitudePayload(31.0f));
        QVERIFY(link.send(invalid + valid));
        QTRY_COMPARE_WITH_TIMEOUT(updates.count(), 1, 750);
        QCOMPARE(attitude->getRoll(), 31.0f);
        const auto stats = link.protocol->getStats();
        QCOMPARE(stats.rxErrors, quint32(1));
        QCOMPARE(stats.rxObjects, quint32(1));
    }

    void outboundWireBytes()
    {
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        AttitudeActual::DataFields data{};
        data.q1 = 1.0f;
        data.Roll = 42.25f;
        data.Pitch = -12.5f;
        data.Yaw = 90.0f;
        attitude->setData(data);
        for (bool acked : {false, true}) {
            const QByteArray expected = frame(acked ? 0x22 : 0x20, attitudeId,
                                              attitudePayload(42.25f));
            QVERIFY(link.protocol->sendObject(attitude, acked, false));
            QCOMPARE(link.read(expected.size()), expected);
        }
        QVERIFY(link.protocol->sendObjectRequest(attitude, true));
        const QByteArray request = frame(0x21, attitudeId);
        QCOMPARE(link.read(request.size()), request);
        const auto stats = link.protocol->getStats();
        QCOMPARE(stats.txObjects, quint32(3));
        QCOMPARE(stats.txErrors, quint32(0));
    }

    void requestsAndAcknowledgements()
    {
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QSignalSpy acks(link.protocol.get(), &UAVTalk::ackReceived);
        QSignalSpy nacks(link.protocol.get(), &UAVTalk::nackReceived);
        QVERIFY(link.send(frame(0x22, attitudeId, attitudePayload(19.0f))));
        QCOMPARE(link.read(9), frame(0x23, attitudeId));
        QVERIFY(link.send(frame(0x21, attitudeId)));
        const QByteArray response = frame(0x20, attitudeId, attitudePayload(19.0f));
        QCOMPARE(link.read(response.size()), response);
        QVERIFY(link.send(frame(0x23, attitudeId) + frame(0x24, attitudeId)));
        QTRY_COMPARE(acks.count(), 1);
        QTRY_COMPARE(nacks.count(), 1);
        QCOMPARE(qvariant_cast<UAVObject *>(acks.first().first()), attitude);
        QCOMPARE(qvariant_cast<UAVObject *>(nacks.first().first()), attitude);
        QVERIFY(link.send(frame(0x21, 0xdeadbeee)));
        QCOMPARE(link.read(9), frame(0x24, 0xdeadbeee));
        const auto stats = link.protocol->getStats();
        QCOMPARE(stats.rxErrors, quint32(1));
        QCOMPARE(stats.rxObjects, quint32(4));
    }

    void multiInstanceObjectsAndRequests()
    {
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        for (quint16 instance : {quint16(0), quint16(2), quint16(1)}) {
            QVERIFY(link.send(frame(0x22, waypointId,
                                    instancePayload(instance, waypointPayload(float(instance + 10))))));
            QCOMPARE(link.read(11), frame(0x23, waypointId, instancePayload(instance)));
        }
        QCOMPARE(objects->getNumInstances(waypointId), 3);
        for (quint16 instance = 0; instance < 3; ++instance) {
            auto *object = Waypoint::GetInstance(objects.get(), instance);
            QVERIFY(object);
            QCOMPARE(object->getPosition_North(), float(instance + 10));
            QCOMPARE(object->getMode(), quint8(1));
        }
        const QByteArray request = frame(0x21, waypointId, instancePayload(0xffff));
        QVERIFY(link.protocol->sendObjectRequest(waypoint, true));
        QCOMPARE(link.read(request.size()), request);
        QVERIFY(link.send(request));
        QByteArray expected;
        for (quint16 instance = 0; instance < 3; ++instance)
            expected.append(frame(0x20, waypointId,
                                  instancePayload(instance, waypointPayload(float(instance + 10)))));
        QCOMPARE(link.read(expected.size()), expected);
        QSignalSpy acks(link.protocol.get(), &UAVTalk::ackReceived);
        QSignalSpy nacks(link.protocol.get(), &UAVTalk::nackReceived);
        QVERIFY(link.send(frame(0x23, waypointId, instancePayload(2))));
        QCOMPARE(qvariant_cast<UAVObject *>(acks.first().first()),
                 Waypoint::GetInstance(objects.get(), 2));
        QVERIFY(link.send(frame(0x24, waypointId)));
        QCOMPARE(qvariant_cast<UAVObject *>(nacks.first().first()), waypoint);
        QVERIFY(link.send(frame(0x24, waypointId, instancePayload(1))));
        QCOMPARE(qvariant_cast<UAVObject *>(nacks.last().first()),
                 Waypoint::GetInstance(objects.get(), 1));
        QVERIFY(link.send(frame(0x24, waypointId, instancePayload(0xffff))));
        QCOMPARE(qvariant_cast<UAVObject *>(nacks.last().first()), waypoint);
        QCOMPARE(link.protocol->getStats().rxErrors, quint32(0));
    }

    void missingInstanceBytes_data()
    {
        QTest::addColumn<quint8>("type");
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("request-no-instance") << quint8(0x21) << QByteArray();
        QTest::newRow("request-one-byte") << quint8(0x21) << QByteArray(1, char(0));
        QTest::newRow("ack-no-instance") << quint8(0x23) << QByteArray();
        QTest::newRow("ack-one-byte") << quint8(0x23) << QByteArray(1, char(0));
        QTest::newRow("nack-one-byte") << quint8(0x24) << QByteArray(1, char(0));
    }

    void missingInstanceBytes()
    {
        QFETCH(quint8, type);
        QFETCH(QByteArray, payload);
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QSignalSpy acks(link.protocol.get(), &UAVTalk::ackReceived);
        QSignalSpy nacks(link.protocol.get(), &UAVTalk::nackReceived);
        QVERIFY(link.send(frame(type, waypointId, payload)
                          + frame(0x20, attitudeId, attitudePayload(13.0f))));
        QCOMPARE(attitude->getRoll(), 13.0f);
        QVERIFY(acks.isEmpty());
        QVERIFY(nacks.isEmpty());
        QCOMPARE(link.board.bytesAvailable(), qint64(0));
        const auto stats = link.protocol->getStats();
        QCOMPARE(stats.rxErrors, quint32(1));
        QCOMPARE(stats.rxObjects, quint32(1));
    }

    void rejectedInstanceCountsAsReceiveError_data()
    {
        QTest::addColumn<quint8>("type");
        QTest::addColumn<quint16>("instance");
        QTest::addColumn<bool>("expectNack");
        QTest::newRow("object-out-of-range") << quint8(0x20) << quint16(1000) << false;
        QTest::newRow("acked-object-out-of-range") << quint8(0x22) << quint16(1000) << true;
        QTest::newRow("ack-unknown-instance") << quint8(0x23) << quint16(1000) << false;
        QTest::newRow("object-all-instances") << quint8(0x20) << quint16(0xffff) << false;
        QTest::newRow("acked-object-all-instances") << quint8(0x22) << quint16(0xffff) << false;
        QTest::newRow("ack-all-instances") << quint8(0x23) << quint16(0xffff) << false;
    }

    void rejectedInstanceCountsAsReceiveError()
    {
        QFETCH(quint8, type);
        QFETCH(quint16, instance);
        QFETCH(bool, expectNack);
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QSignalSpy acks(link.protocol.get(), &UAVTalk::ackReceived);
        const QByteArray data = type == 0x23 ? QByteArray() : waypointPayload(5.0f);
        QVERIFY(link.send(frame(type, waypointId, instancePayload(instance, data))));
        if (expectNack)
            QCOMPARE(link.read(9), frame(0x24, waypointId));
        else
            QCOMPARE(link.board.bytesAvailable(), qint64(0));
        QVERIFY(!objects->getObject(waypointId, instance));
        QVERIFY(acks.isEmpty());
        const auto stats = link.protocol->getStats();
        QCOMPARE(stats.rxErrors, quint32(1));
        QCOMPARE(stats.rxObjects, quint32(0));
    }

    void fileDataAndRequest()
    {
        SocketLink link;
        QVERIFY(link.open(objects.get()));
        QByteArray received;
        quint32 receivedId = 0, offset = 0;
        bool eof = false, last = false;
        connect(link.protocol.get(), &UAVTalk::fileDataReceived, this,
                [&](quint32 id, quint32 position, quint8 *data, quint32 size, bool atEof, bool atLast) {
            receivedId = id;
            offset = position;
            received = QByteArray(reinterpret_cast<const char *>(data), int(size));
            eof = atEof;
            last = atLast;
        });
        QByteArray payload;
        appendLittleEndian(payload, 0x01020304, 4);
        payload.append(char(3));
        const QByteArray contents(242, char(0xa5));
        payload.append(contents);
        const QByteArray packet = frame(0x29, 0x87654321, payload);
        QCOMPARE(packet.size(), 256);
        QVERIFY(link.send(packet));
        QCOMPARE(received, contents);
        QCOMPARE(receivedId, quint32(0x87654321));
        QCOMPARE(offset, quint32(0x01020304));
        QVERIFY(eof && last);
        QVERIFY(link.protocol->requestFile(0x87654321, 0x01020304));
        QByteArray request;
        appendLittleEndian(request, 0x01020304, 4);
        request.append(QByteArray(2, char(0)));
        const QByteArray expected = frame(0x28, 0x87654321, request);
        QCOMPARE(link.read(expected.size()), expected);
        QCOMPARE(link.protocol->getStats().rxErrors, quint32(0));
    }

    void disconnectDuringFrameAndReconnect()
    {
        QSignalSpy updates(attitude, &UAVObject::objectUnpacked);
        const QByteArray packet = frame(0x20, attitudeId, attitudePayload(77.0f));
        {
            SocketLink link;
            QVERIFY(link.open(objects.get()));
            QVERIFY(link.send(packet.left(20)));
            link.board.abort();
            QTRY_COMPARE(link.gcs->state(), QAbstractSocket::UnconnectedState);
            QVERIFY(updates.isEmpty());
            link.gcs.reset();
            QVERIFY(!link.protocol->sendObject(attitude, false, false));
            QVERIFY(!link.protocol->sendObject(waypoint, false, true));
            const auto stats = link.protocol->getStats();
            QCOMPARE(stats.txObjects, quint32(0));
            QVERIFY(stats.txErrors > 0);
        }
        SocketLink replacement;
        QVERIFY(replacement.open(objects.get()));
        QVERIFY(replacement.send(packet));
        QCOMPARE(updates.count(), 1);
        QCOMPARE(attitude->getRoll(), 77.0f);
        QCOMPARE(replacement.protocol->getStats().rxErrors, quint32(0));
    }
};

QTEST_GUILESS_MAIN(UAVTalkStreamTests)
#include "uavtalk_stream.moc"
