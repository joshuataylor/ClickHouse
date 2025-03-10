#include <Storages/MergeTree/MergeTreeDataWriter.h>
#include <Storages/MergeTree/MergedBlockOutputStream.h>
#include <Storages/MergeTree/DataPartStorageOnDisk.h>
#include <Columns/ColumnConst.h>
#include <Common/HashTable/HashMap.h>
#include <Common/Exception.h>
#include <Disks/createVolume.h>
#include <Interpreters/AggregationCommon.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/MergeTreeTransaction.h>
#include <IO/HashingWriteBuffer.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/ObjectUtils.h>
#include <IO/WriteHelpers.h>
#include <Common/typeid_cast.h>
#include <Processors/TTL/ITTLAlgorithm.h>

#include <Parsers/queryToString.h>

#include <Processors/Merges/Algorithms/ReplacingSortedAlgorithm.h>
#include <Processors/Merges/Algorithms/MergingSortedAlgorithm.h>
#include <Processors/Merges/Algorithms/CollapsingSortedAlgorithm.h>
#include <Processors/Merges/Algorithms/SummingSortedAlgorithm.h>
#include <Processors/Merges/Algorithms/AggregatingSortedAlgorithm.h>
#include <Processors/Merges/Algorithms/VersionedCollapsingAlgorithm.h>
#include <Processors/Merges/Algorithms/GraphiteRollupSortedAlgorithm.h>
#include <Processors/Sources/SourceFromSingleChunk.h>

namespace ProfileEvents
{
    extern const Event MergeTreeDataWriterBlocks;
    extern const Event MergeTreeDataWriterBlocksAlreadySorted;
    extern const Event MergeTreeDataWriterRows;
    extern const Event MergeTreeDataWriterUncompressedBytes;
    extern const Event MergeTreeDataWriterCompressedBytes;
    extern const Event MergeTreeDataProjectionWriterBlocks;
    extern const Event MergeTreeDataProjectionWriterBlocksAlreadySorted;
    extern const Event MergeTreeDataProjectionWriterRows;
    extern const Event MergeTreeDataProjectionWriterUncompressedBytes;
    extern const Event MergeTreeDataProjectionWriterCompressedBytes;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int TOO_MANY_PARTS;
}

namespace
{

void buildScatterSelector(
        const ColumnRawPtrs & columns,
        PODArray<size_t> & partition_num_to_first_row,
        IColumn::Selector & selector,
        size_t max_parts)
{
    /// Use generic hashed variant since partitioning is unlikely to be a bottleneck.
    using Data = HashMap<UInt128, size_t, UInt128TrivialHash>;
    Data partitions_map;

    size_t num_rows = columns[0]->size();
    size_t partitions_count = 0;
    for (size_t i = 0; i < num_rows; ++i)
    {
        Data::key_type key = hash128(i, columns.size(), columns);
        typename Data::LookupResult it;
        bool inserted;
        partitions_map.emplace(key, it, inserted);

        if (inserted)
        {
            if (max_parts && partitions_count >= max_parts)
                throw Exception("Too many partitions for single INSERT block (more than " + toString(max_parts) + "). The limit is controlled by 'max_partitions_per_insert_block' setting. Large number of partitions is a common misconception. It will lead to severe negative performance impact, including slow server startup, slow INSERT queries and slow SELECT queries. Recommended total number of partitions for a table is under 1000..10000. Please note, that partitioning is not intended to speed up SELECT queries (ORDER BY key is sufficient to make range queries fast). Partitions are intended for data manipulation (DROP PARTITION, etc).", ErrorCodes::TOO_MANY_PARTS);

            partition_num_to_first_row.push_back(i);
            it->getMapped() = partitions_count;

            ++partitions_count;

            /// Optimization for common case when there is only one partition - defer selector initialization.
            if (partitions_count == 2)
            {
                selector = IColumn::Selector(num_rows);
                std::fill(selector.begin(), selector.begin() + i, 0);
            }
        }

        if (partitions_count > 1)
            selector[i] = it->getMapped();
    }
}

/// Computes ttls and updates ttl infos
void updateTTL(
    const TTLDescription & ttl_entry,
    IMergeTreeDataPart::TTLInfos & ttl_infos,
    DB::MergeTreeDataPartTTLInfo & ttl_info,
    const Block & block,
    bool update_part_min_max_ttls)
{
    auto ttl_column = ITTLAlgorithm::executeExpressionAndGetColumn(ttl_entry.expression, block, ttl_entry.result_column);

    if (const ColumnUInt16 * column_date = typeid_cast<const ColumnUInt16 *>(ttl_column.get()))
    {
        const auto & date_lut = DateLUT::instance();
        for (const auto & val : column_date->getData())
            ttl_info.update(date_lut.fromDayNum(DayNum(val)));
    }
    else if (const ColumnUInt32 * column_date_time = typeid_cast<const ColumnUInt32 *>(ttl_column.get()))
    {
        for (const auto & val : column_date_time->getData())
            ttl_info.update(val);
    }
    else if (const ColumnConst * column_const = typeid_cast<const ColumnConst *>(ttl_column.get()))
    {
        if (typeid_cast<const ColumnUInt16 *>(&column_const->getDataColumn()))
        {
            const auto & date_lut = DateLUT::instance();
            ttl_info.update(date_lut.fromDayNum(DayNum(column_const->getValue<UInt16>())));
        }
        else if (typeid_cast<const ColumnUInt32 *>(&column_const->getDataColumn()))
        {
            ttl_info.update(column_const->getValue<UInt32>());
        }
        else
            throw Exception("Unexpected type of result TTL column", ErrorCodes::LOGICAL_ERROR);
    }
    else
        throw Exception("Unexpected type of result TTL column", ErrorCodes::LOGICAL_ERROR);

    if (update_part_min_max_ttls)
        ttl_infos.updatePartMinMaxTTL(ttl_info.min, ttl_info.max);
}

}

void MergeTreeDataWriter::TemporaryPart::finalize()
{
    for (auto & stream : streams)
        stream.finalizer.finish();
}

BlocksWithPartition MergeTreeDataWriter::splitBlockIntoParts(
    const Block & block, size_t max_parts, const StorageMetadataPtr & metadata_snapshot, ContextPtr context)
{
    BlocksWithPartition result;
    if (!block || !block.rows())
        return result;

    metadata_snapshot->check(block, true);

    if (!metadata_snapshot->hasPartitionKey()) /// Table is not partitioned.
    {
        result.emplace_back(Block(block), Row{});
        return result;
    }

    Block block_copy = block;
    /// After expression execution partition key columns will be added to block_copy with names regarding partition function.
    auto partition_key_names_and_types = MergeTreePartition::executePartitionByExpression(metadata_snapshot, block_copy, context);

    ColumnRawPtrs partition_columns;
    partition_columns.reserve(partition_key_names_and_types.size());
    for (const auto & element : partition_key_names_and_types)
        partition_columns.emplace_back(block_copy.getByName(element.name).column.get());

    PODArray<size_t> partition_num_to_first_row;
    IColumn::Selector selector;
    buildScatterSelector(partition_columns, partition_num_to_first_row, selector, max_parts);

    size_t partitions_count = partition_num_to_first_row.size();
    result.reserve(partitions_count);

    auto get_partition = [&](size_t num)
    {
        Row partition(partition_columns.size());
        for (size_t i = 0; i < partition_columns.size(); ++i)
            partition[i] = (*partition_columns[i])[partition_num_to_first_row[num]];
        return partition;
    };

    if (partitions_count == 1)
    {
        /// A typical case is when there is one partition (you do not need to split anything).
        /// NOTE: returning a copy of the original block so that calculated partition key columns
        /// do not interfere with possible calculated primary key columns of the same name.
        result.emplace_back(Block(block), get_partition(0));
        return result;
    }

    for (size_t i = 0; i < partitions_count; ++i)
        result.emplace_back(block.cloneEmpty(), get_partition(i));

    for (size_t col = 0; col < block.columns(); ++col)
    {
        MutableColumns scattered = block.getByPosition(col).column->scatter(partitions_count, selector);
        for (size_t i = 0; i < partitions_count; ++i)
            result[i].block.getByPosition(col).column = std::move(scattered[i]);
    }

    return result;
}

Block MergeTreeDataWriter::mergeBlock(
    const Block & block,
    SortDescription sort_description,
    const Names & partition_key_columns,
    IColumn::Permutation *& permutation,
    const MergeTreeData::MergingParams & merging_params)
{
    size_t block_size = block.rows();

    auto get_merging_algorithm = [&]() -> std::shared_ptr<IMergingAlgorithm>
    {
        switch (merging_params.mode)
        {
            /// There is nothing to merge in single block in ordinary MergeTree
            case MergeTreeData::MergingParams::Ordinary:
                return nullptr;
            case MergeTreeData::MergingParams::Replacing:
                return std::make_shared<ReplacingSortedAlgorithm>(
                    block, 1, sort_description, merging_params.version_column, block_size + 1);
            case MergeTreeData::MergingParams::Collapsing:
                return std::make_shared<CollapsingSortedAlgorithm>(
                    block, 1, sort_description, merging_params.sign_column,
                    false, block_size + 1, &Poco::Logger::get("MergeTreeDataWriter"));
            case MergeTreeData::MergingParams::Summing:
                return std::make_shared<SummingSortedAlgorithm>(
                    block, 1, sort_description, merging_params.columns_to_sum,
                    partition_key_columns, block_size + 1);
            case MergeTreeData::MergingParams::Aggregating:
                return std::make_shared<AggregatingSortedAlgorithm>(block, 1, sort_description, block_size + 1);
            case MergeTreeData::MergingParams::VersionedCollapsing:
                return std::make_shared<VersionedCollapsingAlgorithm>(
                    block, 1, sort_description, merging_params.sign_column, block_size + 1);
            case MergeTreeData::MergingParams::Graphite:
                return std::make_shared<GraphiteRollupSortedAlgorithm>(
                    block, 1, sort_description, block_size + 1, merging_params.graphite_params, time(nullptr));
        }

        __builtin_unreachable();
    };

    auto merging_algorithm = get_merging_algorithm();
    if (!merging_algorithm)
        return block;

    Chunk chunk(block.getColumns(), block_size);

    IMergingAlgorithm::Input input;
    input.set(std::move(chunk));
    input.permutation = permutation;

    IMergingAlgorithm::Inputs inputs;
    inputs.push_back(std::move(input));
    merging_algorithm->initialize(std::move(inputs));

    IMergingAlgorithm::Status status = merging_algorithm->merge();

    /// Check that after first merge merging_algorithm is waiting for data from input 0.
    if (status.required_source != 0)
        throw Exception("Logical error: required source after the first merge is not 0.", ErrorCodes::LOGICAL_ERROR);

    status = merging_algorithm->merge();

    /// Check that merge is finished.
    if (!status.is_finished)
        throw Exception("Logical error: merge is not finished after the second merge.", ErrorCodes::LOGICAL_ERROR);

    /// Merged Block is sorted and we don't need to use permutation anymore
    permutation = nullptr;

    return block.cloneWithColumns(status.chunk.getColumns());
}

MergeTreeDataWriter::TemporaryPart MergeTreeDataWriter::writeTempPart(
    BlockWithPartition & block_with_partition, const StorageMetadataPtr & metadata_snapshot, ContextPtr context)
{
    TemporaryPart temp_part;
    Block & block = block_with_partition.block;

    auto columns = metadata_snapshot->getColumns().getAllPhysical().filter(block.getNames());

    for (auto & column : columns)
        if (isObject(column.type))
            column.type = block.getByName(column.name).type;

    static const String TMP_PREFIX = "tmp_insert_";

    /// This will generate unique name in scope of current server process.
    Int64 temp_index = data.insert_increment.get();

    auto minmax_idx = std::make_shared<IMergeTreeDataPart::MinMaxIndex>();
    minmax_idx->update(block, data.getMinMaxColumnsNames(metadata_snapshot->getPartitionKey()));

    MergeTreePartition partition(std::move(block_with_partition.partition));

    MergeTreePartInfo new_part_info(partition.getID(metadata_snapshot->getPartitionKey().sample_block), temp_index, temp_index, 0);
    String part_name;
    if (data.format_version < MERGE_TREE_DATA_MIN_FORMAT_VERSION_WITH_CUSTOM_PARTITIONING)
    {
        DayNum min_date(minmax_idx->hyperrectangle[data.minmax_idx_date_column_pos].left.get<UInt64>());
        DayNum max_date(minmax_idx->hyperrectangle[data.minmax_idx_date_column_pos].right.get<UInt64>());

        const auto & date_lut = DateLUT::instance();

        auto min_month = date_lut.toNumYYYYMM(min_date);
        auto max_month = date_lut.toNumYYYYMM(max_date);

        if (min_month != max_month)
            throw Exception("Logical error: part spans more than one month.", ErrorCodes::LOGICAL_ERROR);

        part_name = new_part_info.getPartNameV0(min_date, max_date);
    }
    else
        part_name = new_part_info.getPartName();

    String part_dir = TMP_PREFIX + part_name;
    temp_part.temporary_directory_lock = data.getTemporaryPartDirectoryHolder(part_dir);

    /// If we need to calculate some columns to sort.
    if (metadata_snapshot->hasSortingKey() || metadata_snapshot->hasSecondaryIndices())
        data.getSortingKeyAndSkipIndicesExpression(metadata_snapshot)->execute(block);

    Names sort_columns = metadata_snapshot->getSortingKeyColumns();
    SortDescription sort_description;
    size_t sort_columns_size = sort_columns.size();
    sort_description.reserve(sort_columns_size);

    for (size_t i = 0; i < sort_columns_size; ++i)
        sort_description.emplace_back(sort_columns[i], 1, 1);

    ProfileEvents::increment(ProfileEvents::MergeTreeDataWriterBlocks);

    /// Sort
    IColumn::Permutation * perm_ptr = nullptr;
    IColumn::Permutation perm;
    if (!sort_description.empty())
    {
        if (!isAlreadySorted(block, sort_description))
        {
            stableGetPermutation(block, sort_description, perm);
            perm_ptr = &perm;
        }
        else
            ProfileEvents::increment(ProfileEvents::MergeTreeDataWriterBlocksAlreadySorted);
    }

    Names partition_key_columns = metadata_snapshot->getPartitionKey().column_names;
    if (context->getSettingsRef().optimize_on_insert)
        block = mergeBlock(block, sort_description, partition_key_columns, perm_ptr, data.merging_params);

    /// Size of part would not be greater than block.bytes() + epsilon
    size_t expected_size = block.bytes();

    /// If optimize_on_insert is true, block may become empty after merge.
    /// There is no need to create empty part.
    if (expected_size == 0)
        return temp_part;

    DB::IMergeTreeDataPart::TTLInfos move_ttl_infos;
    const auto & move_ttl_entries = metadata_snapshot->getMoveTTLs();
    for (const auto & ttl_entry : move_ttl_entries)
        updateTTL(ttl_entry, move_ttl_infos, move_ttl_infos.moves_ttl[ttl_entry.result_column], block, false);

    ReservationPtr reservation = data.reserveSpacePreferringTTLRules(metadata_snapshot, expected_size, move_ttl_infos, time(nullptr), 0, true);
    VolumePtr volume = data.getStoragePolicy()->getVolume(0);
    VolumePtr data_part_volume = createVolumeFromReservation(reservation, volume);

    auto data_part_storage = std::make_shared<DataPartStorageOnDisk>(
        data_part_volume,
        data.relative_data_path,
        TMP_PREFIX + part_name);

    auto data_part_storage_builder = std::make_shared<DataPartStorageBuilderOnDisk>(
        data_part_volume,
        data.relative_data_path,
        TMP_PREFIX + part_name);

    auto new_data_part = data.createPart(
        part_name,
        data.choosePartType(expected_size, block.rows()),
        new_part_info,
        data_part_storage);

    if (data.storage_settings.get()->assign_part_uuids)
        new_data_part->uuid = UUIDHelpers::generateV4();

    const auto & data_settings = data.getSettings();

    SerializationInfo::Settings settings{data_settings->ratio_of_defaults_for_sparse_serialization, true};
    SerializationInfoByName infos(columns, settings);
    infos.add(block);

    new_data_part->setColumns(columns, infos);
    new_data_part->rows_count = block.rows();
    new_data_part->partition = std::move(partition);
    new_data_part->minmax_idx = std::move(minmax_idx);
    new_data_part->is_temp = true;

    SyncGuardPtr sync_guard;
    if (new_data_part->isStoredOnDisk())
    {
        /// The name could be non-unique in case of stale files from previous runs.
        String full_path = new_data_part->data_part_storage->getFullPath();

        if (new_data_part->data_part_storage->exists())
        {
            LOG_WARNING(log, "Removing old temporary directory {}", full_path);
            data_part_storage_builder->removeRecursive();
        }

        data_part_storage_builder->createDirectories();

        if (data.getSettings()->fsync_part_directory)
        {
            const auto disk = data_part_volume->getDisk();
            sync_guard = disk->getDirectorySyncGuard(full_path);
        }
    }

    if (metadata_snapshot->hasRowsTTL())
        updateTTL(metadata_snapshot->getRowsTTL(), new_data_part->ttl_infos, new_data_part->ttl_infos.table_ttl, block, true);

    for (const auto & ttl_entry : metadata_snapshot->getGroupByTTLs())
        updateTTL(ttl_entry, new_data_part->ttl_infos, new_data_part->ttl_infos.group_by_ttl[ttl_entry.result_column], block, true);

    for (const auto & ttl_entry : metadata_snapshot->getRowsWhereTTLs())
        updateTTL(ttl_entry, new_data_part->ttl_infos, new_data_part->ttl_infos.rows_where_ttl[ttl_entry.result_column], block, true);

    for (const auto & [name, ttl_entry] : metadata_snapshot->getColumnTTLs())
        updateTTL(ttl_entry, new_data_part->ttl_infos, new_data_part->ttl_infos.columns_ttl[name], block, true);

    const auto & recompression_ttl_entries = metadata_snapshot->getRecompressionTTLs();
    for (const auto & ttl_entry : recompression_ttl_entries)
        updateTTL(ttl_entry, new_data_part->ttl_infos, new_data_part->ttl_infos.recompression_ttl[ttl_entry.result_column], block, false);

    new_data_part->ttl_infos.update(move_ttl_infos);

    /// This effectively chooses minimal compression method:
    ///  either default lz4 or compression method with zero thresholds on absolute and relative part size.
    auto compression_codec = data.getContext()->chooseCompressionCodec(0, 0);

    const auto & index_factory = MergeTreeIndexFactory::instance();
    auto out = std::make_unique<MergedBlockOutputStream>(new_data_part, data_part_storage_builder, metadata_snapshot, columns,
        index_factory.getMany(metadata_snapshot->getSecondaryIndices()), compression_codec,
        context->getCurrentTransaction(), false, false, context->getWriteSettings());

    out->writeWithPermutation(block, perm_ptr);

    for (const auto & projection : metadata_snapshot->getProjections())
    {
        auto projection_block = projection.calculate(block, context);
        if (projection_block.rows())
        {
            auto proj_temp_part = writeProjectionPart(data, log, projection_block, projection, data_part_storage_builder, new_data_part.get());
            new_data_part->addProjectionPart(projection.name, std::move(proj_temp_part.part));
            proj_temp_part.builder->commit();
            for (auto & stream : proj_temp_part.streams)
                temp_part.streams.emplace_back(std::move(stream));
        }
    }

    auto finalizer = out->finalizePartAsync(
        new_data_part,
        data_settings->fsync_after_insert,
        nullptr, nullptr);

    temp_part.part = new_data_part;
    temp_part.builder = data_part_storage_builder;
    temp_part.streams.emplace_back(TemporaryPart::Stream{.stream = std::move(out), .finalizer = std::move(finalizer)});

    ProfileEvents::increment(ProfileEvents::MergeTreeDataWriterRows, block.rows());
    ProfileEvents::increment(ProfileEvents::MergeTreeDataWriterUncompressedBytes, block.bytes());
    ProfileEvents::increment(ProfileEvents::MergeTreeDataWriterCompressedBytes, new_data_part->getBytesOnDisk());

    return temp_part;
}

void MergeTreeDataWriter::deduceTypesOfObjectColumns(const StorageSnapshotPtr & storage_snapshot, Block & block)
{
    if (!storage_snapshot->object_columns.empty())
    {
        auto options = GetColumnsOptions(GetColumnsOptions::AllPhysical).withExtendedObjects();
        auto storage_columns = storage_snapshot->getColumns(options);
        convertObjectsToTuples(block, storage_columns);
    }
}

MergeTreeDataWriter::TemporaryPart MergeTreeDataWriter::writeProjectionPartImpl(
    const String & part_name,
    MergeTreeDataPartType part_type,
    const String & relative_path,
    const DataPartStorageBuilderPtr & data_part_storage_builder,
    bool is_temp,
    const IMergeTreeDataPart * parent_part,
    const MergeTreeData & data,
    Poco::Logger * log,
    Block block,
    const ProjectionDescription & projection)
{
    TemporaryPart temp_part;
    const StorageMetadataPtr & metadata_snapshot = projection.metadata;
    MergeTreePartInfo new_part_info("all", 0, 0, 0);
    auto projection_part_storage = parent_part->data_part_storage->getProjection(relative_path);
    auto new_data_part = data.createPart(
        part_name,
        part_type,
        new_part_info,
        projection_part_storage,
        parent_part);

    auto projection_part_storage_builder = data_part_storage_builder->getProjection(relative_path);
    new_data_part->is_temp = is_temp;

    NamesAndTypesList columns = metadata_snapshot->getColumns().getAllPhysical().filter(block.getNames());
    SerializationInfo::Settings settings{data.getSettings()->ratio_of_defaults_for_sparse_serialization, true};
    SerializationInfoByName infos(columns, settings);
    infos.add(block);

    new_data_part->setColumns(columns, infos);

    if (new_data_part->isStoredOnDisk())
    {
        /// The name could be non-unique in case of stale files from previous runs.
        if (projection_part_storage->exists())
        {
            LOG_WARNING(log, "Removing old temporary directory {}", projection_part_storage->getFullPath());
            projection_part_storage_builder->removeRecursive();
        }

        projection_part_storage_builder->createDirectories();
    }

    /// If we need to calculate some columns to sort.
    if (metadata_snapshot->hasSortingKey() || metadata_snapshot->hasSecondaryIndices())
        data.getSortingKeyAndSkipIndicesExpression(metadata_snapshot)->execute(block);

    Names sort_columns = metadata_snapshot->getSortingKeyColumns();
    SortDescription sort_description;
    size_t sort_columns_size = sort_columns.size();
    sort_description.reserve(sort_columns_size);

    for (size_t i = 0; i < sort_columns_size; ++i)
        sort_description.emplace_back(sort_columns[i], 1, 1);

    ProfileEvents::increment(ProfileEvents::MergeTreeDataProjectionWriterBlocks);

    /// Sort
    IColumn::Permutation * perm_ptr = nullptr;
    IColumn::Permutation perm;
    if (!sort_description.empty())
    {
        if (!isAlreadySorted(block, sort_description))
        {
            stableGetPermutation(block, sort_description, perm);
            perm_ptr = &perm;
        }
        else
            ProfileEvents::increment(ProfileEvents::MergeTreeDataProjectionWriterBlocksAlreadySorted);
    }

    if (projection.type == ProjectionDescription::Type::Aggregate)
    {
        MergeTreeData::MergingParams projection_merging_params;
        projection_merging_params.mode = MergeTreeData::MergingParams::Aggregating;
        block = mergeBlock(block, sort_description, {}, perm_ptr, projection_merging_params);
    }

    /// This effectively chooses minimal compression method:
    ///  either default lz4 or compression method with zero thresholds on absolute and relative part size.
    auto compression_codec = data.getContext()->chooseCompressionCodec(0, 0);

    auto out = std::make_unique<MergedBlockOutputStream>(
        new_data_part,
        projection_part_storage_builder,
        metadata_snapshot,
        columns,
        MergeTreeIndices{},
        compression_codec,
        NO_TRANSACTION_PTR,
        false, false, data.getContext()->getWriteSettings());

    out->writeWithPermutation(block, perm_ptr);
    auto finalizer = out->finalizePartAsync(new_data_part, false);
    temp_part.part = new_data_part;
    temp_part.builder = projection_part_storage_builder;
    temp_part.streams.emplace_back(TemporaryPart::Stream{.stream = std::move(out), .finalizer = std::move(finalizer)});

    ProfileEvents::increment(ProfileEvents::MergeTreeDataProjectionWriterRows, block.rows());
    ProfileEvents::increment(ProfileEvents::MergeTreeDataProjectionWriterUncompressedBytes, block.bytes());
    ProfileEvents::increment(ProfileEvents::MergeTreeDataProjectionWriterCompressedBytes, new_data_part->getBytesOnDisk());

    return temp_part;
}

MergeTreeDataWriter::TemporaryPart MergeTreeDataWriter::writeProjectionPart(
    MergeTreeData & data,
    Poco::Logger * log,
    Block block,
    const ProjectionDescription & projection,
    const DataPartStorageBuilderPtr & data_part_storage_builder,
    const IMergeTreeDataPart * parent_part)
{
    String part_name = projection.name;
    MergeTreeDataPartType part_type;
    if (parent_part->getType() == MergeTreeDataPartType::InMemory)
    {
        part_type = MergeTreeDataPartType::InMemory;
    }
    else
    {
        /// Size of part would not be greater than block.bytes() + epsilon
        size_t expected_size = block.bytes();
        // just check if there is enough space on parent volume
        data.reserveSpace(expected_size, data_part_storage_builder);
        part_type = data.choosePartTypeOnDisk(expected_size, block.rows());
    }

    return writeProjectionPartImpl(
        part_name,
        part_type,
        part_name + ".proj" /* relative_path */,
        data_part_storage_builder,
        false /* is_temp */,
        parent_part,
        data,
        log,
        block,
        projection);
}

/// This is used for projection materialization process which may contain multiple stages of
/// projection part merges.
MergeTreeDataWriter::TemporaryPart MergeTreeDataWriter::writeTempProjectionPart(
    MergeTreeData & data,
    Poco::Logger * log,
    Block block,
    const ProjectionDescription & projection,
    const DataPartStorageBuilderPtr & data_part_storage_builder,
    const IMergeTreeDataPart * parent_part,
    size_t block_num)
{
    String part_name = fmt::format("{}_{}", projection.name, block_num);
    MergeTreeDataPartType part_type;
    if (parent_part->getType() == MergeTreeDataPartType::InMemory)
    {
        part_type = MergeTreeDataPartType::InMemory;
    }
    else
    {
        /// Size of part would not be greater than block.bytes() + epsilon
        size_t expected_size = block.bytes();
        // just check if there is enough space on parent volume
        data.reserveSpace(expected_size, data_part_storage_builder);
        part_type = data.choosePartTypeOnDisk(expected_size, block.rows());
    }

    return writeProjectionPartImpl(
        part_name,
        part_type,
        part_name + ".tmp_proj" /* relative_path */,
        data_part_storage_builder,
        true /* is_temp */,
        parent_part,
        data,
        log,
        block,
        projection);
}

MergeTreeDataWriter::TemporaryPart MergeTreeDataWriter::writeInMemoryProjectionPart(
    const MergeTreeData & data,
    Poco::Logger * log,
    Block block,
    const ProjectionDescription & projection,
    const DataPartStorageBuilderPtr & data_part_storage_builder,
    const IMergeTreeDataPart * parent_part)
{
    return writeProjectionPartImpl(
        projection.name,
        MergeTreeDataPartType::InMemory,
        projection.name + ".proj" /* relative_path */,
        data_part_storage_builder,
        false /* is_temp */,
        parent_part,
        data,
        log,
        block,
        projection);
}

}
