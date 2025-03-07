/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"

namespace mongo {

/**
 * Part of the change stream API machinery used to look up the post-image of a document. Uses the
 * "documentKey" field of the input to look up the new version of the document.
 */
class DocumentSourceChangeStreamAddPostImage final
    : public DocumentSource,
      public ChangeStreamStageSerializationInterface {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamAddPostImage"_sd;
    static constexpr StringData kFullDocumentFieldName =
        DocumentSourceChangeStream::kFullDocumentField;

    /**
     * Creates a DocumentSourceChangeStreamAddPostImage stage.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamAddPostImage> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec) {
        return new DocumentSourceChangeStreamAddPostImage(expCtx, spec.getFullDocument());
    }

    static boost::intrusive_ptr<DocumentSourceChangeStreamAddPostImage> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Only modifies a single path: "fullDocument".
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, {kFullDocumentFieldName.toString()}, {}};
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        invariant(pipeState != Pipeline::SplitState::kSplitForShards);

        // TODO SERVER-55659: remove the feature flag.
        HostTypeRequirement hostTypeRequirement =
            feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()
            ? HostTypeRequirement::kAnyShard
            : HostTypeRequirement::kLocalOnly;

        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     hostTypeRequirement,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);

        constraints.canSwapWithMatch = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const {
        // The namespace is not technically needed yet, but we will if there is more than one
        // collection involved.
        deps->fields.insert(DocumentSourceChangeStream::kNamespaceField.toString());
        deps->fields.insert(DocumentSourceChangeStream::kDocumentKeyField.toString());
        deps->fields.insert(DocumentSourceChangeStream::kOperationTypeField.toString());
        deps->fields.insert(DocumentSourceChangeStream::kIdField.toString());
        // This stage does not restrict the output fields to a finite set, and has no impact on
        // whether metadata is available or needed.
        return DepsTracker::State::SEE_NEXT;
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        return ChangeStreamStageSerializationInterface::serializeToValue(explain);
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

private:
    DocumentSourceChangeStreamAddPostImage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const FullDocumentModeEnum fullDocumentMode)
        : DocumentSource(kStageName, expCtx), _fullDocumentMode(fullDocumentMode) {
        tassert(5842300,
                "the 'fullDocument' field cannot be 'default'",
                _fullDocumentMode != FullDocumentModeEnum::kDefault);
    }

    /**
     * Performs the lookup to retrieve the full document.
     */
    GetNextResult doGetNext() final;

    /**
     * Uses the "documentKey" field from 'updateOp' to look up the current version of the document.
     * Returns Value(BSONNULL) if the document couldn't be found.
     */
    Value lookupPostImage(const Document& updateOp) const;

    /**
     * Throws a AssertionException if the namespace found in 'inputDoc' doesn't match the one on the
     * ExpressionContext. If the namespace on the ExpressionContext is 'collectionless', then this
     * function verifies that the only the database names match.
     */
    NamespaceString assertValidNamespace(const Document& inputDoc) const;

    Value serializeLegacy(boost::optional<ExplainOptions::Verbosity> explain) const final;
    Value serializeLatest(boost::optional<ExplainOptions::Verbosity> explain) const final;

    // Determines whether post-images are strictly required or may be included only when available,
    // and whether to return a point-in-time post-image or the most current majority-committed
    // version of the updated document.
    FullDocumentModeEnum _fullDocumentMode = FullDocumentModeEnum::kDefault;
};

}  // namespace mongo
