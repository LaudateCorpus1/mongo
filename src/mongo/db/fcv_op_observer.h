/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/op_observer_noop.h"
#include "mongo/db/server_options.h"
#include "mongo/util/version/releases.h"

namespace mongo {

/**
 * OpObserver for Feature Compatibility Version (FCV).
 * Observes all writes to the FCV document under admin.system.version and sets the in-memory FCV
 * value.
 */
class FcvOpObserver final : public OpObserver {
    FcvOpObserver(const FcvOpObserver&) = delete;
    FcvOpObserver& operator=(const FcvOpObserver&) = delete;

public:
    FcvOpObserver() = default;
    ~FcvOpObserver() = default;

    // FcvOpObserver overrides.

    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   bool fromMigrate) final;

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) final;

    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  const OplogDeleteEntryArgs& args) final;

    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;

    // Noop overrides.

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       CollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) final {}

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CollectionUUID collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           bool fromMigrate) final {}

    void onStartIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) final {}

    void onCommitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& nss,
                            CollectionUUID collUUID,
                            const UUID& indexBuildUUID,
                            const std::vector<BSONObj>& indexes,
                            bool fromMigrate) final {}

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CollectionUUID collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           const Status& cause,
                           bool fromMigrate) final {}

    void aboutToDelete(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const BSONObj& doc) final {}
    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID> uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj,
                             const boost::optional<repl::OpTime> preImageOpTime,
                             const boost::optional<repl::OpTime> postImageOpTime,
                             const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                             const boost::optional<OplogSlot> slot) final {}
    void onCreateCollection(OperationContext* opCtx,
                            const CollectionPtr& coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime) final {}
    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo) final {}
    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) final {}
    using OpObserver::onDropCollection;
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid,
                                  std::uint64_t numRecords,
                                  const CollectionDropType dropType) final {
        return {};
    }
    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     OptionalCollectionUUID uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor) final {}
    using OpObserver::onRenameCollection;
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            OptionalCollectionUUID uuid,
                            OptionalCollectionUUID dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp) final {}
    void onImportCollection(OperationContext* opCtx,
                            const UUID& importUUID,
                            const NamespaceString& nss,
                            long long numRecords,
                            long long dataSize,
                            const BSONObj& catalogEntry,
                            const BSONObj& storageMetadata,
                            bool isDryRun) final {}
    using OpObserver::preRenameCollection;
    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp) final {
        return {};
    }
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) final {}
    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) final {}
    void onEmptyCapped(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       OptionalCollectionUUID uuid) final {}
    void onUnpreparedTransactionCommit(OperationContext* opCtx,
                                       std::vector<repl::ReplOperation>* statements,
                                       size_t numberOfPreImagesToWrite) final {}
    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept final{};
    void onTransactionPrepare(OperationContext* opCtx,
                              const std::vector<OplogSlot>& reservedSlots,
                              std::vector<repl::ReplOperation>* statements,
                              size_t numberOfPreImagesToWrite) final{};
    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) final{};
    void onMajorityCommitPointUpdate(ServiceContext* service,
                                     const repl::OpTime& newCommitPoint) final {}

private:
    /**
     * Set the FCV to newVersion, making sure to close any outgoing connections with incompatible
     * servers and closing open transactions if necessary. Increments the server TopologyVersion.
     */
    static void _setVersion(OperationContext* opCtx,
                            multiversion::FeatureCompatibilityVersion newVersion,
                            boost::optional<Timestamp> commitTs = boost::none);

    /**
     * Examines a document inserted or updated in the server configuration collection
     * (admin.system.version). If it is the featureCompatibilityVersion document, validates the
     * document and on commit, updates the server parameter.
     */
    static void _onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc);
};

}  // namespace mongo
