#include "topology_manager.hpp"

#include <iostream>
#include <iomanip>
#include <cassert>

// ============================================================================
// HYPER-TRANSFER v2.0 - Topology Manager Test
// ============================================================================
// Tests the routing logic with a realistic scenario:
// - Direct slow link vs. indirect fast path
// - Verifies Dijkstra's algorithm selects optimal route
// ============================================================================

using namespace hyper;

/**
 * @brief Print a NetworkPath in human-readable format.
 */
void print_path(const NetworkPath& path) {
    if (path.total_cost < 0) {
        std::cout << "Path: UNREACHABLE\n";
        return;
    }
    
    std::cout << "Path: ";
    for (size_t i = 0; i < path.nodes.size(); ++i) {
        std::cout << path.nodes[i];
        if (i < path.nodes.size() - 1) {
            std::cout << " -> ";
        }
    }
    std::cout << "\n";
    std::cout << "Total Cost: " << std::fixed << std::setprecision(4) 
              << path.total_cost << "\n";
    std::cout << "Hop Count: " << path.hop_count() << "\n";
}

/**
 * @brief Print link cost calculation for debugging.
 */
void print_cost_calculation(const std::string& name, const LinkMetric& metric) {
    double base_cost = (10000.0 / metric.bandwidth_mbps) + metric.latency_ms;
    double loss_factor = (metric.packet_loss_rate > 0 && metric.packet_loss_rate < 1.0) 
                         ? (1.0 / (1.0 - metric.packet_loss_rate)) 
                         : 1.0;
    double final_cost = base_cost * loss_factor;
    
    std::cout << "  " << name << ": "
              << "BW=" << metric.bandwidth_mbps << " Mbps, "
              << "Lat=" << metric.latency_ms << " ms, "
              << "Loss=" << (metric.packet_loss_rate * 100) << "% "
              << "=> Cost=" << std::fixed << std::setprecision(4) << final_cost
              << "\n";
}

int main() {
    std::cout << "============================================================\n";
    std::cout << "  HYPER-TRANSFER v2.0 - Topology Manager Test\n";
    std::cout << "============================================================\n\n";
    
    // ========================================================================
    // Setup Topology
    // ========================================================================
    TopologyManager topo;
    
    std::cout << "[1] Setting up test topology...\n\n";
    std::cout << "    Topology Diagram:\n";
    std::cout << "    \n";
    std::cout << "                    (Fast: 1Gbps, 5ms)\n";
    std::cout << "              GZ ─────────────────────> WH\n";
    std::cout << "               │                         │\n";
    std::cout << "               │ (Slow: 100Mbps, 50ms)   │ (Fast: 1Gbps, 5ms)\n";
    std::cout << "               │         1% loss         │\n";
    std::cout << "               └────────────────────────>BJ\n";
    std::cout << "    \n";
    
    // Direct Link (Slow): GZ -> BJ
    LinkMetric direct_slow(100.0, 50.0, 0.01);  // 100 Mbps, 50ms, 1% loss
    topo.add_link("GZ", "BJ", direct_slow);
    
    // Indirect Links (Fast): GZ -> WH -> BJ
    LinkMetric fast_link(1000.0, 5.0, 0.0);     // 1000 Mbps, 5ms, 0% loss
    topo.add_link("GZ", "WH", fast_link);
    topo.add_link("WH", "BJ", fast_link);
    
    // ========================================================================
    // Cost Calculations
    // ========================================================================
    std::cout << "[2] Link cost calculations:\n";
    print_cost_calculation("GZ->BJ (direct)", direct_slow);
    print_cost_calculation("GZ->WH (fast)", fast_link);
    print_cost_calculation("WH->BJ (fast)", fast_link);
    
    // Calculate expected costs
    double direct_cost = ((10000.0 / 100.0) + 50.0) * (1.0 / (1.0 - 0.01));
    double indirect_cost = ((10000.0 / 1000.0) + 5.0) * 2;  // Two fast hops
    
    std::cout << "\n    Expected direct cost (GZ->BJ):     " 
              << std::fixed << std::setprecision(4) << direct_cost << "\n";
    std::cout << "    Expected indirect cost (GZ->WH->BJ): " 
              << indirect_cost << "\n";
    std::cout << "    Indirect should be BETTER (lower cost)\n\n";
    
    // ========================================================================
    // Find Best Path
    // ========================================================================
    std::cout << "[3] Finding best path from GZ to BJ...\n\n";
    
    NetworkPath best_path = topo.find_best_path("GZ", "BJ");
    
    std::cout << "    Result:\n    ";
    print_path(best_path);
    std::cout << "\n";
    
    // ========================================================================
    // Validation
    // ========================================================================
    std::cout << "[4] Validation:\n";
    
    bool test_passed = true;
    
    // Check path is valid
    if (!best_path.is_valid()) {
        std::cout << "    ✗ FAILED: Path is invalid/unreachable\n";
        test_passed = false;
    } else {
        std::cout << "    ✓ Path is valid\n";
    }
    
    // Check path goes through WH
    bool goes_through_wh = false;
    for (const auto& node : best_path.nodes) {
        if (node == "WH") {
            goes_through_wh = true;
            break;
        }
    }
    
    if (goes_through_wh) {
        std::cout << "    ✓ Path goes through WH (optimal route selected)\n";
    } else {
        std::cout << "    ✗ FAILED: Path does NOT go through WH!\n";
        std::cout << "      Algorithm selected suboptimal direct route.\n";
        test_passed = false;
    }
    
    // Check expected path structure
    if (best_path.nodes.size() == 3 &&
        best_path.nodes[0] == "GZ" &&
        best_path.nodes[1] == "WH" &&
        best_path.nodes[2] == "BJ") {
        std::cout << "    ✓ Path structure is correct: GZ -> WH -> BJ\n";
    } else {
        std::cout << "    ✗ FAILED: Unexpected path structure\n";
        test_passed = false;
    }
    
    // Check cost is reasonable
    double tolerance = 0.01;
    if (std::abs(best_path.total_cost - indirect_cost) < tolerance) {
        std::cout << "    ✓ Cost matches expected value: " << indirect_cost << "\n";
    } else {
        std::cout << "    ! Cost differs from expected: got " 
                  << best_path.total_cost << ", expected " << indirect_cost << "\n";
    }
    
    // ========================================================================
    // Final Result
    // ========================================================================
    std::cout << "\n============================================================\n";
    if (test_passed) {
        std::cout << "  TEST PASSED ✓\n";
        std::cout << "  Dijkstra's algorithm correctly selected the faster\n";
        std::cout << "  indirect route through WH over the slow direct link.\n";
    } else {
        std::cout << "  TEST FAILED ✗\n";
        std::cout << "  The routing algorithm did not select the optimal path.\n";
    }
    std::cout << "============================================================\n";
    
    return test_passed ? 0 : 1;
}
