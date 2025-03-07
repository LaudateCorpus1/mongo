/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/database_version.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");

class CatalogCacheRefreshTest : public CatalogCacheTestFixture {
protected:
    void setUp() override {
        CatalogCacheTestFixture::setUp();

        setupNShards(2);
    }

    void expectGetDatabase() {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            DatabaseType db(
                kNss.db().toString(), {"0"}, true, DatabaseVersion(UUID::gen(), Timestamp()));
            return std::vector<BSONObj>{db.toBSON()};
        }());
    }

    void expectGetCollection(OID epoch, const ShardKeyPattern& shardKeyPattern) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            return std::vector<BSONObj>{getDefaultCollectionType(epoch, shardKeyPattern).toBSON()};
        }());
    }

    void expectCollectionAndChunksAggregationWithReshardingFields(
        OID epoch,
        const ShardKeyPattern& shardKeyPattern,
        UUID reshardingUUID,
        const std::vector<ChunkType>& chunks) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            auto collType = getDefaultCollectionType(epoch, shardKeyPattern);

            TypeCollectionReshardingFields reshardingFields;
            reshardingFields.setReshardingUUID(reshardingUUID);
            collType.setReshardingFields(std::move(reshardingFields));

            std::vector<BSONObj> aggResult{collType.toBSON()};
            std::transform(
                chunks.begin(), chunks.end(), std::back_inserter(aggResult), [](const auto& chunk) {
                    return BSON("chunks" << chunk.toConfigBSON());
                });
            return aggResult;
        }());
    }

    CollectionType getDefaultCollectionType(OID epoch, const ShardKeyPattern& shardKeyPattern) {
        CollectionType collType(kNss, epoch, Date_t::now(), UUID::gen());
        collType.setKeyPattern(shardKeyPattern.toBSON());
        collType.setUnique(false);
        return collType;
    }
};

TEST_F(CatalogCacheRefreshTest, FullLoad) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    const UUID reshardingUUID = UUID::gen();

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    ChunkVersion version(1, 0, epoch, boost::none /* timestamp */);

    ChunkType chunk1(reshardingUUID,
                     {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                     version,
                     {"0"});
    chunk1.setName(OID::gen());
    version.incMinor();

    ChunkType chunk2(reshardingUUID, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
    chunk2.setName(OID::gen());
    version.incMinor();

    ChunkType chunk3(reshardingUUID, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
    chunk3.setName(OID::gen());
    version.incMinor();

    ChunkType chunk4(reshardingUUID,
                     {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                     version,
                     {"1"});
    chunk4.setName(OID::gen());
    version.incMinor();

    expectCollectionAndChunksAggregationWithReshardingFields(
        epoch, shardKeyPattern, reshardingUUID, {chunk1, chunk2, chunk3, chunk4});

    auto cm = *future.default_timed_get();
    ASSERT(cm.isSharded());
    ASSERT_EQ(4, cm.numChunks());
    ASSERT_EQ(reshardingUUID, cm.getReshardingFields()->getReshardingUUID());
}

TEST_F(CatalogCacheRefreshTest, NoLoadIfShardNotMarkedStaleInOperationContext) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    auto initialRoutingInfo(
        makeChunkManager(kNss, shardKeyPattern, nullptr, true, {BSON("_id" << 0)}));
    ASSERT_EQ(2, initialRoutingInfo.numChunks());

    auto futureNoRefresh = scheduleRoutingInfoUnforcedRefresh(kNss);
    auto cm = *futureNoRefresh.default_timed_get();
    ASSERT(cm.isSharded());
    ASSERT_EQ(2, cm.numChunks());
}

class MockLockerAlwaysReportsToBeLocked : public LockerNoop {
public:
    using LockerNoop::LockerNoop;

    bool isLocked() const final {
        return true;
    }
};

DEATH_TEST_F(CatalogCacheRefreshTest, ShouldFailToRefreshWhenLocksAreHeld, "Invariant") {
    operationContext()->setLockState(std::make_unique<MockLockerAlwaysReportsToBeLocked>());
    scheduleRoutingInfoUnforcedRefresh(kNss);
}

TEST_F(CatalogCacheRefreshTest, DatabaseNotFound) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    // Return an empty database (need to return it twice because for missing databases, the
    // CatalogClient tries twice)
    expectFindSendBSONObjVector(kConfigHostAndPort, {});
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    try {
        auto cm = *future.default_timed_get();
        FAIL(str::stream() << "Returning no database did not fail and returned " << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NamespaceNotFound, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, DatabaseBSONCorrupted) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    // Return a corrupted database entry
    expectFindSendBSONObjVector(kConfigHostAndPort,
                                {BSON(
                                    "BadValue"
                                    << "This value should not be in a database config document")});

    try {
        auto cm = *future.default_timed_get();
        FAIL(str::stream() << "Returning corrupted database entry did not fail and returned "
                           << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NoSuchKey, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, CollectionNotFound) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return an empty collection
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    auto cm = *future.default_timed_get();
    ASSERT(!cm.isSharded());
    ASSERT_EQ(ShardId{"0"}, cm.dbPrimary());
}

TEST_F(CatalogCacheRefreshTest, CollectionBSONCorrupted) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return a corrupted collection entry
    expectFindSendBSONObjVector(
        kConfigHostAndPort,
        {BSON("BadValue"
              << "This value should not be in a collection config document")});

    try {
        auto cm = *future.default_timed_get();
        FAIL(str::stream() << "Returning corrupted collection entry did not fail and returned "
                           << cm.toString());
    } catch (const DBException& ex) {
        constexpr int kParseError = 40414;
        ASSERT_EQ(ErrorCodes::Error(kParseError), ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadNoChunksFound) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    try {
        auto cm = *future.default_timed_get();
        FAIL(str::stream() << "Returning no chunks for collection did not fail and returned "
                           << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadNoChunksFound) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    const OID epoch = initialRoutingInfo.getVersion().epoch();

    ASSERT_EQ(1, initialRoutingInfo.numChunks());

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    try {
        auto cm = *future.default_timed_get();
        FAIL(str::stream() << "Returning no chunks for collection did not fail and returned "
                           << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, ChunksBSONCorrupted) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, shardKeyPattern);
        const auto chunk1 =
            ChunkType(coll.getUuid(),
                      {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                      ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                      {"0"});
        return std::vector<BSONObj>{/* collection */
                                    coll.toBSON(),
                                    /* chunks */
                                    coll.toBSON().addFields(
                                        BSON("chunks" << chunk1.toConfigBSON())),
                                    BSON("BadValue"
                                         << "This value should not be in a chunk config document")};
    }());

    try {
        auto cm = *future.default_timed_get();
        FAIL(str::stream() << "Returning no chunks for collection did not fail and returned "
                           << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NoSuchKey, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadMissingChunkWithLowestVersion) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch, boost::none /* timestamp */);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the lowest version.
        version.incMinor();

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectCollectionAndChunksAggregation(kNss, epoch, uuid, shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(kNss, epoch, uuid, shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(kNss, epoch, uuid, shardKeyPattern, incompleteChunks);

    try {
        auto cm = *future.default_timed_get();
        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadMissingChunkWithHighestVersion) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch, boost::none /* timestamp */);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the higest version.
        version.incMinor();

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectCollectionAndChunksAggregation(kNss, epoch, uuid, shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(kNss, epoch, uuid, shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(kNss, epoch, uuid, shardKeyPattern, incompleteChunks);

    try {
        auto cm = *future.default_timed_get();
        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadMissingChunkWithLowestVersion) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    const OID epoch = initialRoutingInfo.getVersion().epoch();
    const UUID uuid = *initialRoutingInfo.getUUID();
    const auto timestamp = initialRoutingInfo.getVersion().getTimestamp();

    ASSERT_EQ(1, initialRoutingInfo.numChunks());

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch, timestamp);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the lowest version.
        version.incMinor();

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectCollectionAndChunksAggregation(
        kNss, epoch, UUID::gen(), shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(
        kNss, epoch, UUID::gen(), shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(
        kNss, epoch, UUID::gen(), shardKeyPattern, incompleteChunks);

    try {
        auto cm = *future.default_timed_get();
        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadMissingChunkWithHighestVersion) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    const OID epoch = initialRoutingInfo.getVersion().epoch();
    const UUID uuid = *initialRoutingInfo.getUUID();
    const auto timestamp = initialRoutingInfo.getVersion().getTimestamp();

    ASSERT_EQ(1, initialRoutingInfo.numChunks());

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch, timestamp);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the higest version.

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectCollectionAndChunksAggregation(
        kNss, epoch, UUID::gen(), shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(
        kNss, epoch, UUID::gen(), shardKeyPattern, incompleteChunks);

    expectCollectionAndChunksAggregation(
        kNss, epoch, UUID::gen(), shardKeyPattern, incompleteChunks);

    try {
        auto cm = *future.default_timed_get();
        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << cm.toString());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, ChunkEpochChangeDuringIncrementalLoadRecoveryAfterRetry) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo.numChunks());

    setupNShards(2);

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    ChunkVersion oldVersion = initialRoutingInfo.getVersion();
    const OID newEpoch = OID::gen();

    // On the first attempt, return set of chunks, one of which has different epoch. This simulates
    // the situation where a collection existed with epoch0, we started a refresh for that
    // collection, the cursor yielded and while it yielded another node dropped the collection and
    // recreated it with different epoch and chunks.
    onFindCommand([&](const RemoteCommandRequest& request) {
        const auto opMsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto aggRequest = unittest::assertGet(
            aggregation_request_helper::parseFromBSONForTests(kNss, opMsg.body));
        const auto& pipeline = aggRequest.getPipeline();

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$eq" << oldVersion.epoch()));
        ASSERT_BSONOBJ_EQ(
            pipeline[2]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$ne" << oldVersion.epoch()));

        const auto coll = getDefaultCollectionType(oldVersion.epoch(), shardKeyPattern);
        const auto collBSON = coll.toBSON();

        oldVersion.incMajor();
        ChunkType chunk1(coll.getUuid(),
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         oldVersion,
                         {"0"});
        chunk1.setName(OID::gen());

        // "Yield" happens here with drop and recreate in between. This is the "last" chunk from the
        // recreated collection.
        ChunkType chunk3(coll.getUuid(),
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         ChunkVersion(5, 2, newEpoch, boost::none /* timestamp */),
                         {"1"});
        chunk3.setName(OID::gen());

        const auto chunk1BSON = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk3BSON = BSON("chunks" << chunk3.toConfigBSON());
        return std::vector<BSONObj>{collBSON, chunk1BSON, chunk3BSON};
    });

    // On the second retry attempt, return the correct set of chunks from the recreated collection
    ChunkVersion newVersion(5, 0, newEpoch, boost::none /* timestamp */);
    onFindCommand([&](const RemoteCommandRequest& request) {
        const auto opMsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto aggRequest = unittest::assertGet(
            aggregation_request_helper::parseFromBSONForTests(kNss, opMsg.body));
        const auto& pipeline = aggRequest.getPipeline();

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$eq" << oldVersion.epoch()));
        ASSERT_BSONOBJ_EQ(
            pipeline[2]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$ne" << oldVersion.epoch()));

        const auto coll = getDefaultCollectionType(newEpoch, shardKeyPattern);
        const auto collBSON = getDefaultCollectionType(newEpoch, shardKeyPattern).toBSON();

        ChunkType chunk1(coll.getUuid(),
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         newVersion,
                         {"0"});
        chunk1.setName(OID::gen());

        newVersion.incMinor();
        ChunkType chunk2(coll.getUuid(), {BSON("_id" << 0), BSON("_id" << 100)}, newVersion, {"0"});
        chunk2.setName(OID::gen());

        newVersion.incMinor();
        ChunkType chunk3(coll.getUuid(),
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         newVersion,
                         {"1"});
        chunk3.setName(OID::gen());

        const auto chunk1BSON = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk2BSON = BSON("chunks" << chunk2.toConfigBSON());
        const auto chunk3BSON = BSON("chunks" << chunk3.toConfigBSON());
        return std::vector<BSONObj>{collBSON, chunk1BSON, chunk2BSON, chunk3BSON};
    });

    auto cm = *future.default_timed_get();
    ASSERT(cm.isSharded());
    ASSERT_EQ(3, cm.numChunks());
    ASSERT_EQ(newVersion, cm.getVersion());
    ASSERT_EQ(ChunkVersion(5, 1, newVersion.epoch(), newVersion.getTimestamp()),
              cm.getVersion({"0"}));
    ASSERT_EQ(ChunkVersion(5, 2, newVersion.epoch(), newVersion.getTimestamp()),
              cm.getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterCollectionEpochChange) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo.numChunks());

    setupNShards(2);

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    ChunkVersion oldVersion = initialRoutingInfo.getVersion();
    ChunkVersion newVersion(1, 0, OID::gen(), boost::none /* timestamp */);
    const UUID uuid = *initialRoutingInfo.getUUID();

    // Return collection with a different epoch and a set of chunks, which represent a split
    onFindCommand([&](const RemoteCommandRequest& request) {
        const auto opMsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto aggRequest = unittest::assertGet(
            aggregation_request_helper::parseFromBSONForTests(kNss, opMsg.body));
        const auto& pipeline = aggRequest.getPipeline();

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$eq" << oldVersion.epoch()));
        ASSERT_BSONOBJ_EQ(
            pipeline[2]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$ne" << oldVersion.epoch()));

        const auto collBSON =
            getDefaultCollectionType(newVersion.epoch(), shardKeyPattern).toBSON();

        ChunkType chunk1(uuid,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         newVersion,
                         {"0"});
        chunk1.setName(OID::gen());
        newVersion.incMinor();

        ChunkType chunk2(uuid,
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         newVersion,
                         {"1"});
        chunk2.setName(OID::gen());

        const auto chunk1BSON = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk2BSON = BSON("chunks" << chunk2.toConfigBSON());
        return std::vector<BSONObj>{collBSON, chunk1BSON, chunk2BSON};
    });

    auto cm = *future.default_timed_get();
    ASSERT(cm.isSharded());
    ASSERT_EQ(2, cm.numChunks());
    ASSERT_EQ(newVersion, cm.getVersion());
    ASSERT_EQ(ChunkVersion(1, 0, newVersion.epoch(), newVersion.getTimestamp()),
              cm.getVersion({"0"}));
    ASSERT_EQ(ChunkVersion(1, 1, newVersion.epoch(), newVersion.getTimestamp()),
              cm.getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterSplit) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo.numChunks());

    ChunkVersion version = initialRoutingInfo.getVersion();

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    // Return set of chunks, which represent a split
    onFindCommand([&](const RemoteCommandRequest& request) {
        const auto opMsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto aggRequest = unittest::assertGet(
            aggregation_request_helper::parseFromBSONForTests(kNss, opMsg.body));
        const auto& pipeline = aggRequest.getPipeline();

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"]
                .Array()[2]["$lookup"]["pipeline"]
                .Array()[1]["$match"]["lastmod"]
                .Obj(),
            BSON("$gte" << Timestamp(version.majorVersion(), version.minorVersion())));

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$eq" << version.epoch()));

        const auto coll = getDefaultCollectionType(version.epoch(), shardKeyPattern);
        const auto collBSON = coll.toBSON();

        version.incMajor();
        ChunkType chunk1(coll.getUuid(),
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         version,
                         {"0"});
        chunk1.setName(OID::gen());

        version.incMinor();
        ChunkType chunk2(coll.getUuid(),
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"0"});
        chunk2.setName(OID::gen());

        const auto chunk1BSON = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk2BSON = BSON("chunks" << chunk2.toConfigBSON());
        return std::vector<BSONObj>{collBSON, chunk1BSON, chunk2BSON};
    });

    auto cm = *future.default_timed_get();
    ASSERT(cm.isSharded());
    ASSERT_EQ(2, cm.numChunks());
    ASSERT_EQ(version, cm.getVersion());
    ASSERT_EQ(version, cm.getVersion({"0"}));
    ASSERT_EQ(ChunkVersion(0, 0, version.epoch(), version.getTimestamp()), cm.getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterMoveWithReshardingFieldsAdded) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    const UUID reshardingUUID = UUID::gen();

    auto initialRoutingInfo(
        makeChunkManager(kNss, shardKeyPattern, nullptr, true, {BSON("_id" << 0)}));
    ASSERT_EQ(2, initialRoutingInfo.numChunks());
    ASSERT(boost::none == initialRoutingInfo.getReshardingFields());

    ChunkVersion version = initialRoutingInfo.getVersion();
    const UUID uuid = *initialRoutingInfo.getUUID();

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    ChunkVersion expectedDestShardVersion;

    // Return set of chunks, which represent a move
    version.incMajor();
    expectedDestShardVersion = version;
    ChunkType chunk1(
        uuid, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"1"});
    chunk1.setName(OID::gen());

    version.incMinor();
    ChunkType chunk2(
        uuid, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"0"});
    chunk2.setName(OID::gen());

    expectCollectionAndChunksAggregationWithReshardingFields(
        version.epoch(), shardKeyPattern, reshardingUUID, {chunk1, chunk2});

    auto cm = *future.default_timed_get();
    ASSERT(cm.isSharded());
    ASSERT_EQ(2, cm.numChunks());
    ASSERT_EQ(reshardingUUID, cm.getReshardingFields()->getReshardingUUID());
    ASSERT_EQ(version, cm.getVersion());
    ASSERT_EQ(version, cm.getVersion({"0"}));
    ASSERT_EQ(expectedDestShardVersion, cm.getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterMoveLastChunkWithReshardingFieldsRemoved) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    const UUID reshardingUUID = UUID::gen();

    TypeCollectionReshardingFields reshardingFields;
    reshardingFields.setReshardingUUID(reshardingUUID);

    auto initialRoutingInfo(
        makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}, reshardingFields));

    ASSERT_EQ(1, initialRoutingInfo.numChunks());
    ASSERT_EQ(reshardingUUID, initialRoutingInfo.getReshardingFields()->getReshardingUUID());

    setupNShards(2);

    ChunkVersion version = initialRoutingInfo.getVersion();

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    // The collection type won't have resharding fields this time.
    // Return set of chunks, which represent a move
    version.incMajor();
    ChunkType chunk1(
        *initialRoutingInfo.getUUID(),
        {shardKeyPattern.getKeyPattern().globalMin(), shardKeyPattern.getKeyPattern().globalMax()},
        version,
        {"1"});
    chunk1.setName(OID::gen());

    expectCollectionAndChunksAggregation(
        kNss, version.epoch(), UUID::gen(), shardKeyPattern, {chunk1});

    auto cm = *future.default_timed_get();
    ASSERT(cm.isSharded());
    ASSERT_EQ(1, cm.numChunks());
    ASSERT_EQ(version, cm.getVersion());
    ASSERT_EQ(ChunkVersion(0, 0, version.epoch(), version.getTimestamp()), cm.getVersion({"0"}));
    ASSERT_EQ(version, cm.getVersion({"1"}));
    ASSERT(boost::none == cm.getReshardingFields());
}

}  // namespace
}  // namespace mongo
