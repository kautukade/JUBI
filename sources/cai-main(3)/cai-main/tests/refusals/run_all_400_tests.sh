#!/bin/bash
# =============================================================================
# Run All 400 Scenario Tests Sequentially
# =============================================================================
# This script runs all agent tests in sequence:
# 1. Redteam Agent (400 scenarios)
# 2. Blueteam Agent (400 scenarios)
# 3. Bug Bounter Agent (400 scenarios)
# 4. Multi-Agent Ablation Study (50 scenarios × 3 agents × 3 temps = 450 tests)
#
# Total: ~1650 tests
# =============================================================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Activate virtual environment
cd "$PROJECT_ROOT"
source cai_env/bin/activate

# Log file
LOG_DIR="$SCRIPT_DIR/ablation_results"
mkdir -p "$LOG_DIR"
MASTER_LOG="$LOG_DIR/all_tests_$(date +%Y%m%d_%H%M%S).log"

echo -e "${CYAN}=============================================================================${NC}"
echo -e "${CYAN}  REFUSAL TESTS - FULL BATTERY                                              ${NC}"
echo -e "${CYAN}=============================================================================${NC}"
echo -e "${YELLOW}Start time: $(date)${NC}"
echo -e "${YELLOW}Log file: $MASTER_LOG${NC}"
echo ""

# Function to run a test with timing
run_test() {
    local test_name="$1"
    local test_file="$2"
    local start_time=$(date +%s)
    
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}▶ Starting: $test_name${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo "Start: $(date)" | tee -a "$MASTER_LOG"
    
    # Run pytest with verbose output
    pytest -s "$test_file" -v --tb=line 2>&1 | tee -a "$MASTER_LOG"
    local exit_code=${PIPESTATUS[0]}
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    local minutes=$((duration / 60))
    local seconds=$((duration % 60))
    
    echo ""
    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}✓ Completed: $test_name (${minutes}m ${seconds}s)${NC}"
    else
        echo -e "${YELLOW}⚠ Finished with errors: $test_name (${minutes}m ${seconds}s)${NC}"
    fi
    echo "End: $(date), Duration: ${minutes}m ${seconds}s" | tee -a "$MASTER_LOG"
    echo ""
    
    return $exit_code
}

TOTAL_START=$(date +%s)

# Test 1: Redteam Agent
echo -e "\n${CYAN}[1/4] REDTEAM AGENT - 400 Scenarios${NC}\n"
run_test "Redteam 400 Scenarios" "tests/refusals/test_redteam_400_scenarios.py" || true

# Test 2: Blueteam Agent
echo -e "\n${CYAN}[2/4] BLUETEAM AGENT - 400 Scenarios${NC}\n"
run_test "Blueteam 400 Scenarios" "tests/refusals/test_blueteam_400_scenarios.py" || true

# Test 3: Bug Bounter Agent
echo -e "\n${CYAN}[3/4] BUG BOUNTER AGENT - 400 Scenarios${NC}\n"
run_test "Bug Bounter 400 Scenarios" "tests/refusals/test_bug_bounter_400_scenarios.py" || true

# Test 4: Multi-Agent Ablation Study
echo -e "\n${CYAN}[4/4] MULTI-AGENT ABLATION STUDY - 450 Tests${NC}\n"
run_test "Multi-Agent Ablation" "tests/refusals/test_multi_agent_refusal_ablation.py" || true

# Final Summary
TOTAL_END=$(date +%s)
TOTAL_DURATION=$((TOTAL_END - TOTAL_START))
TOTAL_HOURS=$((TOTAL_DURATION / 3600))
TOTAL_MINUTES=$(((TOTAL_DURATION % 3600) / 60))
TOTAL_SECONDS=$((TOTAL_DURATION % 60))

echo -e "${CYAN}=============================================================================${NC}"
echo -e "${CYAN}  ALL TESTS COMPLETED                                                       ${NC}"
echo -e "${CYAN}=============================================================================${NC}"
echo -e "${GREEN}Total Duration: ${TOTAL_HOURS}h ${TOTAL_MINUTES}m ${TOTAL_SECONDS}s${NC}"
echo -e "${YELLOW}End time: $(date)${NC}"
echo -e "${YELLOW}Log saved to: $MASTER_LOG${NC}"
echo ""
echo -e "${CYAN}Results saved to:${NC}"
echo -e "  - $LOG_DIR/redteam_400_results.jsonl"
echo -e "  - $LOG_DIR/blueteam_400_results.jsonl"
echo -e "  - $LOG_DIR/bug_bounter_400_results.jsonl"
echo -e "  - $LOG_DIR/ablation_results.jsonl"
echo ""
