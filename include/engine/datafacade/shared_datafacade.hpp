#ifndef SHARED_DATAFACADE_HPP
#define SHARED_DATAFACADE_HPP

// implements all data storage when shared memory _IS_ used

#include "storage/shared_datatype.hpp"
#include "storage/shared_memory.hpp"
#include "storage/shared_barriers.hpp"
#include "engine/datafacade/datafacade_base.hpp"

#include "extractor/compressed_edge_container.hpp"
#include "extractor/guidance/turn_instruction.hpp"
#include "extractor/guidance/turn_lane_types.hpp"
#include "extractor/profile_properties.hpp"
#include "util/guidance/bearing_class.hpp"
#include "util/guidance/entry_class.hpp"
#include "util/guidance/turn_lanes.hpp"

#include "engine/geospatial_query.hpp"
#include "util/packed_vector.hpp"
#include "util/guidance/turn_bearing.hpp"
#include "util/range_table.hpp"
#include "util/rectangle.hpp"
#include "util/simple_logger.hpp"
#include "util/static_graph.hpp"
#include "util/static_rtree.hpp"
#include "util/typedefs.hpp"

#include <boost/assert.hpp>
#include <boost/thread/tss.hpp>
#include <boost/interprocess/sync/named_sharable_mutex.hpp>
#include <boost/interprocess/sync/sharable_lock.hpp>

#include <cstddef>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>


namespace osrm
{
namespace engine
{
namespace datafacade
{

class SharedDataFacade final : public BaseDataFacade
{

  private:
    using super = BaseDataFacade;
    using QueryGraph = util::StaticGraph<EdgeData, true>;
    using GraphNode = QueryGraph::NodeArrayEntry;
    using GraphEdge = QueryGraph::EdgeArrayEntry;
    using IndexBlock = util::RangeTable<16, true>::BlockT;
    using InputEdge = QueryGraph::InputEdge;
    using RTreeLeaf = super::RTreeLeaf;
    using SharedRTree =
        util::StaticRTree<RTreeLeaf, util::ShM<util::Coordinate, true>::vector, true>;
    using SharedGeospatialQuery = GeospatialQuery<SharedRTree, BaseDataFacade>;
    using RTreeNode = SharedRTree::TreeNode;

    storage::SharedDataLayout *data_layout;
    char *shared_memory;

    std::shared_ptr<storage::SharedBarriers> shared_barriers;
    storage::SharedDataType layout_region;
    storage::SharedDataType data_region;
    unsigned shared_timestamp;

    unsigned m_check_sum;
    std::unique_ptr<QueryGraph> m_query_graph;
    std::unique_ptr<storage::SharedMemory> m_layout_memory;
    std::unique_ptr<storage::SharedMemory> m_large_memory;
    std::string m_timestamp;
    extractor::ProfileProperties *m_profile_properties;

    util::ShM<util::Coordinate, true>::vector m_coordinate_list;
    util::PackedVector<OSMNodeID, true> m_osmnodeid_list;
    util::ShM<GeometryID, true>::vector m_via_geometry_list;
    util::ShM<unsigned, true>::vector m_name_ID_list;
    util::ShM<LaneDataID, true>::vector m_lane_data_id;
    util::ShM<extractor::guidance::TurnInstruction, true>::vector m_turn_instruction_list;
    util::ShM<extractor::TravelMode, true>::vector m_travel_mode_list;
    util::ShM<util::guidance::TurnBearing, true>::vector m_pre_turn_bearing;
    util::ShM<util::guidance::TurnBearing, true>::vector m_post_turn_bearing;
    util::ShM<char, true>::vector m_names_char_list;
    util::ShM<unsigned, true>::vector m_name_begin_indices;
    util::ShM<unsigned, true>::vector m_geometry_indices;
    util::ShM<NodeID, true>::vector m_geometry_node_list;
    util::ShM<EdgeWeight, true>::vector m_geometry_fwd_weight_list;
    util::ShM<EdgeWeight, true>::vector m_geometry_rev_weight_list;
    util::ShM<bool, true>::vector m_is_core_node;
    util::ShM<uint8_t, true>::vector m_datasource_list;
    util::ShM<std::uint32_t, true>::vector m_lane_description_offsets;
    util::ShM<extractor::guidance::TurnLaneType::Mask, true>::vector m_lane_description_masks;

    util::ShM<char, true>::vector m_datasource_name_data;
    util::ShM<std::size_t, true>::vector m_datasource_name_offsets;
    util::ShM<std::size_t, true>::vector m_datasource_name_lengths;
    util::ShM<util::guidance::LaneTupleIdPair, true>::vector m_lane_tupel_id_pairs;

    std::unique_ptr<SharedRTree> m_static_rtree;
    std::unique_ptr<SharedGeospatialQuery> m_geospatial_query;
    boost::filesystem::path file_index_path;

    std::shared_ptr<util::RangeTable<16, true>> m_name_table;
    // bearing classes by node based node
    util::ShM<BearingClassID, true>::vector m_bearing_class_id_table;
    // entry class IDs
    util::ShM<EntryClassID, true>::vector m_entry_class_id_list;

    // the look-up table for entry classes. An entry class lists the possibility of entry for all
    // available turns. Such a class id is stored with every edge.
    util::ShM<util::guidance::EntryClass, true>::vector m_entry_class_table;
    // the look-up table for distinct bearing classes. A bearing class lists the available bearings
    // at an intersection
    std::shared_ptr<util::RangeTable<16, true>> m_bearing_ranges_table;
    util::ShM<DiscreteBearing, true>::vector m_bearing_values_table;

    void LoadChecksum()
    {
        m_check_sum = *data_layout->GetBlockPtr<unsigned>(shared_memory,
                                                          storage::SharedDataLayout::HSGR_CHECKSUM);
        util::SimpleLogger().Write() << "set checksum: " << m_check_sum;
    }

    void LoadProfileProperties()
    {
        m_profile_properties = data_layout->GetBlockPtr<extractor::ProfileProperties>(
            shared_memory, storage::SharedDataLayout::PROPERTIES);
    }

    void LoadTimestamp()
    {
        auto timestamp_ptr =
            data_layout->GetBlockPtr<char>(shared_memory, storage::SharedDataLayout::TIMESTAMP);
        m_timestamp.resize(data_layout->GetBlockSize(storage::SharedDataLayout::TIMESTAMP));
        std::copy(timestamp_ptr,
                  timestamp_ptr + data_layout->GetBlockSize(storage::SharedDataLayout::TIMESTAMP),
                  m_timestamp.begin());
    }

    void LoadRTree()
    {
        BOOST_ASSERT_MSG(!m_coordinate_list.empty(), "coordinates must be loaded before r-tree");

        const auto file_index_ptr = data_layout->GetBlockPtr<char>(
            shared_memory, storage::SharedDataLayout::FILE_INDEX_PATH);
        file_index_path = boost::filesystem::path(file_index_ptr);
        if (!boost::filesystem::exists(file_index_path))
        {
            util::SimpleLogger().Write(logDEBUG) << "Leaf file name " << file_index_path.string();
            throw util::exception("Could not load " + file_index_path.string() +
                                  "Is any data loaded into shared memory?");
        }

        auto tree_ptr = data_layout->GetBlockPtr<RTreeNode>(
            shared_memory, storage::SharedDataLayout::R_SEARCH_TREE);
        m_static_rtree.reset(
            new SharedRTree(tree_ptr,
                            data_layout->num_entries[storage::SharedDataLayout::R_SEARCH_TREE],
                            file_index_path,
                            m_coordinate_list));
        m_geospatial_query.reset(
            new SharedGeospatialQuery(*m_static_rtree, m_coordinate_list, *this));
    }

    void LoadGraph()
    {
        auto graph_nodes_ptr = data_layout->GetBlockPtr<GraphNode>(
            shared_memory, storage::SharedDataLayout::GRAPH_NODE_LIST);

        auto graph_edges_ptr = data_layout->GetBlockPtr<GraphEdge>(
            shared_memory, storage::SharedDataLayout::GRAPH_EDGE_LIST);

        util::ShM<GraphNode, true>::vector node_list(
            graph_nodes_ptr, data_layout->num_entries[storage::SharedDataLayout::GRAPH_NODE_LIST]);
        util::ShM<GraphEdge, true>::vector edge_list(
            graph_edges_ptr, data_layout->num_entries[storage::SharedDataLayout::GRAPH_EDGE_LIST]);
        m_query_graph.reset(new QueryGraph(node_list, edge_list));
    }

    void LoadNodeAndEdgeInformation()
    {
        const auto coordinate_list_ptr = data_layout->GetBlockPtr<util::Coordinate>(
            shared_memory, storage::SharedDataLayout::COORDINATE_LIST);
        m_coordinate_list.reset(
            coordinate_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::COORDINATE_LIST]);

        for (unsigned i = 0; i < m_coordinate_list.size(); ++i)
        {
            BOOST_ASSERT(GetCoordinateOfNode(i).IsValid());
        }

        const auto osmnodeid_list_ptr = data_layout->GetBlockPtr<std::uint64_t>(
            shared_memory, storage::SharedDataLayout::OSM_NODE_ID_LIST);
        m_osmnodeid_list.reset(
            osmnodeid_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::OSM_NODE_ID_LIST]);
        // We (ab)use the number of coordinates here because we know we have the same amount of ids
        m_osmnodeid_list.set_number_of_entries(
            data_layout->num_entries[storage::SharedDataLayout::COORDINATE_LIST]);

        const auto travel_mode_list_ptr = data_layout->GetBlockPtr<extractor::TravelMode>(
            shared_memory, storage::SharedDataLayout::TRAVEL_MODE);
        util::ShM<extractor::TravelMode, true>::vector travel_mode_list(
            travel_mode_list_ptr, data_layout->num_entries[storage::SharedDataLayout::TRAVEL_MODE]);
        m_travel_mode_list = std::move(travel_mode_list);

        const auto lane_data_id_ptr = data_layout->GetBlockPtr<LaneDataID>(
            shared_memory, storage::SharedDataLayout::LANE_DATA_ID);
        util::ShM<LaneDataID, true>::vector lane_data_id(
            lane_data_id_ptr, data_layout->num_entries[storage::SharedDataLayout::LANE_DATA_ID]);
        m_lane_data_id = std::move(lane_data_id);

        const auto lane_tupel_id_pair_ptr =
            data_layout->GetBlockPtr<util::guidance::LaneTupleIdPair>(
                shared_memory, storage::SharedDataLayout::TURN_LANE_DATA);
        util::ShM<util::guidance::LaneTupleIdPair, true>::vector lane_tupel_id_pair(
            lane_tupel_id_pair_ptr,
            data_layout->num_entries[storage::SharedDataLayout::TURN_LANE_DATA]);
        m_lane_tupel_id_pairs = std::move(lane_tupel_id_pair);

        const auto turn_instruction_list_ptr =
            data_layout->GetBlockPtr<extractor::guidance::TurnInstruction>(
                shared_memory, storage::SharedDataLayout::TURN_INSTRUCTION);
        util::ShM<extractor::guidance::TurnInstruction, true>::vector turn_instruction_list(
            turn_instruction_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::TURN_INSTRUCTION]);
        m_turn_instruction_list = std::move(turn_instruction_list);

        const auto name_id_list_ptr = data_layout->GetBlockPtr<unsigned>(
            shared_memory, storage::SharedDataLayout::NAME_ID_LIST);
        util::ShM<unsigned, true>::vector name_id_list(
            name_id_list_ptr, data_layout->num_entries[storage::SharedDataLayout::NAME_ID_LIST]);
        m_name_ID_list = std::move(name_id_list);

        const auto entry_class_id_list_ptr = data_layout->GetBlockPtr<EntryClassID>(
            shared_memory, storage::SharedDataLayout::ENTRY_CLASSID);
        typename util::ShM<EntryClassID, true>::vector entry_class_id_list(
            entry_class_id_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::ENTRY_CLASSID]);
        m_entry_class_id_list = std::move(entry_class_id_list);

        const auto pre_turn_bearing_ptr = data_layout->GetBlockPtr<util::guidance::TurnBearing>(
            shared_memory, storage::SharedDataLayout::PRE_TURN_BEARING);
        typename util::ShM<util::guidance::TurnBearing, true>::vector pre_turn_bearing(
            pre_turn_bearing_ptr,
            data_layout->num_entries[storage::SharedDataLayout::PRE_TURN_BEARING]);
        m_pre_turn_bearing = std::move(pre_turn_bearing);

        const auto post_turn_bearing_ptr = data_layout->GetBlockPtr<util::guidance::TurnBearing>(
            shared_memory, storage::SharedDataLayout::POST_TURN_BEARING);
        typename util::ShM<util::guidance::TurnBearing, true>::vector post_turn_bearing(
            post_turn_bearing_ptr,
            data_layout->num_entries[storage::SharedDataLayout::POST_TURN_BEARING]);
        m_post_turn_bearing = std::move(post_turn_bearing);
    }

    void LoadViaNodeList()
    {
        auto via_geometry_list_ptr = data_layout->GetBlockPtr<GeometryID>(
            shared_memory, storage::SharedDataLayout::VIA_NODE_LIST);
        util::ShM<GeometryID, true>::vector via_geometry_list(
            via_geometry_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::VIA_NODE_LIST]);
        m_via_geometry_list = std::move(via_geometry_list);
    }

    void LoadNames()
    {
        auto offsets_ptr = data_layout->GetBlockPtr<unsigned>(
            shared_memory, storage::SharedDataLayout::NAME_OFFSETS);
        auto blocks_ptr = data_layout->GetBlockPtr<IndexBlock>(
            shared_memory, storage::SharedDataLayout::NAME_BLOCKS);
        util::ShM<unsigned, true>::vector name_offsets(
            offsets_ptr, data_layout->num_entries[storage::SharedDataLayout::NAME_OFFSETS]);
        util::ShM<IndexBlock, true>::vector name_blocks(
            blocks_ptr, data_layout->num_entries[storage::SharedDataLayout::NAME_BLOCKS]);

        auto names_list_ptr = data_layout->GetBlockPtr<char>(
            shared_memory, storage::SharedDataLayout::NAME_CHAR_LIST);
        util::ShM<char, true>::vector names_char_list(
            names_list_ptr, data_layout->num_entries[storage::SharedDataLayout::NAME_CHAR_LIST]);
        m_name_table = std::make_unique<util::RangeTable<16, true>>(
            name_offsets, name_blocks, static_cast<unsigned>(names_char_list.size()));

        m_names_char_list = std::move(names_char_list);
    }

    void LoadTurnLaneDescriptions()
    {
        auto offsets_ptr = data_layout->GetBlockPtr<std::uint32_t>(
            shared_memory, storage::SharedDataLayout::LANE_DESCRIPTION_OFFSETS);
        util::ShM<std::uint32_t, true>::vector offsets(
            offsets_ptr,
            data_layout->num_entries[storage::SharedDataLayout::LANE_DESCRIPTION_OFFSETS]);
        m_lane_description_offsets = std::move(offsets);

        auto masks_ptr = data_layout->GetBlockPtr<extractor::guidance::TurnLaneType::Mask>(
            shared_memory, storage::SharedDataLayout::LANE_DESCRIPTION_MASKS);

        util::ShM<extractor::guidance::TurnLaneType::Mask, true>::vector masks(
            masks_ptr, data_layout->num_entries[storage::SharedDataLayout::LANE_DESCRIPTION_MASKS]);
        m_lane_description_masks = std::move(masks);
    }

    void LoadCoreInformation()
    {
        auto core_marker_ptr = data_layout->GetBlockPtr<unsigned>(
            shared_memory, storage::SharedDataLayout::CORE_MARKER);
        util::ShM<bool, true>::vector is_core_node(
            core_marker_ptr, data_layout->num_entries[storage::SharedDataLayout::CORE_MARKER]);
        m_is_core_node = std::move(is_core_node);
    }

    void LoadGeometries()
    {
        auto geometries_index_ptr = data_layout->GetBlockPtr<unsigned>(
            shared_memory, storage::SharedDataLayout::GEOMETRIES_INDEX);
        util::ShM<unsigned, true>::vector geometry_begin_indices(
            geometries_index_ptr,
            data_layout->num_entries[storage::SharedDataLayout::GEOMETRIES_INDEX]);
        m_geometry_indices = std::move(geometry_begin_indices);

        auto geometries_node_list_ptr =
            data_layout->GetBlockPtr<NodeID>(
                shared_memory, storage::SharedDataLayout::GEOMETRIES_NODE_LIST);
        util::ShM<NodeID, true>::vector geometry_node_list(
            geometries_node_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::GEOMETRIES_NODE_LIST]);
        m_geometry_node_list = std::move(geometry_node_list);

        auto geometries_fwd_weight_list_ptr =
            data_layout->GetBlockPtr<EdgeWeight>(
                shared_memory, storage::SharedDataLayout::GEOMETRIES_FWD_WEIGHT_LIST);
        util::ShM<EdgeWeight, true>::vector geometry_fwd_weight_list(
            geometries_fwd_weight_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::GEOMETRIES_FWD_WEIGHT_LIST]);
        m_geometry_fwd_weight_list = std::move(geometry_fwd_weight_list);

        auto geometries_rev_weight_list_ptr =
            data_layout->GetBlockPtr<EdgeWeight>(
                shared_memory, storage::SharedDataLayout::GEOMETRIES_REV_WEIGHT_LIST);
        util::ShM<EdgeWeight, true>::vector geometry_rev_weight_list(
            geometries_rev_weight_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::GEOMETRIES_REV_WEIGHT_LIST]);
        m_geometry_rev_weight_list = std::move(geometry_rev_weight_list);

        auto datasources_list_ptr = data_layout->GetBlockPtr<uint8_t>(
            shared_memory, storage::SharedDataLayout::DATASOURCES_LIST);
        util::ShM<uint8_t, true>::vector datasources_list(
            datasources_list_ptr,
            data_layout->num_entries[storage::SharedDataLayout::DATASOURCES_LIST]);
        m_datasource_list = std::move(datasources_list);

        auto datasource_name_data_ptr = data_layout->GetBlockPtr<char>(
            shared_memory, storage::SharedDataLayout::DATASOURCE_NAME_DATA);
        util::ShM<char, true>::vector datasource_name_data(
            datasource_name_data_ptr,
            data_layout->num_entries[storage::SharedDataLayout::DATASOURCE_NAME_DATA]);
        m_datasource_name_data = std::move(datasource_name_data);

        auto datasource_name_offsets_ptr = data_layout->GetBlockPtr<std::size_t>(
            shared_memory, storage::SharedDataLayout::DATASOURCE_NAME_OFFSETS);
        util::ShM<std::size_t, true>::vector datasource_name_offsets(
            datasource_name_offsets_ptr,
            data_layout->num_entries[storage::SharedDataLayout::DATASOURCE_NAME_OFFSETS]);
        m_datasource_name_offsets = std::move(datasource_name_offsets);

        auto datasource_name_lengths_ptr = data_layout->GetBlockPtr<std::size_t>(
            shared_memory, storage::SharedDataLayout::DATASOURCE_NAME_LENGTHS);
        util::ShM<std::size_t, true>::vector datasource_name_lengths(
            datasource_name_lengths_ptr,
            data_layout->num_entries[storage::SharedDataLayout::DATASOURCE_NAME_LENGTHS]);
        m_datasource_name_lengths = std::move(datasource_name_lengths);
    }

    void LoadIntersectionClasses()
    {
        auto bearing_class_id_ptr = data_layout->GetBlockPtr<BearingClassID>(
            shared_memory, storage::SharedDataLayout::BEARING_CLASSID);
        typename util::ShM<BearingClassID, true>::vector bearing_class_id_table(
            bearing_class_id_ptr,
            data_layout->num_entries[storage::SharedDataLayout::BEARING_CLASSID]);
        m_bearing_class_id_table = std::move(bearing_class_id_table);

        auto bearing_class_ptr = data_layout->GetBlockPtr<DiscreteBearing>(
            shared_memory, storage::SharedDataLayout::BEARING_VALUES);
        typename util::ShM<DiscreteBearing, true>::vector bearing_class_table(
            bearing_class_ptr, data_layout->num_entries[storage::SharedDataLayout::BEARING_VALUES]);
        m_bearing_values_table = std::move(bearing_class_table);

        auto offsets_ptr = data_layout->GetBlockPtr<unsigned>(
            shared_memory, storage::SharedDataLayout::BEARING_OFFSETS);
        auto blocks_ptr = data_layout->GetBlockPtr<IndexBlock>(
            shared_memory, storage::SharedDataLayout::BEARING_BLOCKS);
        util::ShM<unsigned, true>::vector bearing_offsets(
            offsets_ptr, data_layout->num_entries[storage::SharedDataLayout::BEARING_OFFSETS]);
        util::ShM<IndexBlock, true>::vector bearing_blocks(
            blocks_ptr, data_layout->num_entries[storage::SharedDataLayout::BEARING_BLOCKS]);

        m_bearing_ranges_table = std::make_unique<util::RangeTable<16, true>>(
            bearing_offsets, bearing_blocks, static_cast<unsigned>(m_bearing_values_table.size()));

        auto entry_class_ptr = data_layout->GetBlockPtr<util::guidance::EntryClass>(
            shared_memory, storage::SharedDataLayout::ENTRY_CLASS);
        typename util::ShM<util::guidance::EntryClass, true>::vector entry_class_table(
            entry_class_ptr, data_layout->num_entries[storage::SharedDataLayout::ENTRY_CLASS]);
        m_entry_class_table = std::move(entry_class_table);
    }

  public:

    // this function handle the deallocation of the shared memory it we can prove it will not be used anymore
    virtual ~SharedDataFacade()
    {
        boost::interprocess::scoped_lock<boost::interprocess::named_sharable_mutex>
            exclusive_lock(data_region == storage::DATA_1 ? shared_barriers->regions_1_mutex
                                                          : shared_barriers->regions_2_mutex,
                                                          boost::interprocess::defer_lock);

        // if this returns false this is still in use
        if (exclusive_lock.try_lock())
        {
            // Now check if this is still the newest dataset
            const boost::interprocess::sharable_lock<boost::interprocess::named_upgradable_mutex> lock(
                shared_barriers->current_regions_mutex);

            auto shared_regions = storage::makeSharedMemory(storage::CURRENT_REGIONS);
            const auto current_timestamp =
                static_cast<const storage::SharedDataTimestamp *>(shared_regions->Ptr());

            if (current_timestamp->timestamp == shared_timestamp)
            {
                util::SimpleLogger().Write(logDEBUG) << "Retaining data with shared timestamp " << shared_timestamp;
            }
            else
            {
                storage::SharedMemory::Remove(data_region);
                storage::SharedMemory::Remove(layout_region);
            }
        }
    }

    SharedDataFacade(const std::shared_ptr<storage::SharedBarriers> &shared_barriers_,
                         storage::SharedDataType layout_region_,
                     storage::SharedDataType data_region_,
                     unsigned shared_timestamp_)
        : shared_barriers(shared_barriers_), layout_region(layout_region_),
          data_region(data_region_), shared_timestamp(shared_timestamp_)
    {
        util::SimpleLogger().Write(logDEBUG) << "Loading new data with shared timestamp "
                                             << shared_timestamp;

        BOOST_ASSERT(storage::SharedMemory::RegionExists(layout_region));
        m_layout_memory = storage::makeSharedMemory(layout_region);

        data_layout = static_cast<storage::SharedDataLayout *>(m_layout_memory->Ptr());

        BOOST_ASSERT(storage::SharedMemory::RegionExists(data_region));
        m_large_memory = storage::makeSharedMemory(data_region);
        shared_memory = (char *)(m_large_memory->Ptr());

        LoadGraph();
        LoadChecksum();
        LoadNodeAndEdgeInformation();
        LoadGeometries();
        LoadTimestamp();
        LoadViaNodeList();
        LoadNames();
        LoadTurnLaneDescriptions();
        LoadCoreInformation();
        LoadProfileProperties();
        LoadRTree();
        LoadIntersectionClasses();
    }

    // search graph access
    unsigned GetNumberOfNodes() const override final { return m_query_graph->GetNumberOfNodes(); }

    unsigned GetNumberOfEdges() const override final { return m_query_graph->GetNumberOfEdges(); }

    unsigned GetOutDegree(const NodeID n) const override final
    {
        return m_query_graph->GetOutDegree(n);
    }

    NodeID GetTarget(const EdgeID e) const override final { return m_query_graph->GetTarget(e); }

    EdgeData &GetEdgeData(const EdgeID e) const override final
    {
        return m_query_graph->GetEdgeData(e);
    }

    EdgeID BeginEdges(const NodeID n) const override final { return m_query_graph->BeginEdges(n); }

    EdgeID EndEdges(const NodeID n) const override final { return m_query_graph->EndEdges(n); }

    EdgeRange GetAdjacentEdgeRange(const NodeID node) const override final
    {
        return m_query_graph->GetAdjacentEdgeRange(node);
    }

    // searches for a specific edge
    EdgeID FindEdge(const NodeID from, const NodeID to) const override final
    {
        return m_query_graph->FindEdge(from, to);
    }

    EdgeID FindEdgeInEitherDirection(const NodeID from, const NodeID to) const override final
    {
        return m_query_graph->FindEdgeInEitherDirection(from, to);
    }

    EdgeID
    FindEdgeIndicateIfReverse(const NodeID from, const NodeID to, bool &result) const override final
    {
        return m_query_graph->FindEdgeIndicateIfReverse(from, to, result);
    }

    EdgeID FindSmallestEdge(const NodeID from,
                            const NodeID to,
                            std::function<bool(EdgeData)> filter) const override final
    {
        return m_query_graph->FindSmallestEdge(from, to, filter);
    }

    // node and edge information access
    util::Coordinate GetCoordinateOfNode(const NodeID id) const override final
    {
        return m_coordinate_list[id];
    }

    OSMNodeID GetOSMNodeIDOfNode(const unsigned id) const override final
    {
        return m_osmnodeid_list.at(id);
    }

    virtual std::vector<NodeID> GetUncompressedForwardGeometry(const EdgeID id) const override final
    {
        /*
         * NodeID's for geometries are stored in one place for
         * both forward and reverse segments along the same bi-
         * directional edge. The m_geometry_indices stores
         * refences to where to find the beginning of the bi-
         * directional edge in the m_geometry_node_list vector. For
         * forward geometries of bi-directional edges, edges 2 to
         * n of that edge need to be read.
         */
        const unsigned begin = m_geometry_indices.at(id);
        const unsigned end = m_geometry_indices.at(id + 1);

        std::vector<NodeID> result_nodes;

        result_nodes.reserve(end - begin);

        std::for_each(m_geometry_node_list.begin() + begin,
                      m_geometry_node_list.begin() + end,
                      [&](const NodeID &node_id) {
                          result_nodes.emplace_back(node_id);
                      });

        return result_nodes;
    }

    virtual std::vector<NodeID> GetUncompressedReverseGeometry(const EdgeID id) const override final
    {
        /*
         * NodeID's for geometries are stored in one place for
         * both forward and reverse segments along the same bi-
         * directional edge. The m_geometry_indices stores
         * refences to where to find the beginning of the bi-
         * directional edge in the m_geometry_node_list vector.
         * */
        const signed begin = m_geometry_indices.at(id);
        const signed end = m_geometry_indices.at(id + 1);

        std::vector<NodeID> result_nodes;

        result_nodes.reserve(end - begin);

        std::for_each(m_geometry_node_list.rbegin() + (m_geometry_node_list.size() - end),
                      m_geometry_node_list.rbegin() + (m_geometry_node_list.size() - begin),
                      [&](const NodeID &node_id) {
                          result_nodes.emplace_back(node_id);
                      });

        return result_nodes;
    }

    virtual std::vector<EdgeWeight>
    GetUncompressedForwardWeights(const EdgeID id) const override final
    {
        /*
         * EdgeWeights's for geometries are stored in one place for
         * both forward and reverse segments along the same bi-
         * directional edge. The m_geometry_indices stores
         * refences to where to find the beginning of the bi-
         * directional edge in the m_geometry_fwd_weight_list vector.
         * */
        const unsigned begin = m_geometry_indices.at(id) + 1;
        const unsigned end = m_geometry_indices.at(id + 1);

        std::vector<EdgeWeight> result_weights;
        result_weights.reserve(end - begin);

        std::for_each(m_geometry_fwd_weight_list.begin() + begin,
                      m_geometry_fwd_weight_list.begin() + end,
                      [&](const EdgeWeight &forward_weight) {
                          result_weights.emplace_back(forward_weight);
                      });

        return result_weights;
    }

    virtual std::vector<EdgeWeight>
    GetUncompressedReverseWeights(const EdgeID id) const override final
    {
        /*
         * EdgeWeights for geometries are stored in one place for
         * both forward and reverse segments along the same bi-
         * directional edge. The m_geometry_indices stores
         * refences to where to find the beginning of the bi-
         * directional edge in the m_geometry_rev_weight_list vector. For
         * reverse weights of bi-directional edges, edges 1 to
         * n-1 of that edge need to be read in reverse.
         */
        const signed begin = m_geometry_indices.at(id);
        const signed end = m_geometry_indices.at(id + 1) - 1;

        std::vector<EdgeWeight> result_weights;
        result_weights.reserve(end - begin);

        std::for_each(m_geometry_rev_weight_list.rbegin() + (m_geometry_rev_weight_list.size() - end),
                      m_geometry_rev_weight_list.rbegin() + (m_geometry_rev_weight_list.size() - begin),
                      [&](const EdgeWeight &reverse_weight) {
                          result_weights.emplace_back(reverse_weight);
                      });

        return result_weights;
    }

    virtual GeometryID GetGeometryIndexForEdgeID(const unsigned id) const override final
    {
        return m_via_geometry_list.at(id);
    }

    extractor::guidance::TurnInstruction
    GetTurnInstructionForEdgeID(const unsigned id) const override final
    {
        return m_turn_instruction_list.at(id);
    }

    extractor::TravelMode GetTravelModeForEdgeID(const unsigned id) const override final
    {
        return m_travel_mode_list.at(id);
    }

    std::vector<RTreeLeaf> GetEdgesInBox(const util::Coordinate south_west,
                                         const util::Coordinate north_east) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());
        const util::RectangleInt2D bbox{
            south_west.lon, north_east.lon, south_west.lat, north_east.lat};
        return m_geospatial_query->Search(bbox);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodesInRange(const util::Coordinate input_coordinate,
                               const float max_distance) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodesInRange(input_coordinate, max_distance);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodesInRange(const util::Coordinate input_coordinate,
                               const float max_distance,
                               const int bearing,
                               const int bearing_range) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodesInRange(
            input_coordinate, max_distance, bearing, bearing_range);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodes(const util::Coordinate input_coordinate,
                        const unsigned max_results) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodes(input_coordinate, max_results);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodes(const util::Coordinate input_coordinate,
                        const unsigned max_results,
                        const double max_distance) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodes(input_coordinate, max_results, max_distance);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodes(const util::Coordinate input_coordinate,
                        const unsigned max_results,
                        const int bearing,
                        const int bearing_range) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodes(
            input_coordinate, max_results, bearing, bearing_range);
    }

    std::vector<PhantomNodeWithDistance>
    NearestPhantomNodes(const util::Coordinate input_coordinate,
                        const unsigned max_results,
                        const double max_distance,
                        const int bearing,
                        const int bearing_range) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodes(
            input_coordinate, max_results, max_distance, bearing, bearing_range);
    }

    std::pair<PhantomNode, PhantomNode> NearestPhantomNodeWithAlternativeFromBigComponent(
        const util::Coordinate input_coordinate) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodeWithAlternativeFromBigComponent(
            input_coordinate);
    }

    std::pair<PhantomNode, PhantomNode> NearestPhantomNodeWithAlternativeFromBigComponent(
        const util::Coordinate input_coordinate, const double max_distance) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodeWithAlternativeFromBigComponent(
            input_coordinate, max_distance);
    }

    std::pair<PhantomNode, PhantomNode>
    NearestPhantomNodeWithAlternativeFromBigComponent(const util::Coordinate input_coordinate,
                                                      const double max_distance,
                                                      const int bearing,
                                                      const int bearing_range) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodeWithAlternativeFromBigComponent(
            input_coordinate, max_distance, bearing, bearing_range);
    }

    std::pair<PhantomNode, PhantomNode>
    NearestPhantomNodeWithAlternativeFromBigComponent(const util::Coordinate input_coordinate,
                                                      const int bearing,
                                                      const int bearing_range) const override final
    {
        BOOST_ASSERT(m_geospatial_query.get());

        return m_geospatial_query->NearestPhantomNodeWithAlternativeFromBigComponent(
            input_coordinate, bearing, bearing_range);
    }

    unsigned GetCheckSum() const override final { return m_check_sum; }

    unsigned GetNameIndexFromEdgeID(const unsigned id) const override final
    {
        return m_name_ID_list.at(id);
    }

    std::string GetNameForID(const unsigned name_id) const override final
    {
        if (std::numeric_limits<unsigned>::max() == name_id)
        {
            return "";
        }
        auto range = m_name_table->GetRange(name_id);

        std::string result;
        result.reserve(range.size());
        if (range.begin() != range.end())
        {
            result.resize(range.back() - range.front() + 1);
            std::copy(m_names_char_list.begin() + range.front(),
                      m_names_char_list.begin() + range.back() + 1,
                      result.begin());
        }
        return result;
    }

    std::string GetRefForID(const unsigned name_id) const override final
    {
        // We store the ref after the name, destination and pronunciation of a street.
        // We do this to get around the street length limit of 255 which would hit
        // if we concatenate these. Order (see extractor_callbacks):
        // name (0), destination (1), pronunciation (2), ref (3)
        return GetNameForID(name_id + 3);
    }

    std::string GetPronunciationForID(const unsigned name_id) const override final
    {
        // We store the pronunciation after the name and destination of a street.
        // We do this to get around the street length limit of 255 which would hit
        // if we concatenate these. Order (see extractor_callbacks):
        // name (0), destination (1), pronunciation (2), ref (3)
        return GetNameForID(name_id + 2);
    }

    std::string GetDestinationsForID(const unsigned name_id) const override final
    {
        // We store the destination after the name of a street.
        // We do this to get around the street length limit of 255 which would hit
        // if we concatenate these. Order (see extractor_callbacks):
        // name (0), destination (1), pronunciation (2), ref (3)
        return GetNameForID(name_id + 1);
    }

    bool IsCoreNode(const NodeID id) const override final
    {
        if (m_is_core_node.size() > 0)
        {
            return m_is_core_node.at(id);
        }

        return false;
    }

    virtual std::size_t GetCoreSize() const override final { return m_is_core_node.size(); }

    // Returns the data source ids that were used to supply the edge
    // weights.
    virtual std::vector<uint8_t>
    GetUncompressedForwardDatasources(const EdgeID id) const override final
    {
        /*
         * Data sources for geometries are stored in one place for
         * both forward and reverse segments along the same bi-
         * directional edge. The m_geometry_indices stores
         * refences to where to find the beginning of the bi-
         * directional edge in the m_geometry_list vector. For
         * forward datasources of bi-directional edges, edges 2 to
         * n of that edge need to be read.
         */
        const unsigned begin = m_geometry_indices.at(id) + 1;
        const unsigned end = m_geometry_indices.at(id + 1);

        std::vector<uint8_t> result_datasources;
        result_datasources.reserve(end - begin);

        // If there was no datasource info, return an array of 0's.
        if (m_datasource_list.empty())
        {
            for (unsigned i = 0; i < end - begin; ++i)
            {
                result_datasources.push_back(0);
            }
        }
        else
        {
            std::for_each(
                m_datasource_list.begin() + begin,
                m_datasource_list.begin() + end,
                [&](const uint8_t &datasource_id) { result_datasources.push_back(datasource_id); });
        }

        return result_datasources;
    }

    // Returns the data source ids that were used to supply the edge
    // weights.
    virtual std::vector<uint8_t>
    GetUncompressedReverseDatasources(const EdgeID id) const override final
    {
        /*
         * Datasources for geometries are stored in one place for
         * both forward and reverse segments along the same bi-
         * directional edge. The m_geometry_indices stores
         * refences to where to find the beginning of the bi-
         * directional edge in the m_geometry_list vector. For
         * reverse datasources of bi-directional edges, edges 1 to
         * n-1 of that edge need to be read in reverse.
         */
        const unsigned begin = m_geometry_indices.at(id);
        const unsigned end = m_geometry_indices.at(id + 1) - 1;

        std::vector<uint8_t> result_datasources;
        result_datasources.reserve(end - begin);

        // If there was no datasource info, return an array of 0's.
        if (m_datasource_list.empty())
        {
            for (unsigned i = 0; i < end - begin; ++i)
            {
                result_datasources.push_back(0);
            }
        }
        else
        {
            std::for_each(
                m_datasource_list.rbegin() + (m_datasource_list.size() - end),
                m_datasource_list.rbegin() + (m_datasource_list.size() - begin),
                [&](const uint8_t &datasource_id) { result_datasources.push_back(datasource_id); });
        }

        return result_datasources;
    }

    virtual std::string GetDatasourceName(const uint8_t datasource_name_id) const override final
    {
        BOOST_ASSERT(m_datasource_name_offsets.size() >= 1);
        BOOST_ASSERT(m_datasource_name_offsets.size() > datasource_name_id);

        std::string result;
        result.reserve(m_datasource_name_lengths[datasource_name_id]);
        std::copy(m_datasource_name_data.begin() + m_datasource_name_offsets[datasource_name_id],
                  m_datasource_name_data.begin() + m_datasource_name_offsets[datasource_name_id] +
                      m_datasource_name_lengths[datasource_name_id],
                  std::back_inserter(result));

        return result;
    }

    std::string GetTimestamp() const override final { return m_timestamp; }

    bool GetContinueStraightDefault() const override final
    {
        return m_profile_properties->continue_straight_at_waypoint;
    }

    BearingClassID GetBearingClassID(const NodeID id) const override final
    {
        return m_bearing_class_id_table.at(id);
    }

    util::guidance::BearingClass
    GetBearingClass(const BearingClassID bearing_class_id) const override final
    {
        BOOST_ASSERT(bearing_class_id != INVALID_BEARING_CLASSID);
        auto range = m_bearing_ranges_table->GetRange(bearing_class_id);
        util::guidance::BearingClass result;
        for (auto itr = m_bearing_values_table.begin() + range.front();
             itr != m_bearing_values_table.begin() + range.back() + 1;
             ++itr)
            result.add(*itr);
        return result;
    }

    EntryClassID GetEntryClassID(const EdgeID eid) const override final
    {
        return m_entry_class_id_list.at(eid);
    }

    util::guidance::TurnBearing PreTurnBearing(const EdgeID eid) const override final
    {
        return m_pre_turn_bearing.at(eid);
    }
    util::guidance::TurnBearing PostTurnBearing(const EdgeID eid) const override final
    {
        return m_post_turn_bearing.at(eid);
    }

    util::guidance::EntryClass GetEntryClass(const EntryClassID entry_class_id) const override final
    {
        return m_entry_class_table.at(entry_class_id);
    }

    bool hasLaneData(const EdgeID id) const override final
    {
        return INVALID_LANE_DATAID != m_lane_data_id.at(id);
    }

    util::guidance::LaneTupleIdPair GetLaneData(const EdgeID id) const override final
    {
        BOOST_ASSERT(hasLaneData(id));
        return m_lane_tupel_id_pairs.at(m_lane_data_id.at(id));
    }

    extractor::guidance::TurnLaneDescription
    GetTurnDescription(const LaneDescriptionID lane_description_id) const override final
    {
        if (lane_description_id == INVALID_LANE_DESCRIPTIONID)
            return {};
        else
            return extractor::guidance::TurnLaneDescription(
                m_lane_description_masks.begin() + m_lane_description_offsets[lane_description_id],
                m_lane_description_masks.begin() +
                    m_lane_description_offsets[lane_description_id + 1]);
    }
};
}
}
}

#endif // SHARED_DATAFACADE_HPP
