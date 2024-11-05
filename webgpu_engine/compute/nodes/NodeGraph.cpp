/*****************************************************************************
 * weBIGeo
 * Copyright (C) 2024 Patrick Komon
 * Copyright (C) 2024 Gerald Kimmersdorfer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include "NodeGraph.h"

#include "ComputeAvalancheInfluenceAreaNode.h"
#include "ComputeAvalancheTrajectoriesNode.h"
#include "ComputeNormalsNode.h"
#include "ComputeSnowNode.h"
#include "CreateHashMapNode.h"
#include "DownsampleTilesNode.h"
#include "RequestTilesNode.h"
#include "SelectTilesNode.h"
#include "UpsampleTexturesNode.h"
#include <QDebug>
#include <memory>

namespace webgpu_engine::compute::nodes {

GraphRunFailureInfo::GraphRunFailureInfo(const std::string& node_name, NodeRunFailureInfo node_run_failure_info)
    : m_node_name(node_name)
    , m_node_run_failure_info(node_run_failure_info)
{
}

const std::string& GraphRunFailureInfo::node_name() const { return m_node_name; }

const NodeRunFailureInfo& GraphRunFailureInfo::node_run_failure_info() const { return m_node_run_failure_info; }

Node* NodeGraph::add_node(const std::string& name, std::unique_ptr<Node> node)
{
    assert(!m_nodes.contains(name));
    m_nodes.emplace(name, std::move(node));
    return m_nodes.at(name).get();
}

Node& NodeGraph::get_node(const std::string& node_name) { return *m_nodes.at(node_name); }

const Node& NodeGraph::get_node(const std::string& node_name) const { return *m_nodes.at(node_name); }

bool NodeGraph::exists_node(const std::string& node_name) const { return m_nodes.find(node_name) != m_nodes.end(); }

const GpuHashMap<tile::Id, uint32_t, GpuTileId>& NodeGraph::output_hash_map() const { return *m_output_hash_map_ptr; }

GpuHashMap<tile::Id, uint32_t, GpuTileId>& NodeGraph::output_hash_map() { return *m_output_hash_map_ptr; }

const TileStorageTexture& NodeGraph::output_texture_storage() const { return *m_output_texture_storage_ptr; }

TileStorageTexture& NodeGraph::output_texture_storage() { return *m_output_texture_storage_ptr; }

const GpuHashMap<tile::Id, uint32_t, GpuTileId>& NodeGraph::output_hash_map_2() const { return *m_output_hash_map_ptr_2; }

GpuHashMap<tile::Id, uint32_t, GpuTileId>& NodeGraph::output_hash_map_2() { return *m_output_hash_map_ptr_2; }

const TileStorageTexture& NodeGraph::output_texture_storage_2() const { return *m_output_texture_storage_ptr_2; }

TileStorageTexture& NodeGraph::output_texture_storage_2() { return *m_output_texture_storage_ptr_2; }

void NodeGraph::connect_node_signals_and_slots()
{
    // basic idea: find topological ordering by counting in-coming edges (in-degree)
    //  1. start with nodes that have no incoming edges
    //  2. select node with 0 incoming edges
    //  3. add it to topological order
    //  4. "remove node" from graph, i.e. update in-degrees of nodes that are connected to outputs of this node
    //   -> this decreases in-degree of other nodes
    //   -> if some nodes reaches zero, add it to queue to for processing next
    //
    // known as https://en.wikipedia.org/wiki/Topological_sorting#Kahn's_algorithm

    assert(!m_nodes.empty());

    std::unordered_map<Node*, uint32_t> in_degrees;
    std::queue<Node*> node_queue;
    std::vector<Node*> topological_ordering;

    for (auto& [_, node] : m_nodes) {
        uint32_t in_degree = 0;
        for (auto& socket : node->input_sockets()) {
            if (socket.is_socket_connected()) {
                in_degree++;
            }
        }
        in_degrees[node.get()] = in_degree;
        if (in_degree == 0) {
            node_queue.push(node.get());
        }
    }

    while (!node_queue.empty()) {
        Node* node = node_queue.front();
        node_queue.pop();
        topological_ordering.push_back(node);
        for (auto& output_socket : node->output_sockets()) {
            for (auto& connected_socket : output_socket.connected_sockets()) {
                auto& connected_node = connected_socket->node();
                in_degrees[&connected_node]--;
                if (in_degrees[&connected_node] == 0) {
                    node_queue.push(&connected_node);
                }
            }
        }
    }

    for (auto& [node, in_degree] : in_degrees) {
        if (in_degree) {
            qFatal() << "cycle in node graph detected";
        }
    }

    connect(this, &NodeGraph::run_triggered, topological_ordering.front(), &Node::run);
    for (uint32_t i = 0; i < topological_ordering.size() - 1; i++) {
        connect(topological_ordering[i], &Node::run_completed, topological_ordering[i + 1], &Node::run);
    }
    connect(topological_ordering.back(), &Node::run_completed, this, &NodeGraph::run_completed); // emits run completed signal in NodeGraph

    for (auto& [_, node] : m_nodes) {
        connect(node.get(), &Node::run_failed, this, &NodeGraph::emit_graph_failure);
    }
}

void NodeGraph::run()
{
    qDebug() << "running node graph ...";
    emit run_triggered();
}

void NodeGraph::emit_graph_failure(NodeRunFailureInfo info)
{
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&info](const auto& key_value_pair) { return key_value_pair.second.get() == &info.node(); });
    assert(it != m_nodes.end());
    emit run_failed(GraphRunFailureInfo(it->first, info));
}

std::unique_ptr<NodeGraph> NodeGraph::create_normal_compute_graph(const PipelineManager& manager, WGPUDevice device)
{
    size_t capacity = 1024;
    glm::uvec2 input_resolution = { 65, 65 };
    glm::uvec2 normal_output_resolution = { 65, 65 };
    glm::uvec2 upsample_output_resolution = { 256, 256 };

    auto node_graph = std::make_unique<NodeGraph>();
    Node* tile_select_node = node_graph->add_node("select_tiles_node", std::make_unique<SelectTilesNode>());
    Node* height_request_node = node_graph->add_node("request_height_node", std::make_unique<RequestTilesNode>());
    Node* hash_map_node
        = node_graph->add_node("hashmap_node", std::make_unique<CreateHashMapNode>(device, input_resolution, capacity, WGPUTextureFormat_R16Uint));
    Node* normal_compute_node = node_graph->add_node(
        "compute_normals_node", std::make_unique<ComputeNormalsNode>(manager, device, normal_output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm));
    Node* upsample_textures_node
        = node_graph->add_node("upsample_textures_node", std::make_unique<UpsampleTexturesNode>(manager, device, upsample_output_resolution, capacity));
    DownsampleTilesNode* downsample_tiles_node
        = static_cast<DownsampleTilesNode*>(node_graph->add_node("downsample_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));

    // connect height request inputs
    tile_select_node->output_socket("tile ids").connect(height_request_node->input_socket("tile ids"));

    // connect height request inputs
    tile_select_node->output_socket("tile ids").connect(hash_map_node->input_socket("tile ids"));
    height_request_node->output_socket("tile data").connect(hash_map_node->input_socket("texture data"));

    // connect normal node inputs
    tile_select_node->output_socket("tile ids").connect(normal_compute_node->input_socket("tile ids"));
    hash_map_node->output_socket("hash map").connect(normal_compute_node->input_socket("hash map"));
    hash_map_node->output_socket("textures").connect(normal_compute_node->input_socket("height textures"));

    //  connect upsample textures node inputs
    normal_compute_node->output_socket("normal textures").connect(upsample_textures_node->input_socket("source textures"));

    // connect downsample tiles node inputs
    tile_select_node->output_socket("tile ids").connect(downsample_tiles_node->input_socket("tile ids"));
    normal_compute_node->output_socket("hash map").connect(downsample_tiles_node->input_socket("hash map"));
    normal_compute_node->output_socket("normal textures").connect(downsample_tiles_node->input_socket("textures"));

    node_graph->m_output_hash_map_ptr = &downsample_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr = &downsample_tiles_node->texture_storage();
    node_graph->m_output_hash_map_ptr_2 = &downsample_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr_2 = &downsample_tiles_node->texture_storage();

    node_graph->connect_node_signals_and_slots();

    return node_graph;
}

std::unique_ptr<NodeGraph> NodeGraph::create_normal_with_snow_compute_graph(const PipelineManager& manager, WGPUDevice device)
{
    size_t capacity = 1024;
    glm::uvec2 input_resolution = { 65, 65 };
    glm::uvec2 normal_output_resolution = { 65, 65 };
    glm::uvec2 upsample_output_resolution = { 256, 256 };

    auto node_graph = std::make_unique<NodeGraph>();
    Node* tile_select_node = node_graph->add_node("select_tiles_node", std::make_unique<SelectTilesNode>());
    Node* height_request_node = node_graph->add_node("request_height_node", std::make_unique<RequestTilesNode>());
    Node* hash_map_node
        = node_graph->add_node("create_hashmap_node", std::make_unique<CreateHashMapNode>(device, input_resolution, capacity, WGPUTextureFormat_R16Uint));
    Node* normal_compute_node = node_graph->add_node(
        "compute_normals_node", std::make_unique<ComputeNormalsNode>(manager, device, normal_output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm));
    Node* snow_compute_node = node_graph->add_node(
        "compute_snow_node", std::make_unique<ComputeSnowNode>(manager, device, normal_output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm));
    Node* upsample_textures_node
        = node_graph->add_node("upsample_textures_node", std::make_unique<UpsampleTexturesNode>(manager, device, upsample_output_resolution, capacity));
    Node* upsample_snow_textures_node
        = node_graph->add_node("upsample_snow_textures_node", std::make_unique<UpsampleTexturesNode>(manager, device, upsample_output_resolution, capacity));
    DownsampleTilesNode* downsample_snow_tiles_node
        = static_cast<DownsampleTilesNode*>(node_graph->add_node("downsample_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));
    DownsampleTilesNode* downsample_tiles_node = static_cast<DownsampleTilesNode*>(
        node_graph->add_node("downsample_snow_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));

    // connect height request node inputs
    tile_select_node->output_socket("tile ids").connect(height_request_node->input_socket("tile ids"));

    // connect hash map node inputs
    tile_select_node->output_socket("tile ids").connect(hash_map_node->input_socket("tile ids"));
    height_request_node->output_socket("tile data").connect(hash_map_node->input_socket("texture data"));

    // connect normal node inputs
    tile_select_node->output_socket("tile ids").connect(normal_compute_node->input_socket("tile ids"));
    hash_map_node->output_socket("hash map").connect(normal_compute_node->input_socket("hash map"));
    hash_map_node->output_socket("textures").connect(normal_compute_node->input_socket("height textures"));

    // connect snow compute node inputs
    tile_select_node->output_socket("tile ids").connect(snow_compute_node->input_socket("tile ids"));
    hash_map_node->output_socket("hash map").connect(snow_compute_node->input_socket("hash map"));
    hash_map_node->output_socket("textures").connect(snow_compute_node->input_socket("height textures"));

    // upscale snow texture
    snow_compute_node->output_socket("snow textures").connect(upsample_snow_textures_node->input_socket("source textures"));

    // create downsamples snow tiles
    tile_select_node->output_socket("tile ids").connect(downsample_snow_tiles_node->input_socket("tile ids"));
    snow_compute_node->output_socket("hash map").connect(downsample_snow_tiles_node->input_socket("hash map"));
    upsample_snow_textures_node->output_socket("output textures").connect(downsample_snow_tiles_node->input_socket("textures"));

    // connect upsample textures node inputs
    normal_compute_node->output_socket("normal textures").connect(upsample_textures_node->input_socket("source textures"));

    // connect downsample tiles node inputs
    tile_select_node->output_socket("tile ids").connect(downsample_tiles_node->input_socket("tile ids"));
    normal_compute_node->output_socket("hash map").connect(downsample_tiles_node->input_socket("hash map"));
    upsample_textures_node->output_socket("output textures").connect(downsample_tiles_node->input_socket("textures"));

    node_graph->m_output_hash_map_ptr = &downsample_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr = &downsample_tiles_node->texture_storage();

    node_graph->m_output_hash_map_ptr_2 = &downsample_snow_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr_2 = &downsample_snow_tiles_node->texture_storage();

    node_graph->connect_node_signals_and_slots();

    return node_graph;
}

std::unique_ptr<NodeGraph> NodeGraph::create_snow_compute_graph(const PipelineManager& manager, WGPUDevice device)
{
    size_t capacity = 256;
    glm::uvec2 input_resolution = { 65, 65 };
    glm::uvec2 output_resolution = { 65, 65 };

    auto node_graph = std::make_unique<NodeGraph>();
    Node* tile_select_node = node_graph->add_node("select_tiles_node", std::make_unique<SelectTilesNode>());
    Node* height_request_node = node_graph->add_node("request_height_node", std::make_unique<RequestTilesNode>());
    Node* hash_map_node
        = node_graph->add_node("hashmap_node", std::make_unique<CreateHashMapNode>(device, input_resolution, capacity, WGPUTextureFormat_R16Uint));
    Node* snow_compute_node = node_graph->add_node(
        "compute_snow_node", std::make_unique<ComputeSnowNode>(manager, device, output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm));
    DownsampleTilesNode* downsample_tiles_node
        = static_cast<DownsampleTilesNode*>(node_graph->add_node("downsample_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));

    tile_select_node->output_socket("tile ids").connect(height_request_node->input_socket("tile ids"));

    tile_select_node->output_socket("tile ids").connect(hash_map_node->input_socket("tile ids"));
    height_request_node->output_socket("tile data").connect(hash_map_node->input_socket("texture data"));

    tile_select_node->output_socket("tile ids").connect(snow_compute_node->input_socket("tile ids"));
    hash_map_node->output_socket("hash map").connect(snow_compute_node->input_socket("hash map"));
    hash_map_node->output_socket("textures").connect(snow_compute_node->input_socket("height textures"));

    tile_select_node->output_socket("tile ids").connect(downsample_tiles_node->input_socket("tile ids"));
    snow_compute_node->output_socket("hash map").connect(downsample_tiles_node->input_socket("hash map"));
    snow_compute_node->output_socket("snow textures").connect(downsample_tiles_node->input_socket("textures"));

    node_graph->m_output_hash_map_ptr = &downsample_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr = &downsample_tiles_node->texture_storage();

    node_graph->connect_node_signals_and_slots();

    return node_graph;
}

std::unique_ptr<NodeGraph> NodeGraph::create_avalanche_trajectories_compute_graph(const PipelineManager& manager, WGPUDevice device)
{
    size_t capacity = 1024;
    glm::uvec2 input_resolution = { 65, 65 };
    glm::uvec2 normal_output_resolution = { 65, 65 };
    glm::uvec2 trajectories_output_resolution = { 256, 256 };
    glm::uvec2 upsample_output_resolution = { 256, 256 };

    auto node_graph = std::make_unique<NodeGraph>();

    Node* target_tile_select_node = node_graph->add_node("select_target_tiles_node", std::make_unique<SelectTilesNode>());
    Node* source_tile_select_node = node_graph->add_node("select_source_tiles_node", std::make_unique<SelectTilesNode>());

    Node* height_request_node = node_graph->add_node("request_height_node", std::make_unique<RequestTilesNode>());
    Node* hash_map_node
        = node_graph->add_node("create_hashmap_node", std::make_unique<CreateHashMapNode>(device, input_resolution, capacity, WGPUTextureFormat_R16Uint));
    ComputeNormalsNode* normal_compute_node = static_cast<ComputeNormalsNode*>(node_graph->add_node(
        "compute_normals_node", std::make_unique<ComputeNormalsNode>(manager, device, normal_output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm)));
    ComputeAvalancheTrajectoriesNode* avalanche_trajectories_compute_node = static_cast<ComputeAvalancheTrajectoriesNode*>(node_graph->add_node(
        "compute_avalanche_trajectories_node", std::make_unique<ComputeAvalancheTrajectoriesNode>(manager, device, trajectories_output_resolution, capacity)));
    ComputeAvalancheTrajectoriesBufferToTextureNode* avalanche_trajectories_buffer_to_texture_compute_node
        = static_cast<ComputeAvalancheTrajectoriesBufferToTextureNode*>(node_graph->add_node("avalanche_trajectories_buffer_to_texture_compute_node",
            std::make_unique<ComputeAvalancheTrajectoriesBufferToTextureNode>(
                manager, device, trajectories_output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm)));
    Node* upsample_normals_textures_node
        = node_graph->add_node("upsample_textures_node", std::make_unique<UpsampleTexturesNode>(manager, device, upsample_output_resolution, capacity));
    DownsampleTilesNode* downsample_trajectory_tiles_node = static_cast<DownsampleTilesNode*>(
        node_graph->add_node("downsample_trajectory_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));
    DownsampleTilesNode* downsample_normals_tiles_node = static_cast<DownsampleTilesNode*>(
        node_graph->add_node("downsample_normals_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));

    // connect tile request node inputs
    source_tile_select_node->output_socket("tile ids").connect(height_request_node->input_socket("tile ids"));

    // connect hash map node inputs
    source_tile_select_node->output_socket("tile ids").connect(hash_map_node->input_socket("tile ids"));
    height_request_node->output_socket("tile data").connect(hash_map_node->input_socket("texture data"));

    // connect normal node inputs
    source_tile_select_node->output_socket("tile ids").connect(normal_compute_node->input_socket("tile ids"));
    hash_map_node->output_socket("hash map").connect(normal_compute_node->input_socket("hash map"));
    hash_map_node->output_socket("textures").connect(normal_compute_node->input_socket("height textures"));

    // connect trajectories node inputs
    target_tile_select_node->output_socket("tile ids").connect(avalanche_trajectories_compute_node->input_socket("tile ids"));
    normal_compute_node->output_socket("hash map").connect(avalanche_trajectories_compute_node->input_socket("hash map"));
    normal_compute_node->output_socket("normal textures").connect(avalanche_trajectories_compute_node->input_socket("normal textures"));
    hash_map_node->output_socket("textures").connect(avalanche_trajectories_compute_node->input_socket("height textures"));

    // connect trajectories buffer to texture node inputs
    target_tile_select_node->output_socket("tile ids").connect(avalanche_trajectories_buffer_to_texture_compute_node->input_socket("tile ids"));
    avalanche_trajectories_compute_node->output_socket("hash map").connect(avalanche_trajectories_buffer_to_texture_compute_node->input_socket("hash map"));
    avalanche_trajectories_compute_node->output_socket("storage buffer")
        .connect(avalanche_trajectories_buffer_to_texture_compute_node->input_socket("storage buffer"));

    // create downsampled area of influence tiles
    target_tile_select_node->output_socket("tile ids").connect(downsample_trajectory_tiles_node->input_socket("tile ids"));
    avalanche_trajectories_compute_node->output_socket("hash map").connect(downsample_trajectory_tiles_node->input_socket("hash map"));
    avalanche_trajectories_buffer_to_texture_compute_node->output_socket("textures").connect(downsample_trajectory_tiles_node->input_socket("textures"));

    // connect upsample textures node inputs
    normal_compute_node->output_socket("normal textures").connect(upsample_normals_textures_node->input_socket("source textures"));

    // connect downsample normal tiles node inputs
    source_tile_select_node->output_socket("tile ids").connect(downsample_normals_tiles_node->input_socket("tile ids"));
    normal_compute_node->output_socket("hash map").connect(downsample_normals_tiles_node->input_socket("hash map"));
    upsample_normals_textures_node->output_socket("output textures").connect(downsample_normals_tiles_node->input_socket("textures"));

    node_graph->m_output_hash_map_ptr = &downsample_normals_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr = &downsample_normals_tiles_node->texture_storage();

    node_graph->m_output_hash_map_ptr_2 = &downsample_trajectory_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr_2 = &downsample_trajectory_tiles_node->texture_storage();

    node_graph->connect_node_signals_and_slots();

    return node_graph;
}

std::unique_ptr<NodeGraph> NodeGraph::create_avalanche_influence_area_compute_graph(const PipelineManager& manager, WGPUDevice device)
{
    size_t capacity = 1024;
    glm::uvec2 input_resolution = { 65, 65 };
    glm::uvec2 normal_output_resolution = { 65, 65 };
    glm::uvec2 area_of_influence_output_resolution = { 256, 256 };
    glm::uvec2 upsample_output_resolution = { 256, 256 };

    auto node_graph = std::make_unique<NodeGraph>();

    Node* target_tile_select_node = node_graph->add_node("select_target_tiles_node", std::make_unique<SelectTilesNode>());
    Node* source_tile_select_node = node_graph->add_node("select_source_tiles_node", std::make_unique<SelectTilesNode>());

    Node* height_request_node = node_graph->add_node("request_height_node", std::make_unique<RequestTilesNode>());
    Node* hash_map_node
        = node_graph->add_node("create_hashmap_node", std::make_unique<CreateHashMapNode>(device, input_resolution, capacity, WGPUTextureFormat_R16Uint));
    ComputeNormalsNode* normal_compute_node = static_cast<ComputeNormalsNode*>(node_graph->add_node(
        "compute_normals_node", std::make_unique<ComputeNormalsNode>(manager, device, normal_output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm)));
    ComputeAvalancheInfluenceAreaNode* avalanche_influence_area_compute_node
        = static_cast<ComputeAvalancheInfluenceAreaNode*>(node_graph->add_node("compute_area_of_influence_node",
            std::make_unique<ComputeAvalancheInfluenceAreaNode>(manager, device, area_of_influence_output_resolution, capacity, WGPUTextureFormat_RGBA8Unorm)));
    Node* upsample_normals_textures_node
        = node_graph->add_node("upsample_textures_node", std::make_unique<UpsampleTexturesNode>(manager, device, upsample_output_resolution, capacity));
    DownsampleTilesNode* downsample_area_of_influence_tiles_node = static_cast<DownsampleTilesNode*>(
        node_graph->add_node("downsample_area_of_influence_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));
    DownsampleTilesNode* downsample_normals_tiles_node = static_cast<DownsampleTilesNode*>(
        node_graph->add_node("downsample_normals_tiles_node", std::make_unique<DownsampleTilesNode>(manager, device, capacity)));

    // connect tile request node inputs
    source_tile_select_node->output_socket("tile ids").connect(height_request_node->input_socket("tile ids"));

    // connect hash map node inputs
    source_tile_select_node->output_socket("tile ids").connect(hash_map_node->input_socket("tile ids"));
    height_request_node->output_socket("tile data").connect(hash_map_node->input_socket("texture data"));

    // connect normal node inputs
    source_tile_select_node->output_socket("tile ids").connect(normal_compute_node->input_socket("tile ids"));
    hash_map_node->output_socket("hash map").connect(normal_compute_node->input_socket("hash map"));
    hash_map_node->output_socket("textures").connect(normal_compute_node->input_socket("height textures"));

    // connect influence area compute node inputs
    target_tile_select_node->output_socket("tile ids").connect(avalanche_influence_area_compute_node->input_socket("tile ids"));
    normal_compute_node->output_socket("hash map").connect(avalanche_influence_area_compute_node->input_socket("hash map"));
    normal_compute_node->output_socket("normal textures").connect(avalanche_influence_area_compute_node->input_socket("normal textures"));
    hash_map_node->output_socket("textures").connect(avalanche_influence_area_compute_node->input_socket("height textures"));

    // create downsampled area of influence tiles
    target_tile_select_node->output_socket("tile ids").connect(downsample_area_of_influence_tiles_node->input_socket("tile ids"));
    avalanche_influence_area_compute_node->output_socket("hash map").connect(downsample_area_of_influence_tiles_node->input_socket("hash map"));
    avalanche_influence_area_compute_node->output_socket("influence area textures").connect(downsample_area_of_influence_tiles_node->input_socket("textures"));

    // connect upsample textures node inputs
    normal_compute_node->output_socket("normal textures").connect(upsample_normals_textures_node->input_socket("source textures"));

    // connect downsample normal tiles node inputs
    source_tile_select_node->output_socket("tile ids").connect(downsample_normals_tiles_node->input_socket("tile ids"));
    normal_compute_node->output_socket("hash map").connect(downsample_normals_tiles_node->input_socket("hash map"));
    upsample_normals_textures_node->output_socket("output textures").connect(downsample_normals_tiles_node->input_socket("textures"));

    node_graph->m_output_hash_map_ptr = &downsample_normals_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr = &downsample_normals_tiles_node->texture_storage();

    node_graph->m_output_hash_map_ptr_2 = &downsample_area_of_influence_tiles_node->hash_map();
    node_graph->m_output_texture_storage_ptr_2 = &downsample_area_of_influence_tiles_node->texture_storage();

    node_graph->connect_node_signals_and_slots();

    return node_graph;
}

} // namespace webgpu_engine::compute::nodes
