#ifndef TOPOLOGY_MANAGER_HPP
#define TOPOLOGY_MANAGER_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <limits>
#include <queue>
#include <algorithm>

// ============================================================================
// HYPER-TRANSFER v2.0 - Network Topology Manager
// ============================================================================
// Manages network topology graph and computes optimal transfer paths
// using Dijkstra's algorithm with custom cost metrics.
// ============================================================================

namespace hyper {

/**
 * @brief Metrics for a network link between two nodes.
 */
struct LinkMetric {
    double bandwidth_mbps;     // Bandwidth in Megabits per second
    double latency_ms;         // Latency in milliseconds
    double packet_loss_rate;   // Packet loss rate (0.0 to 1.0)
    
    LinkMetric()
        : bandwidth_mbps(1000.0)
        , latency_ms(1.0)
        , packet_loss_rate(0.0)
    {}
    
    LinkMetric(double bw, double lat, double loss = 0.0)
        : bandwidth_mbps(bw)
        , latency_ms(lat)
        , packet_loss_rate(loss)
    {}
};

/**
 * @brief Represents a path through the network.
 */
struct NetworkPath {
    std::vector<std::string> nodes;  // Ordered list of node IDs in the path
    double total_cost;               // Cumulative cost of the path
    
    NetworkPath()
        : nodes()
        , total_cost(std::numeric_limits<double>::infinity())
    {}
    
    NetworkPath(const std::vector<std::string>& path, double cost)
        : nodes(path)
        , total_cost(cost)
    {}
    
    /**
     * @brief Check if the path is valid (has at least src and dst).
     */
    bool is_valid() const {
        return nodes.size() >= 2 && 
               total_cost < std::numeric_limits<double>::infinity();
    }
    
    /**
     * @brief Get the number of hops in the path.
     */
    size_t hop_count() const {
        return nodes.empty() ? 0 : nodes.size() - 1;
    }
};

/**
 * @brief Represents a neighbor node in the adjacency list.
 */
struct Neighbor {
    std::string node_id;    // ID of the neighbor node
    LinkMetric metric;      // Metrics for the link to this neighbor
    
    Neighbor(const std::string& id, const LinkMetric& m)
        : node_id(id)
        , metric(m)
    {}
};

/**
 * @brief Manages network topology and computes optimal paths.
 * 
 * Uses an adjacency list representation for efficient graph operations
 * and Dijkstra's algorithm for shortest path computation.
 */
class TopologyManager {
public:
    TopologyManager() = default;
    ~TopologyManager() = default;
    
    // Non-copyable, movable
    TopologyManager(const TopologyManager&) = delete;
    TopologyManager& operator=(const TopologyManager&) = delete;
    TopologyManager(TopologyManager&&) = default;
    TopologyManager& operator=(TopologyManager&&) = default;
    
    /**
     * @brief Add a directed link between two nodes.
     * 
     * @param src_id   Source node ID
     * @param dst_id   Destination node ID
     * @param metric   Link metrics (bandwidth, latency, loss)
     */
    void add_link(const std::string& src_id, 
                  const std::string& dst_id, 
                  const LinkMetric& metric);
    
    /**
     * @brief Add a bidirectional link between two nodes.
     * 
     * @param node_a   First node ID
     * @param node_b   Second node ID
     * @param metric   Link metrics (same for both directions)
     */
    void add_bidirectional_link(const std::string& node_a,
                                const std::string& node_b,
                                const LinkMetric& metric);
    
    /**
     * @brief Find the best (lowest cost) path between two nodes.
     * 
     * Uses Dijkstra's algorithm with a custom cost function.
     * 
     * @param src_id   Source node ID
     * @param dst_id   Destination node ID
     * @return NetworkPath containing the optimal path and total cost.
     *         Returns invalid path if no route exists.
     */
    NetworkPath find_best_path(const std::string& src_id,
                               const std::string& dst_id) const;
    
    /**
     * @brief Check if a node exists in the topology.
     */
    bool has_node(const std::string& node_id) const;
    
    /**
     * @brief Get the number of nodes in the topology.
     */
    size_t node_count() const { return adjacency_list_.size(); }
    
    /**
     * @brief Get all node IDs in the topology.
     */
    std::vector<std::string> get_all_nodes() const;
    
    /**
     * @brief Get neighbors of a node.
     */
    const std::vector<Neighbor>* get_neighbors(const std::string& node_id) const;
    
    /**
     * @brief Clear all nodes and links.
     */
    void clear() { adjacency_list_.clear(); }
    
private:
    // Adjacency list representation of the network graph
    std::unordered_map<std::string, std::vector<Neighbor>> adjacency_list_;
    
    /**
     * @brief Calculate the cost of traversing a link.
     * 
     * Cost formula prioritizes high bandwidth and low latency:
     *   cost = (10000.0 / bandwidth_mbps) + latency_ms
     * 
     * Optional: Factor in packet loss with a penalty multiplier.
     * 
     * @param metric Link metrics
     * @return Cost value (lower is better)
     */
    double calculate_cost(const LinkMetric& metric) const;
};

} // namespace hyper

#endif // TOPOLOGY_MANAGER_HPP
