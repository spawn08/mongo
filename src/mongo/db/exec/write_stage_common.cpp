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

#include "mongo/db/exec/write_stage_common.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/s/pm2423_feature_flags_gen.h"

namespace mongo {

namespace {

bool isStandaloneOrPrimary(OperationContext* opCtx) {
    const auto replCoord{repl::ReplicationCoordinator::get(opCtx)};
    return replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet ||
        replCoord->getMemberState() == repl::MemberState::RS_PRIMARY;
}

}  // namespace

namespace write_stage_common {

bool ensureStillMatches(const CollectionPtr& collection,
                        OperationContext* opCtx,
                        WorkingSet* ws,
                        WorkingSetID id,
                        const CanonicalQuery* cq) {
    // If the snapshot changed, then we have to make sure we have the latest copy of the doc and
    // that it still matches.
    WorkingSetMember* member = ws->get(id);
    if (opCtx->recoveryUnit()->getSnapshotId() != member->doc.snapshotId()) {
        std::unique_ptr<SeekableRecordCursor> cursor(collection->getCursor(opCtx));

        if (!WorkingSetCommon::fetch(opCtx, ws, id, cursor.get(), collection, collection->ns())) {
            // Doc is already deleted.
            return false;
        }

        // Make sure the re-fetched doc still matches the predicate.
        if (cq && !cq->root()->matchesBSON(member->doc.value().toBson(), nullptr)) {
            // No longer matches.
            return false;
        }

        // Ensure that the BSONObj underlying the WorkingSetMember is owned because the cursor's
        // destructor is allowed to free the memory.
        member->makeObjOwnedIfNeeded();
    }
    return true;
}

bool skipWriteToOrphanDocument(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& doc) {
    if (!isStandaloneOrPrimary(opCtx)) {
        // Secondaries do not apply any filtering logic as the primary already did.
        return false;
    }

    if (!feature_flags::gFeatureFlagNoChangeStreamEventsDueToOrphans.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        return false;
    }

    const auto css{CollectionShardingState::get(opCtx, nss)};
    const auto collFilter{css->getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup)};

    if (!collFilter.isSharded()) {
        // NOTE: Sharded collections queried by direct writes are identified by CSS as unsharded
        // collections. This behavior may be fine because direct writes to orphan documents are
        // currently allowed, making subsequent check on the shard version useless.
        return false;
    }

    if (!OperationShardingState::get(opCtx).getShardVersion(nss)) {
        // Direct writes to shards (not forwarded by router) are allowed.
        return false;
    }

    const ShardKeyPattern shardKeyPattern{collFilter.getKeyPattern()};
    const auto shardKey{shardKeyPattern.extractShardKeyFromDocThrows(doc)};

    return !collFilter.keyBelongsToMe(shardKey);
}

}  // namespace write_stage_common
}  // namespace mongo
