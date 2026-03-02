#include "../src/sql/sql_runner.hpp"
#include "../src/ir/umbra_ir.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct TestFailure {
    std::string name;
    std::string message;
};

static InMemoryTable makeTable(
    const std::string& name,
    const std::vector<std::string>& colNames,
    const std::vector<std::vector<int64_t>>& cols) {
    InMemoryTable t;
    t.name = name;
    t.columnNames = colNames;
    t.columnTypes.assign(colNames.size(), SQLType::Integer);
    t.columns = cols;
    t.numRows = cols.empty() ? 0 : cols[0].size();
    return t;
}

static void expectRowCount(
    const std::string& testName,
    const QueryResult& r,
    size_t expected,
    std::vector<TestFailure>& failures) {
    if (r.rows.size() != expected) {
        failures.push_back({
            testName,
            "row-count mismatch: expected " + std::to_string(expected) +
                ", got " + std::to_string(r.rows.size())
        });
    }
}

static std::multiset<std::pair<int64_t, int64_t>> asPairSet(const QueryResult& r) {
    std::multiset<std::pair<int64_t, int64_t>> out;
    for (const auto& row : r.rows) {
        if (row.size() >= 2) out.insert({row[0], row[1]});
    }
    return out;
}

static void runSqlBehaviorTests(std::vector<TestFailure>& failures) {
    auto customers = std::make_unique<InMemoryTable>(makeTable(
        "customers",
        {"id", "name"},
        {
            {1, 2, 3},
            {100, 200, 300},
        }));

    auto orders = std::make_unique<InMemoryTable>(makeTable(
        "orders",
        {"id", "customer_id", "amount"},
        {
            {10, 11, 12, 13, 14},
            {1, 2, 1, 3, 2},
            {50, 150, 200, 75, 300},
        }));

    auto a = std::make_unique<InMemoryTable>(makeTable(
        "a",
        {"k", "v"},
        {
            {1, 1},
            {10, 11},
        }));

    auto b = std::make_unique<InMemoryTable>(makeTable(
        "b",
        {"k", "v"},
        {
            {1, 1, 2},
            {20, 21, 22},
        }));

    std::vector<const InMemoryTable*> tables = {
        customers.get(), orders.get(), a.get(), b.get()
    };

    {
        QueryResult r = runQuery(
            "SELECT o.id FROM orders o WHERE 0 = 1 OR o.id = 10",
            tables);
        expectRowCount("or-constant", r, 1, failures);
        if (r.rows.size() == 1 && r.rows[0][0] != 10) {
            failures.push_back({"or-constant", "expected row value 10"});
        }
    }

    {
        QueryResult r = runQuery(
            "SELECT o.id FROM orders o WHERE (o.id = 10) OR (o.id = 11)",
            tables);
        expectRowCount("or-comparison", r, 2, failures);
        std::set<int64_t> ids;
        for (const auto& row : r.rows) ids.insert(row[0]);
        if (ids != std::set<int64_t>{10, 11}) {
            failures.push_back({"or-comparison", "expected ids {10, 11}"});
        }
    }

    {
        QueryResult r = runQuery(
            "SELECT o.id FROM orders o WHERE o.id = 10 AND o.amount > 100",
            tables);
        expectRowCount("and-semantics", r, 0, failures);
    }

    {
        QueryResult r = runQuery(
            "SELECT c.id, o.id FROM customers c, orders o",
            tables);
        expectRowCount("cross-join", r, 15, failures);
    }

    {
        QueryResult r = runQuery(
            "SELECT a.v, b.v FROM a, b WHERE a.k = b.k",
            tables);
        expectRowCount("duplicate-hash-join", r, 4, failures);
        auto got = asPairSet(r);
        std::multiset<std::pair<int64_t, int64_t>> expected = {
            {10, 20}, {10, 21}, {11, 20}, {11, 21}
        };
        if (got != expected) {
            failures.push_back({"duplicate-hash-join", "expected 2x2 match combinations"});
        }
    }
}

static void runIrTests(std::vector<TestFailure>& failures) {
    {
        IRProgram p;
        p.beginFunction("const_fold", IRType::Void, {});

        IRValueRef c2 = p.addConstInt(2, IRType::Int64);
        IRValueRef c3 = p.addConstInt(3, IRType::Int64);
        IRValueRef add = p.addBinary(Opcode::Add, IRType::Int64, c2, c3);
        IRValueRef cmp = p.addBinary(Opcode::CmpLt, IRType::Bool, c2, c3);
        IRValueRef t   = p.addConstBool(true);
        IRValueRef f   = p.addConstBool(false);
        IRValueRef lor = p.addBinary(Opcode::LOr, IRType::Bool, t, f);
        p.addReturn();

        if (!p.isConstant(add) || p.constIntValue(add) != 5) {
            failures.push_back({"ir-const-fold-add", "expected folded constant 5"});
        }
        if (!p.isConstant(cmp) || p.constIntValue(cmp) != 1) {
            failures.push_back({"ir-const-fold-cmp", "expected folded boolean true"});
        }
        if (!p.isConstant(lor) || p.constIntValue(lor) != 1) {
            failures.push_back({"ir-const-fold-lor", "expected folded boolean true"});
        }
    }

    {
        IRProgram p;
        p.beginFunction("dce", IRType::Void, {});
        uint32_t reachable = p.addBlock("reachable");
        uint32_t dead = p.addBlock("dead");

        p.addBranch(reachable);

        p.setInsertionPoint(0, dead);
        p.addConstInt(42, IRType::Int64);
        p.addReturn();

        p.setInsertionPoint(0, reachable);
        p.addReturn();

        if (p.functions[0].blocks[dead].instructions.empty()) {
            failures.push_back({"ir-dce-unreachable-setup", "dead block setup failed"});
        }

        p.eliminateDeadCode();

        if (!p.functions[0].blocks[dead].instructions.empty()) {
            failures.push_back({"ir-dce-unreachable", "expected unreachable block instructions removed"});
        }
    }
}

int main() {
    std::vector<TestFailure> failures;

    runSqlBehaviorTests(failures);
    runIrTests(failures);

    if (failures.empty()) {
        std::cout << "[PASS] All tests passed\n";
        return 0;
    }

    std::cout << "[FAIL] " << failures.size() << " test(s) failed:\n";
    for (const auto& f : failures) {
        std::cout << "  - " << f.name << ": " << f.message << "\n";
    }
    return 1;
}
