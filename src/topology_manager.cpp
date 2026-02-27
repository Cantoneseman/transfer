#include "topology_manager.hpp"

#include <queue>
#include <unordered_set>
#include <stdexcept>

namespace hyper {

// ============================================================================
// Cost Calculation
// ============================================================================

double TopologyManager::calculate_cost(const LinkMetric& metric) const {
    // Handle edge case: zero or negative bandwidth
    if (metric.bandwidth_mbps <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    
    // Base cost formula: prioritizes high bandwidth and low latency
    // Lower cost = better path
    double cost = (10000.0 / metric.bandwidth_mbps) + metric.latency_ms;
    
    // Optional: Apply packet loss penalty
    // Loss rate of 0.1 (10%) would multiply cost by ~1.11
    // Loss rate of 0.5 (50%) would multiply cost by 2.0
    if (metric.packet_loss_rate > 0.0 && metric.packet_loss_rate < 1.0) {
        cost *= (1.0 / (1.0 - metric.packet_loss_rate));
    } else if (metric.packet_loss_rate >= 1.0) {
        // Complete packet loss = unusable link
        return std::numeric_limits<double>::infinity();
    }
    
    return cost;
}

// ============================================================================
// Link Management
// ============================================================================

void TopologyManager::add_link(const std::string& src_id,
                               const std::string& dst_id,
                               const LinkMetric& metric) {
    // Ensure source node exists in adjacency list
    if (adjacency_list_.find(src_id) == adjacency_list_.end()) {
        adjacency_list_[src_id] = std::vector<Neighbor>();
    }
    
    // Ensure destination node exists (even if it has no outgoing edges)
    if (adjacency_list_.find(dst_id) == adjacency_list_.end()) {
        adjacency_list_[dst_id] = std::vector<Neighbor>();
    }
    
    // Check if link already exists and update it
    auto& neighbors = adjacency_list_[src_id];
    for (auto& neighbor : neighbors) {
        if (neighbor.node_id == dst_id) {
            // Update existing link
            neighbor.metric = metric;
            return;
        }
    }
    
    // Add new neighbor
    neighbors.emplace_back(dst_id, metric);
}

void TopologyManager::add_bidirectional_link(const std::string& node_a,
                                             const std::string& node_b,
                                             const LinkMetric& metric) {
    add_link(node_a, node_b, metric);
    add_link(node_b, node_a, metric);
}

// ============================================================================
// Path Finding - Dijkstra's Algorithm
// ============================================================================

NetworkPath TopologyManager::find_best_path(const std::string& src_id,
                                            const std::string& dst_id) const {
    // Check if source and destination exist
    if (adjacency_list_.find(src_id) == adjacency_list_.end() ||
        adjacency_list_.find(dst_id) == adjacency_list_.end()) {
        // Return invalid path with cost -1 for unreachable
        NetworkPath invalid_path;
        invalid_path.total_cost = -1.0;
        return invalid_path;
    }
    
    // Special case: source == destination
    if (src_id == dst_id) {
        return NetworkPath({src_id}, 0.0);
    }
    
    // Distance map: node_id -> minimum cost to reach from source
    std::unordered_map<std::string, double> distances;
    
    // Predecessor map: node_id -> previous node in optimal path
    std::unordered_map<std::string, std::string> predecessors;
    
    // Visited set
    std::unordered_set<std::string> visited;
    
    // Priority queue: (cost, node_id) - min-heap by cost
    using PQElement = std::pair<double, std::string>;
    std::priority_queue<PQElement, std::vector<PQElement>, std::greater<PQElement>> pq;
    
    // Initialize distances to infinity
    for (const auto& [node_id, _] : adjacency_list_) {
        distances[node_id] = std::numeric_limits<double>::infinity();
    }
    
    // Source distance is 0
    distances[src_id] = 0.0;
    pq.emplace(0.0, src_id);
    
    // Dijkstra's main loop
    while (!pq.empty()) {
        auto [current_cost, current_node] = pq.top();
        pq.pop();
        
        // Skip if already visited (we may have duplicate entries in PQ)
        if (visited.count(current_node)) {
            continue;
        }
        visited.insert(current_node);
        
        // Early termination: found destination
        if (current_node == dst_id) {
            break;
        }
        
        // Skip if current cost is worse than known distance
        // (stale entry in priority queue)
        if (current_cost > distances[current_node]) {
            continue;
        }
        
        // Explore neighbors
        auto it = adjacency_list_.find(current_node);
        if (it == adjacency_list_.end()) {
            continue;
        }
        
        for (const auto& neighbor : it->second) {
            // Skip already visited nodes
            if (visited.count(neighbor.node_id)) {
                continue;
            }
            
            // Calculate cost to reach neighbor through current node
            double edge_cost = calculate_cost(neighbor.metric);
            double new_cost = distances[current_node] + edge_cost;
            
            // If this path is better, update
            if (new_cost < distances[neighbor.node_id]) {
                distances[neighbor.node_id] = new_cost;
                predecessors[neighbor.node_id] = current_node;
                pq.emplace(new_cost, neighbor.node_id);
            }
        }
    }
    
    // Check if destination is reachable
    if (distances[dst_id] == std::numeric_limits<double>::infinity()) {
        // Destination unreachable - return invalid path with cost -1
        NetworkPath invalid_path;
        invalid_path.total_cost = -1.0;
        return invalid_path;
    }
    
    // Reconstruct path by backtracking from destination to source
    std::vector<std::string> path;
    std::string current = dst_id;
    
    while (current != src_id) {
        path.push_back(current);
        
        auto pred_it = predecessors.find(current);
        if (pred_it == predecessors.end()) {
            // Should not happen if algorithm is correct, but safety check
            NetworkPath invalid_path;
            invalid_path.total_cost = -1.0;
            return invalid_path;
        }
        current = pred_it->second;
    }
    path.push_back(src_id);
    
    // Reverse to get path from source to destination
    std::reverse(path.begin(), path.end());
    
    return NetworkPath(path, distances[dst_id]);
}

// ============================================================================
// Utility Methods
// ============================================================================

bool TopologyManager::has_node(const std::string& node_id) const {
    return adjacency_list_.find(node_id) != adjacency_list_.end();
}

std::vector<std::string> TopologyManager::get_all_nodes() const {
    std::vector<std::string> nodes;
    nodes.reserve(adjacency_list_.size());
    
    for (const auto& [node_id, _] : adjacency_list_) {
        nodes.push_back(node_id);
    }
    
    return nodes;
}

const std::vector<Neighbor>* TopologyManager::get_neighbors(const std::string& node_id) const {
    auto it = adjacency_list_.find(node_id);
    if (it == adjacency_list_.end()) {
        return nullptr;
    }
    return &(it->second);
}

} // namespace hyper
