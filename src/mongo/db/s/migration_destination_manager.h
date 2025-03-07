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

#pragma once

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration_destination.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/shard_id.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/timer.h"

namespace mongo {

class OperationContext;
class StartChunkCloneRequest;
class Status;
struct WriteConcernOptions;

namespace repl {
class OpTime;
}

struct CollectionOptionsAndIndexes {
    UUID uuid;
    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
    BSONObj options;
};

/**
 * Drives the receiving side of the MongoD migration process. One instance exists per shard.
 */
class MigrationDestinationManager {
    MigrationDestinationManager(const MigrationDestinationManager&) = delete;
    MigrationDestinationManager& operator=(const MigrationDestinationManager&) = delete;

public:
    enum State { READY, CLONE, CATCHUP, STEADY, COMMIT_START, DONE, FAIL, ABORT };

    MigrationDestinationManager();
    ~MigrationDestinationManager();

    /**
     * Returns the singleton instance of the migration destination manager.
     */
    static MigrationDestinationManager* get(OperationContext* opCtx);

    State getState() const;

    /**
     * Checks whether the MigrationDestinationManager is currently handling a migration.
     */
    bool isActive() const;

    /**
     * Reports the state of the migration manager as a BSON document.
     */
    void report(BSONObjBuilder& b, OperationContext* opCtx, bool waitForSteadyOrDone);

    /**
     * Returns a report on the active migration, if the migration is active. Otherwise return an
     * empty BSONObj.
     */
    BSONObj getMigrationStatusReport();

    /**
     * Returns OK if migration started successfully.
     */
    Status start(OperationContext* opCtx,
                 const NamespaceString& nss,
                 ScopedReceiveChunk scopedReceiveChunk,
                 const StartChunkCloneRequest& cloneRequest,
                 const OID& epoch,
                 const WriteConcernOptions& writeConcern);

    /**
     * Clones documents from a donor shard.
     */
    static repl::OpTime fetchAndApplyBatch(
        OperationContext* opCtx,
        std::function<bool(OperationContext*, BSONObj)> applyBatchFn,
        std::function<bool(OperationContext*, BSONObj*)> fetchBatchFn);

    /**
     * Idempotent method, which causes the current ongoing migration to abort only if it has the
     * specified session id. If the migration is already aborted, does nothing.
     */
    Status abort(const MigrationSessionId& sessionId);

    /**
     * Same as 'abort' above, but unconditionally aborts the current migration without checking the
     * session id. Only used for backwards compatibility.
     */
    void abortWithoutSessionIdCheck();

    Status startCommit(const MigrationSessionId& sessionId);

    /**
     * Gets the collection indexes from fromShardId. If given a chunk manager, will fetch the
     * indexes using the shard version protocol.
     */
    struct IndexesAndIdIndex {
        std::vector<BSONObj> indexSpecs;
        BSONObj idIndexSpec;
    };
    static IndexesAndIdIndex getCollectionIndexes(OperationContext* opCtx,
                                                  const NamespaceStringOrUUID& nssOrUUID,
                                                  const ShardId& fromShardId,
                                                  const boost::optional<ChunkManager>& cm,
                                                  boost::optional<Timestamp> afterClusterTime);

    /**
     * Gets the collection uuid and options from fromShardId. If given a chunk manager, will fetch
     * the collection options using the database version protocol.
     */
    struct CollectionOptionsAndUUID {
        BSONObj options;
        UUID uuid;
    };
    static CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        const ShardId& fromShardId,
        const boost::optional<ChunkManager>& cm,
        boost::optional<Timestamp> afterClusterTime);


    /**
     * Creates the collection on the shard and clones the indexes and options.
     */
    static void cloneCollectionIndexesAndOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptionsAndIndexes& collectionOptionsAndIndexes);

private:
    /**
     * These log the argument msg; then, under lock, move msg to _errmsg and set the state to FAIL.
     * The setStateWailWarn version logs with "warning() << msg".
     */
    void _setStateFail(StringData msg);
    void _setStateFailWarn(StringData msg);

    void _setState(State newState);

    /**
     * Thread which drives the migration apply process on the recipient side.
     */
    void _migrateThread();

    void _migrateDriver(OperationContext* opCtx);

    bool _applyMigrateOp(OperationContext* opCtx, const BSONObj& xfer);

    bool _flushPendingWrites(OperationContext* opCtx, const repl::OpTime& lastOpApplied);

    /**
     * If this shard doesn't own any chunks for the collection to be cloned and the collection
     * exists locally, drops its indexes to guarantee that no stale indexes carry over.
     */
    void _dropLocalIndexesIfNecessary(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptionsAndIndexes& collectionOptionsAndIndexes);

    /**
     * Remembers a chunk range between 'min' and 'max' as a range which will have data migrated
     * into it, to protect it against separate commands to clean up orphaned data. First, though,
     * it schedules deletion of any documents in the range, so that process must be seen to be
     * complete before migrating any new documents in.
     */
    SharedSemiFuture<void> _notePending(OperationContext*, ChunkRange const&);

    /**
     * Stops tracking a chunk range between 'min' and 'max' that previously was having data
     * migrated into it, and schedules deletion of any such documents already migrated in.
     */
    void _forgetPending(OperationContext*, ChunkRange const&);

    /**
     * Checks whether the MigrationDestinationManager is currently handling a migration by checking
     * that the migration "_sessionId" is initialized.
     */
    bool _isActive(WithLock) const;

    // Mutex to guard all fields
    mutable Mutex _mutex = MONGO_MAKE_LATCH("MigrationDestinationManager::_mutex");

    // Migration session ID uniquely identifies the migration and indicates whether the prepare
    // method has been called.
    boost::optional<MigrationSessionId> _sessionId;
    boost::optional<ScopedReceiveChunk> _scopedReceiveChunk;

    // A condition variable on which to wait for the prepare method to be called.
    stdx::condition_variable _isActiveCV;

    stdx::thread _migrateThreadHandle;

    boost::optional<UUID> _migrationId;
    LogicalSessionId _lsid;
    TxnNumber _txnNumber{kUninitializedTxnNumber};
    NamespaceString _nss;
    boost::optional<UUID> _collUuid;
    ConnectionString _fromShardConnString;
    ShardId _fromShard;
    ShardId _toShard;

    BSONObj _min;
    BSONObj _max;
    BSONObj _shardKeyPattern;

    OID _epoch;

    WriteConcernOptions _writeConcern;

    // Set to true once we have accepted the chunk as pending into our metadata. Used so that on
    // failure we can perform the appropriate cleanup.
    bool _chunkMarkedPending{false};

    long long _numCloned{0};
    long long _clonedBytes{0};
    long long _numCatchup{0};
    long long _numSteady{0};

    State _state{READY};
    std::string _errmsg;

    std::unique_ptr<SessionCatalogMigrationDestination> _sessionMigration;

    // Condition variable, which is signalled every time the state of the migration changes.
    stdx::condition_variable _stateChangedCV;
};

}  // namespace mongo
