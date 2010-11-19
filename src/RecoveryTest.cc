/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <cstring>

#include "TestUtil.h"
#include "BindTransport.h"
#include "BackupManager.h"
#include "BackupServer.h"
#include "BackupStorage.h"
#include "CoordinatorClient.h"
#include "CoordinatorServer.h"
#include "Logging.h"
#include "MasterServer.h"
#include "Tablets.pb.h"
#include "TransportManager.h"
#include "Recovery.h"

namespace RAMCloud {

/**
 * Unit tests for Recovery.
 */
class RecoveryTest : public CppUnit::TestFixture {

    CPPUNIT_TEST_SUITE(RecoveryTest);
    CPPUNIT_TEST(test_buildSegmentIdToBackups);
    CPPUNIT_TEST(test_createBackupList);
    CPPUNIT_TEST(test_start);
    CPPUNIT_TEST(test_start_notEnoughMasters);
    CPPUNIT_TEST_SUITE_END();

    /**
     * Used to control precise timing of destruction of the Segment object
     * which implicitly calls freeSegment.
     */
    struct WriteValidSegment {
        ProtoBuf::ServerList backupList;
        BackupManager* mgr;
        char *segMem;
        Segment* seg;

        WriteValidSegment(uint64_t masterId, uint64_t segmentId,
                          const uint32_t segmentSize, const string& locator)
            : backupList()
            , mgr()
            , segMem()
            , seg()
        {
            mgr = new BackupManager(NULL, 1);
            ProtoBuf::ServerList::Entry& e(*backupList.add_server());
            e.set_service_locator(locator);
            e.set_server_type(ProtoBuf::BACKUP);
            mgr->setHostList(backupList);

            segMem = new char[segmentSize];
            seg = new Segment(masterId, segmentId, segMem, segmentSize, mgr);
            seg->close();
        }

        ~WriteValidSegment()
        {
            delete seg;
            delete[] segMem;
            delete mgr;
        }

        DISALLOW_COPY_AND_ASSIGN(WriteValidSegment);
    };

    BackupClient* backup1;
    BackupClient* backup2;
    BackupClient* backup3;
    BackupServer* backupServer1;
    BackupServer* backupServer2;
    BackupServer* backupServer3;
    CoordinatorClient* coordinator;
    CoordinatorServer* coordinatorServer;
    BackupServer::Config* config;
    ProtoBuf::ServerList* masterHosts;
    ProtoBuf::ServerList* backupHosts;
    const uint32_t segmentFrames;
    const uint32_t segmentSize;
    vector<WriteValidSegment*> segmentsToFree;
    BackupStorage* storage1;
    BackupStorage* storage2;
    BackupStorage* storage3;
    BindTransport* transport;

  public:
    RecoveryTest()
        : backup1()
        , backup2()
        , backup3()
        , backupServer1()
        , backupServer2()
        , backupServer3()
        , coordinator()
        , coordinatorServer()
        , config()
        , masterHosts()
        , backupHosts()
        , segmentFrames(2)
        , segmentSize(1 << 16)
        , segmentsToFree()
        , storage1()
        , storage2()
        , storage3()
        , transport()
    {
    }

    void
    setUp(bool enlist)
    {
        logger.setLogLevels(SILENT_LOG_LEVEL);
        if (!enlist)
            tearDown();

        transport = new BindTransport;
        transportManager.registerMock(transport);

        config = new BackupServer::Config;
        config->coordinatorLocator = "mock:host=coordinator";

        coordinatorServer = new CoordinatorServer;
        transport->addServer(*coordinatorServer, config->coordinatorLocator);

        coordinator = new CoordinatorClient(config->coordinatorLocator.c_str());

        storage1 = new InMemoryStorage(segmentSize, segmentFrames);
        storage2 = new InMemoryStorage(segmentSize, segmentFrames);
        storage3 = new InMemoryStorage(segmentSize, segmentFrames);

        backupServer1 = new BackupServer(*config, *storage1);
        backupServer2 = new BackupServer(*config, *storage2);
        backupServer3 = new BackupServer(*config, *storage3);

        transport->addServer(*backupServer1, "mock:host=backup1");
        transport->addServer(*backupServer2, "mock:host=backup2");
        transport->addServer(*backupServer3, "mock:host=backup3");

        if (enlist) {
            coordinator->enlistServer(BACKUP, "mock:host=backup1");
            coordinator->enlistServer(BACKUP, "mock:host=backup2");
            coordinator->enlistServer(BACKUP, "mock:host=backup3");
        }

        backup1 =
            new BackupClient(transportManager.getSession("mock:host=backup1"));
        backup2 =
            new BackupClient(transportManager.getSession("mock:host=backup2"));
        backup3 =
            new BackupClient(transportManager.getSession("mock:host=backup3"));

        // Two segs on backup1, one that overlaps with backup2
        segmentsToFree.push_back(
            new WriteValidSegment(99, 88, segmentSize, "mock:host=backup1"));
        segmentsToFree.push_back(
            new WriteValidSegment(99, 89, segmentSize, "mock:host=backup1"));
        // One seg on backup2
        segmentsToFree.push_back(
            new WriteValidSegment(99, 88, segmentSize, "mock:host=backup2"));
        // Zero segs on backup3

        masterHosts = new ProtoBuf::ServerList();
        {
            ProtoBuf::ServerList::Entry& host(*masterHosts->add_server());
            host.set_server_type(ProtoBuf::MASTER);
            host.set_server_id(9999998);
            host.set_service_locator("mock:host=master1");
        }{
            ProtoBuf::ServerList::Entry& host(*masterHosts->add_server());
            host.set_server_type(ProtoBuf::MASTER);
            host.set_server_id(9999999);
            host.set_service_locator("mock:host=master2");
        }

        backupHosts = new ProtoBuf::ServerList();
        {
            ProtoBuf::ServerList::Entry& host(*backupHosts->add_server());
            host.set_server_type(ProtoBuf::BACKUP);
            host.set_server_id(backupServer1->getServerId());
            host.set_service_locator("mock:host=backup1");
        }{
            ProtoBuf::ServerList::Entry& host(*backupHosts->add_server());
            host.set_server_type(ProtoBuf::BACKUP);
            host.set_server_id(backupServer2->getServerId());
            host.set_service_locator("mock:host=backup2");
        }{
            ProtoBuf::ServerList::Entry& host(*backupHosts->add_server());
            host.set_server_type(ProtoBuf::BACKUP);
            host.set_server_id(backupServer3->getServerId());
            host.set_service_locator("mock:host=backup3");
        }
    }

    void
    setUp()
    {
        setUp(true);
    }

    void
    tearDown()
    {
        delete backupHosts;
        delete masterHosts;
        foreach (WriteValidSegment* s, segmentsToFree)
            delete s;
        delete backup3;
        delete backup2;
        delete backup1;
        delete backupServer3;
        delete backupServer2;
        delete backupServer1;
        delete storage3;
        delete storage2;
        delete storage1;
        delete coordinator;
        delete coordinatorServer;
        delete config;
        transportManager.unregisterMock();
        delete transport;
        CPPUNIT_ASSERT_EQUAL(0,
            BackupStorage::Handle::resetAllocatedHandlesCount());
    }

    void
    test_buildSegmentIdToBackups()
    {
        ProtoBuf::Tablets tablets;
        Recovery recovery(99, tablets, *masterHosts, *backupHosts);

        Recovery::BackupMap::iterator it = recovery.segmentIdToBackups.begin();
        CPPUNIT_ASSERT_EQUAL(88, it->first);
        CPPUNIT_ASSERT_EQUAL("mock:host=backup1", it->second.service_locator());
        CPPUNIT_ASSERT_EQUAL(backupServer1->getServerId(),
                             it->second.server_id());
        it++;
        CPPUNIT_ASSERT(recovery.segmentIdToBackups.end() != it);
        CPPUNIT_ASSERT_EQUAL(88, it->first);
        CPPUNIT_ASSERT_EQUAL("mock:host=backup2", it->second.service_locator());
        CPPUNIT_ASSERT_EQUAL(backupServer2->getServerId(),
                             it->second.server_id());
        it++;
        CPPUNIT_ASSERT(recovery.segmentIdToBackups.end() != it);
        CPPUNIT_ASSERT_EQUAL(89, it->first);
        CPPUNIT_ASSERT_EQUAL("mock:host=backup1", it->second.service_locator());
        CPPUNIT_ASSERT_EQUAL(backupServer1->getServerId(),
                             it->second.server_id());
        it++;
        CPPUNIT_ASSERT(recovery.segmentIdToBackups.end() == it);
    }

    void
    test_createBackupList()
    {
        ProtoBuf::Tablets tablets;
        Recovery recovery(99, tablets, *masterHosts, *backupHosts);

        CPPUNIT_ASSERT_EQUAL(3, recovery.backups.server_size());
        {
            const ProtoBuf::ServerList::Entry&
                backup(recovery.backups.server(0));
            CPPUNIT_ASSERT_EQUAL(88, backup.segment_id());
            CPPUNIT_ASSERT_EQUAL("mock:host=backup1", backup.service_locator());
            CPPUNIT_ASSERT_EQUAL(ProtoBuf::BACKUP, backup.server_type());
        }{
            const ProtoBuf::ServerList::Entry&
                backup(recovery.backups.server(1));
            CPPUNIT_ASSERT_EQUAL(88, backup.segment_id());
            CPPUNIT_ASSERT_EQUAL("mock:host=backup2", backup.service_locator());
            CPPUNIT_ASSERT_EQUAL(ProtoBuf::BACKUP, backup.server_type());
        }{
            const ProtoBuf::ServerList::Entry&
                backup(recovery.backups.server(2));
            CPPUNIT_ASSERT_EQUAL(89, backup.segment_id());
            CPPUNIT_ASSERT_EQUAL("mock:host=backup1", backup.service_locator());
            CPPUNIT_ASSERT_EQUAL(ProtoBuf::BACKUP, backup.server_type());
        }
    }

    /// Create a master along with its config and clean them up on destruction.
    struct AutoMaster {
        AutoMaster(BindTransport& transport,
                   CoordinatorClient &coordinator,
                   const string& locator)
            : backup(&coordinator, 0)
            , config()
            , master()
        {
            config.coordinatorLocator = "mock:host=coordinator";
            config.localLocator = locator;
            MasterServer::sizeLogAndHashTable("64", "8", &config);
            master = new MasterServer(config, &coordinator, &backup);
            transport.addServer(*master, locator);
            coordinator.enlistServer(MASTER, locator);
        }

        ~AutoMaster()
        {
            delete master;
        }

        BackupManager backup;
        ServerConfig config;
        MasterServer* master;

        DISALLOW_COPY_AND_ASSIGN(AutoMaster);
    };

    static bool
    getRecoveryDataFilter(string s)
    {
        return s == "getRecoveryData" ||
               s == "start";
    }

    void
    test_start()
    {
        AutoMaster am1(*transport, *coordinator, "mock:host=master1");
        AutoMaster am2(*transport, *coordinator, "mock:host=master2");

        ProtoBuf::Tablets tablets; {
            ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
            tablet.set_table_id(123);
            tablet.set_start_object_id(0);
            tablet.set_end_object_id(9);
            tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
            tablet.set_user_data(0); // partition 0
        }{
            ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
            tablet.set_table_id(123);
            tablet.set_start_object_id(10);
            tablet.set_end_object_id(19);
            tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
            tablet.set_user_data(1); // partition 1
        }

        Recovery recovery(99, tablets, *masterHosts, *backupHosts);
        TestLog::Enable _(&getRecoveryDataFilter);
        recovery.start();
        CPPUNIT_ASSERT_EQUAL(
            "start: Trying partition recovery on mock:host=master1 with "
            "1 tablets and 3 hosts | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 "
            "complete | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 "
            "complete | "
            "start: Trying partition recovery on mock:host=master2 with "
            "1 tablets and 3 hosts | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 "
            "complete | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 "
            "complete",
            TestLog::get());
    }

    void
    test_start_notEnoughMasters()
    {
        AutoMaster am1(*transport, *coordinator, "mock:host=master1");
        AutoMaster am2(*transport, *coordinator, "mock:host=master2");

        ProtoBuf::Tablets tablets; {
            ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
            tablet.set_table_id(123);
            tablet.set_start_object_id(0);
            tablet.set_end_object_id(9);
            tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
            tablet.set_user_data(0); // partition 0
        }{
            ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
            tablet.set_table_id(123);
            tablet.set_start_object_id(10);
            tablet.set_end_object_id(19);
            tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
            tablet.set_user_data(1); // partition 1
        }{
            ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
            tablet.set_table_id(123);
            tablet.set_start_object_id(20);
            tablet.set_end_object_id(29);
            tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
            tablet.set_user_data(2); // partition 2
        }

        Recovery recovery(99, tablets, *masterHosts, *backupHosts);
        TestLog::Enable _(&getRecoveryDataFilter);
        recovery.start();
        CPPUNIT_ASSERT_EQUAL(
            "start: Trying partition recovery on mock:host=master1 with "
            "1 tablets and 3 hosts | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 "
            "complete | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 "
            "complete | "
            "start: Trying partition recovery on mock:host=master2 with "
            "1 tablets and 3 hosts | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 88 "
            "complete | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 | "
            "getRecoveryData: getRecoveryData masterId 99, segmentId 89 "
            "complete | "
            "start: Failed to recover all partitions for a crashed master, "
            "your RAMCloud is now busted.",
            TestLog::get());
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(RecoveryTest);
};
CPPUNIT_TEST_SUITE_REGISTRATION(RecoveryTest);


} // namespace RAMCloud
