// main.cpp - Umbra POC
//
// Usage:
//   umbra                        — start interactive REPL (built-in tables)
//   umbra -v                     — verbose REPL (pipeline details to stderr)
//   umbra --demo                 — run paper demonstration (all 6 steps)
//   umbra --csv <file> <table>   — load CSV at startup, can repeat
//   umbra --validate             — run CSV validation suite (data/ directory)
//
// Inside the REPL:
//   SELECT ...                   — execute query, show results
//   EXPLAIN SELECT ...           — show operator tree only
//   EXPLAIN tt=1 SELECT ...      — + Umbra IR (Tidy Tuples layer)
//   EXPLAIN fs=1 SELECT ...      — + Flying Start native assembly
//   EXPLAIN tt=1 fs=1 SELECT ... — show all pipeline stages
//   .load <file.csv> <tablename> — load CSV file as a table
//   .tables                      — list available tables
//   .schema <table>              — show table schema (first 20 rows if large)
//   .help                        — show this help
//   .quit / .exit / Ctrl-D      — exit

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <algorithm>
#include <cctype>

#include "ir/umbra_ir.hpp"
#include "ir/ir_printer.hpp"
#include "codegen/codegen_types.hpp"
#include "codegen/sql_value.hpp"
#include "codegen/tuple_ops.hpp"
#include "operators/compilation_context.hpp"
#include "operators/scan_translator.hpp"
#include "operators/select_translator.hpp"
#include "operators/hash_join_translator.hpp"
#include "backend/flying_start.hpp"
#include "sql/sql_runner.hpp"
#include "io/csv_loader.hpp"

// ─── Global state ─────────────────────────────────────────────────────────────

static bool g_verbose = false;

static void vlog(const std::string& msg) {
    if (g_verbose) std::cerr << "[v] " << msg << "\n";
}

// All tables currently registered in the REPL.
// We own both the InMemoryTable and (for CSV-loaded tables) the CSVTable.
struct TableEntry {
    std::unique_ptr<CSVTable>    csv;    // non-null if loaded from CSV
    std::unique_ptr<InMemoryTable> mem;  // non-null if hard-coded
    InMemoryTable* ptr() {
        return csv ? &csv->table : mem.get();
    }
    const InMemoryTable* ptr() const {
        return csv ? &csv->table : mem.get();
    }
};

static std::vector<TableEntry> g_tableStorage;
static std::vector<const InMemoryTable*> g_tables;  // parallel to g_tableStorage

// ─── Built-in demo tables ─────────────────────────────────────────────────────

static void addBuiltinTables() {
    {
        auto t = std::make_unique<InMemoryTable>();
        t->name        = "customers";
        t->numRows     = 3;
        t->columnNames = {"id", "name"};
        t->columnTypes = {SQLType::Integer, SQLType::Integer};
        t->columns.resize(2);
        t->columns[0] = {1,   2,   3  };
        t->columns[1] = {100, 200, 300};
        TableEntry e;
        e.mem = std::move(t);
        g_tables.push_back(e.ptr());
        g_tableStorage.push_back(std::move(e));
    }
    {
        auto t = std::make_unique<InMemoryTable>();
        t->name        = "orders";
        t->numRows     = 5;
        t->columnNames = {"id", "customer_id", "amount"};
        t->columnTypes = {SQLType::Integer, SQLType::Integer, SQLType::Integer};
        t->columns.resize(3);
        t->columns[0] = {10, 11,  12,  13,  14};
        t->columns[1] = {1,   2,   1,   3,   2};
        t->columns[2] = {50, 150, 200,  75, 300};
        TableEntry e;
        e.mem = std::move(t);
        g_tables.push_back(e.ptr());
        g_tableStorage.push_back(std::move(e));
    }
}

// Register a CSV-loaded table (replaces existing table with same name).
static void registerCSVTable(std::unique_ptr<CSVTable> csv) {
    const std::string& name = csv->table.name;
    // Replace existing entry if same name
    for (size_t i = 0; i < g_tableStorage.size(); ++i) {
        if (g_tableStorage[i].ptr()->name == name) {
            TableEntry e;
            e.csv = std::move(csv);
            g_tables[i] = e.ptr();
            g_tableStorage[i] = std::move(e);
            return;
        }
    }
    // New table
    TableEntry e;
    e.csv = std::move(csv);
    g_tables.push_back(e.ptr());
    g_tableStorage.push_back(std::move(e));
}

// Find a CSVTable by name (for string column display)
static const CSVTable* findCSVTable(const std::string& name) {
    for (const auto& e : g_tableStorage)
        if (e.csv && e.csv->table.name == name)
            return e.csv.get();
    return nullptr;
}

// ─── Result printing ──────────────────────────────────────────────────────────
// For string-column tables, look up the integer ID in the string dictionary.

static void printResult(const QueryResult& r) {
    if (r.columnNames.empty()) return;

    // Collect string dicts for columns that came from CSV string columns.
    // We'll look them up by checking the value range (positive small ints → likely IDs).
    // Simpler: just print integers. If user wants string display they can .schema.
    // But for the validation output, we want readable strings for status/name columns.
    // We can't easily know which CSV table each output column came from here,
    // so we just print integers. The validation code prints strings explicitly.

    const size_t COL_W = 14;
    std::cout << "\n  ";
    for (auto& col : r.columnNames)
        std::cout << std::setw(COL_W) << col;
    std::cout << "\n  ";
    for (size_t i = 0; i < r.columnNames.size(); ++i)
        std::cout << std::string(COL_W, '-');
    std::cout << "\n";

    size_t printed = 0;
    for (auto& row : r.rows) {
        if (printed >= 40 && r.rows.size() > 45) {
            if (printed == 40)
                std::cout << "  ... (" << (r.rows.size() - 40)
                          << " more rows not shown)\n";
            ++printed;
            continue;
        }
        std::cout << "  ";
        for (int64_t v : row)
            std::cout << std::setw(COL_W) << v;
        std::cout << "\n";
        ++printed;
    }
    std::cout << "\n  (" << r.rows.size() << " row"
              << (r.rows.size() != 1 ? "s" : "") << ")\n\n";
}

// ─── EXPLAIN parser ───────────────────────────────────────────────────────────

static bool parseExplain(const std::string& rest, ExplainOptions& opts,
                          std::string& sqlOut) {
    std::string s = rest;
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) { sqlOut = ""; return true; }
    s = s.substr(start);

    opts.showTree = true;

    while (true) {
        if (s.size() >= 4 && s.substr(0, 4) == "tt=1") {
            opts.showTT = true;
            s = s.substr(4);
            start = s.find_first_not_of(" \t");
            s = (start == std::string::npos) ? "" : s.substr(start);
        } else if (s.size() >= 4 && s.substr(0, 4) == "fs=1") {
            opts.showFS = true;
            s = s.substr(4);
            start = s.find_first_not_of(" \t");
            s = (start == std::string::npos) ? "" : s.substr(start);
        } else {
            break;
        }
    }
    sqlOut = s;
    return true;
}

// ─── REPL commands ────────────────────────────────────────────────────────────

static void cmdLoad(const std::string& args) {
    // .load <path> <tablename>
    std::istringstream ss(args);
    std::string path, tableName;
    ss >> path >> tableName;

    if (path.empty() || tableName.empty()) {
        std::cout << "  Usage: .load <file.csv> <tablename>\n\n";
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    try {
        auto csv = std::make_unique<CSVTable>(loadCSV(path, tableName));
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        size_t rows = csv->table.numRows;
        size_t cols = csv->table.columnNames.size();
        std::cout << "  Loaded " << tableName << ": "
                  << rows << " rows, " << cols << " cols"
                  << "  (" << std::fixed << std::setprecision(1) << ms << " ms)\n";

        // Show column types
        std::cout << "  Columns: ";
        for (size_t c = 0; c < cols; ++c) {
            if (c) std::cout << ", ";
            std::cout << csv->table.columnNames[c];
            if (csv->isStringCol[c]) std::cout << "(str)";
        }
        std::cout << "\n\n";

        registerCSVTable(std::move(csv));
    } catch (const std::exception& ex) {
        std::cout << "  Error loading CSV: " << ex.what() << "\n\n";
    }
}

static void cmdTables() {
    std::cout << "\n  Available tables:\n";
    for (const InMemoryTable* t : g_tables) {
        std::cout << "    " << std::left << std::setw(16) << t->name
                  << std::right << std::setw(8) << t->numRows << " rows  ";
        for (size_t c = 0; c < t->columnNames.size(); ++c) {
            if (c) std::cout << ", ";
            std::cout << t->columnNames[c];
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

static void cmdSchema(const std::string& arg) {
    std::string name = arg;
    size_t s = name.find_first_not_of(" \t");
    if (s != std::string::npos) name = name.substr(s);
    size_t e = name.find_last_not_of(" \t\n\r");
    if (e != std::string::npos) name = name.substr(0, e + 1);

    const InMemoryTable* found = nullptr;
    for (const InMemoryTable* t : g_tables)
        if (t->name == name) { found = t; break; }

    if (!found) {
        std::cout << "  Unknown table: " << name << "\n\n";
        return;
    }

    const CSVTable* csv = findCSVTable(name);

    std::cout << "\n  Table: " << found->name
              << "  (" << found->numRows << " rows, "
              << found->columnNames.size() << " cols)\n\n";

    const size_t COL_W = 14;
    std::cout << "  ";
    for (auto& c : found->columnNames)
        std::cout << std::setw(COL_W) << c;
    std::cout << "\n  ";
    for (size_t i = 0; i < found->columnNames.size(); ++i)
        std::cout << std::string(COL_W, '-');
    std::cout << "\n";

    size_t showRows = std::min(found->numRows, size_t(20));
    for (size_t row = 0; row < showRows; ++row) {
        std::cout << "  ";
        for (size_t col = 0; col < found->columnNames.size(); ++col) {
            int64_t v = found->columns[col][row];
            if (csv && col < csv->isStringCol.size() && csv->isStringCol[col]) {
                std::cout << std::setw(COL_W) << csv->strings.lookup(v);
            } else {
                std::cout << std::setw(COL_W) << v;
            }
        }
        std::cout << "\n";
    }
    if (found->numRows > showRows)
        std::cout << "  ... (" << (found->numRows - showRows) << " more rows)\n";
    std::cout << "\n";
}

static void cmdHelp() {
    std::cout << R"(
  Umbra v0 — interactive SQL REPL
  Based on: Kersten, Leis, Neumann — PVLDB 2021

  Query commands:
    SELECT ...                   execute query, show results
    EXPLAIN SELECT ...           show operator tree (Tidy Tuples Layer 1)
    EXPLAIN tt=1 SELECT ...      + Umbra IR output (flat byte-array IR)
    EXPLAIN fs=1 SELECT ...      + Flying Start native assembly + stats
    EXPLAIN tt=1 fs=1 SELECT ... show all pipeline stages

  CSV commands:
    .load <file.csv> <table>     load CSV file as a queryable table
                                 (replaces existing table with same name)

  Meta commands:
    .tables              list available tables with row counts
    .schema <table>      show column types and first 20 rows
    .help                show this help
    .quit / .exit        exit

  Notes:
    - All values are stored as int64_t; string columns are interned to IDs
    - String columns show original values in .schema
    - Query results show raw IDs for string columns (use WHERE on the int value)

  Examples:
    .load data/customers.csv customers
    .load data/orders.csv orders
    SELECT c.id, o.id, o.amount FROM customers c, orders o
           WHERE c.id = o.customer_id AND o.amount > 1500
    EXPLAIN tt=1 SELECT o.id, o.amount FROM orders o WHERE o.amount > 1000

)";
}

// ─── Execute one REPL line ────────────────────────────────────────────────────

static bool execLine(const std::string& rawLine) {
    std::string line = rawLine;
    size_t s = line.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return true;
    line = line.substr(s);
    size_t e = line.find_last_not_of(" \t\r\n");
    if (e != std::string::npos) line = line.substr(0, e + 1);
    if (line.empty()) return true;

    if (line[0] == '.') {
        std::string cmd = line.substr(1);
        if (cmd == "quit" || cmd == "exit") return false;
        if (cmd == "tables") { cmdTables(); return true; }
        if (cmd == "help")   { cmdHelp();   return true; }
        if (cmd.substr(0, 6) == "schema") { cmdSchema(cmd.substr(6)); return true; }
        if (cmd.substr(0, 4) == "load")   { cmdLoad(cmd.substr(4));   return true; }
        std::cout << "  Unknown command: " << line << "  (type .help)\n\n";
        return true;
    }

    // Case-insensitive EXPLAIN check
    std::string upper = line;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    if (upper.substr(0, 7) == "EXPLAIN") {
        ExplainOptions opts;
        std::string sql;
        parseExplain(line.substr(7), opts, sql);
        if (sql.empty()) { std::cout << "  EXPLAIN requires a SQL statement.\n\n"; return true; }
        vlog("EXPLAIN tt=" + std::to_string(opts.showTT) +
             " fs=" + std::to_string(opts.showFS));
        try {
            runQuery(sql, g_tables, &opts, &std::cout);
        } catch (const std::exception& ex) {
            std::cout << "  Error: " << ex.what() << "\n\n";
        }
        return true;
    }

    // Regular query
    vlog("Query: " + line);
    auto t0 = std::chrono::steady_clock::now();
    try {
        QueryResult r = runQuery(line, g_tables);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printResult(r);
        std::cout << "  [" << std::fixed << std::setprecision(2) << ms << " ms]\n\n";
    } catch (const std::exception& ex) {
        std::cout << "  Error: " << ex.what() << "\n\n";
    }
    return true;
}

// ─── REPL ─────────────────────────────────────────────────────────────────────

static void runREPL() {
    std::cout << "╔═══════════════════════════════════════════════════════╗\n";
    std::cout << "║  Umbra v0  —  Tidy Tuples + Umbra IR + Flying Start\n";
    std::cout << "║  Based on: Kersten, Leis, Neumann — PVLDB 2021\n";
    std::cout << "╚═══════════════════════════════════════════════════════╝\n\n";
    if (g_verbose)
        std::cout << "  [verbose mode: pipeline details on stderr]\n\n";
    std::cout << "  Type .help for commands, .tables to list tables.\n\n";

    std::string line;
    while (true) {
        std::cout << "umbra> ";
        std::cout.flush();
        if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }
        if (!execLine(line)) break;
    }
}

// ─── CSV Validation Suite ─────────────────────────────────────────────────────
// Loads data/customers.csv and data/orders.csv, runs several queries,
// prints row counts + timing, and spot-checks a few expected values.

static void runValidation(const std::string& dataDir) {
    std::cout << "╔═══════════════════════════════════════════════════════╗\n";
    std::cout << "║  Umbra v0 — CSV Validation Suite\n";
    std::cout << "╚═══════════════════════════════════════════════════════╝\n\n";

    // ── Load CSV files ────────────────────────────────────────────────────────
    std::cout << "Loading CSV files from: " << dataDir << "\n\n";

    auto loadAndRegister = [&](const std::string& file, const std::string& tbl) {
        std::string path = dataDir + "/" + file;
        std::cout << "  " << file << " → " << tbl << " ... ";
        std::cout.flush();
        auto t0 = std::chrono::steady_clock::now();
        auto csv = std::make_unique<CSVTable>(loadCSV(path, tbl));
        auto t1  = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << csv->table.numRows << " rows, "
                  << csv->table.columnNames.size() << " cols  ("
                  << std::fixed << std::setprecision(1) << ms << " ms)\n";
        registerCSVTable(std::move(csv));
    };

    try {
        loadAndRegister("customers.csv", "customers");
        loadAndRegister("orders.csv",    "orders");
    } catch (const std::exception& ex) {
        std::cout << "\n  Error: " << ex.what() << "\n\n";
        std::cout << "  Hint: generate test data with:\n";
        std::cout << "    python3 scripts/gen_csv.py\n\n";
        return;
    }

    // ── Show schema ───────────────────────────────────────────────────────────
    std::cout << "\n";
    for (const InMemoryTable* t : g_tables) {
        std::cout << "  " << t->name << " columns: ";
        for (size_t c = 0; c < t->columnNames.size(); ++c) {
            if (c) std::cout << ", ";
            std::cout << t->columnNames[c];
            const CSVTable* csv = findCSVTable(t->name);
            if (csv && c < csv->isStringCol.size() && csv->isStringCol[c])
                std::cout << "(str)";
        }
        std::cout << "\n";
    }
    std::cout << "\n";

    // ── Run queries ───────────────────────────────────────────────────────────
    struct TestCase {
        std::string desc;
        std::string sql;
        size_t      expectedMinRows;  // sanity lower bound
        size_t      expectedMaxRows;  // sanity upper bound
    };

    std::vector<TestCase> tests = {
        {
            "Q1: High-value orders (amount > 1500)",
            // ~25% of 50k = ~12,500 rows expected
            "SELECT o.id, o.amount FROM orders o WHERE o.amount > 1500",
            5000, 20000
        },
        {
            "Q2: Customers with credit_limit >= 10000",
            // 1-in-5 credit tiers → ~20% of 10k = ~2000 rows
            "SELECT c.id, c.credit_limit FROM customers c WHERE c.credit_limit >= 10000",
            500, 5000
        },
        {
            "Q3: Hash join — orders with amount > 1500, with customer id",
            // same ~12,500 orders joined to matching customers
            "SELECT o.id, o.amount, c.id "
            "FROM customers c, orders o "
            "WHERE c.id = o.customer_id AND o.amount > 1500",
            5000, 20000
        },
        {
            "Q4: High-credit customers with large orders (join + two filters)",
            // amount > 1800 (~10% of orders) AND credit >= 5000 (~40% customers)
            // ~50k * 10% * 40% = ~2000
            "SELECT o.id, o.amount, c.id, c.credit_limit "
            "FROM customers c, orders o "
            "WHERE c.id = o.customer_id AND o.amount > 1800 AND c.credit_limit >= 5000",
            100, 8000
        },
        {
            "Q5: Very selective — amount >= 1990",
            // ~10/1991 of 50k ≈ 250 rows
            "SELECT o.id, o.amount FROM orders o WHERE o.amount >= 1990",
            50, 1000
        },
    };

    std::cout << "Running " << tests.size() << " queries:\n\n";

    bool allPass = true;
    for (size_t qi = 0; qi < tests.size(); ++qi) {
        const TestCase& tc = tests[qi];
        std::cout << "  " << tc.desc << "\n";
        std::cout << "  SQL: " << tc.sql << "\n";

        auto t0 = std::chrono::steady_clock::now();
        QueryResult r;
        try {
            r = runQuery(tc.sql, g_tables);
        } catch (const std::exception& ex) {
            std::cout << "  ✗ ERROR: " << ex.what() << "\n\n";
            allPass = false;
            continue;
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        bool pass = (r.rows.size() >= tc.expectedMinRows &&
                     r.rows.size() <= tc.expectedMaxRows);
        allPass &= pass;

        std::cout << "  " << (pass ? "✓" : "✗")
                  << " " << r.rows.size() << " rows"
                  << "  (" << std::fixed << std::setprecision(2) << ms << " ms)";
        if (!pass)
            std::cout << "  [expected " << tc.expectedMinRows
                      << "–" << tc.expectedMaxRows << "]";
        std::cout << "\n";

        // Print first 5 rows
        if (!r.rows.empty()) {
            std::cout << "    ";
            for (auto& col : r.columnNames)
                std::cout << std::setw(12) << col;
            std::cout << "\n";
            for (size_t i = 0; i < std::min(r.rows.size(), size_t(5)); ++i) {
                std::cout << "    ";
                for (int64_t v : r.rows[i])
                    std::cout << std::setw(12) << v;
                std::cout << "\n";
            }
            if (r.rows.size() > 5)
                std::cout << "    ... (" << r.rows.size() - 5 << " more)\n";
        }
        std::cout << "\n";
    }

    // ── Cross-check: verify a specific known row ───────────────────────────────
    // In the generated data (seed=42), we know all orders with amount > 1990
    // exist — just verify the query returns plausible values.
    {
        std::cout << "  Cross-check: verify orders.id values are in [1, 50000]\n";
        QueryResult r = runQuery(
            "SELECT o.id, o.amount FROM orders o WHERE o.amount > 1990",
            g_tables);
        bool ok = true;
        for (auto& row : r.rows) {
            if (row[0] < 1 || row[0] > 50000 || row[1] < 1990 || row[1] > 2000)
                ok = false;
        }
        allPass &= ok;
        std::cout << "  " << (ok ? "✓" : "✗")
                  << " All " << r.rows.size()
                  << " high-amount orders have valid id and amount\n\n";
    }

    {
        std::cout << "  Cross-check: join result — every o.customer_id must be in customers\n";
        // Get all customer ids
        QueryResult custs = runQuery(
            "SELECT c.id FROM customers c WHERE c.id >= 1", g_tables);
        std::vector<int64_t> custIds;
        custIds.reserve(custs.rows.size());
        for (auto& r : custs.rows) custIds.push_back(r[0]);
        std::sort(custIds.begin(), custIds.end());

        QueryResult joined = runQuery(
            "SELECT o.id, o.amount, c.id "
            "FROM customers c, orders o "
            "WHERE c.id = o.customer_id AND o.amount > 1500",
            g_tables);

        bool ok = true;
        for (auto& row : joined.rows) {
            // row[2] = c.id from the join
            if (!std::binary_search(custIds.begin(), custIds.end(), row[2]))
                ok = false;
        }
        allPass &= ok;
        std::cout << "  " << (ok ? "✓" : "✗")
                  << " All " << joined.rows.size()
                  << " join results have valid customer_id\n\n";
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    std::cout << (allPass
        ? "  ✓ All checks passed.\n\n"
        : "  ✗ Some checks failed.\n\n");
}

// ─── Demo mode ────────────────────────────────────────────────────────────────

static void printSep(const std::string& title) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "  " << title << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n\n";
}

static void demoUmbraIR() {
    printSep("STEP 1: Umbra IR — Direct Example  [paper Section 3]");
    std::cout << "Building IR for: if (x > 5) return 1 else return 0\n\n";

    IRProgram prog;
    prog.beginFunction("example_func", IRType::Int64, {{"x", IRType::Int64}});
    uint32_t fnIdx = 0;
    IRValueRef xParam = IRProgram::paramRef(fnIdx, 0);
    uint32_t yesBlk = prog.addBlock("yes");
    uint32_t noBlk  = prog.addBlock("no");
    IRValueRef five  = prog.addConstInt(5, IRType::Int64);
    IRValueRef cmpGt = prog.addBinary(Opcode::CmpGt, IRType::Bool, xParam, five);
    prog.addCondBranch(cmpGt, yesBlk, noBlk);
    prog.setInsertionPoint(fnIdx, yesBlk);
    prog.addReturn(prog.addConstInt(1, IRType::Int64));
    prog.setInsertionPoint(fnIdx, noBlk);
    prog.addReturn(prog.addConstInt(0, IRType::Int64));

    std::cout << "── Umbra IR ───────────────────────────────────────────\n";
    printIR(prog, std::cout);
    prog.eliminateDeadCode();
    std::cout << "── After DCE (paper Section 3.3) ─────────────────────\n";
    printIR(prog, std::cout);
    std::cout << "instrData size: " << prog.instrData.size() << " bytes\n";
    std::cout << "ConstIntInstr: " << sizeof(ConstIntInstr) << " bytes  |  ";
    std::cout << "BinaryInstr: " << sizeof(BinaryInstr) << " bytes\n\n";
}

static void demoHashGeneration() {
    printSep("STEP 2: Tidy Tuples — Hash Value Generation  [paper Section 2.5]");
    IRProgram prog;
    prog.beginFunction("hash_example", IRType::UInt64,
                        {{"int1", IRType::Int64}, {"int2", IRType::Int64}});
    CodegenContext cg(prog);
    uint32_t fnIdx = 0;
    SQLValue v1{IRProgram::paramRef(fnIdx, 0), NullRef, SQLType::Integer, &cg};
    SQLValue v2{IRProgram::paramRef(fnIdx, 1), NullRef, SQLType::Integer, &cg};
    UInt64 h = hashValues(cg, {v1, v2});
    prog.addReturn(h.ref);
    prog.eliminateDeadCode();
    std::cout << "── Generated IR (matches paper Figure, Section 2.5) ───\n";
    printIR(prog, std::cout);
}

static void demoTidyTuples() {
    // Use the built-in small tables for the demo
    const InMemoryTable* custs   = nullptr;
    const InMemoryTable* orders_ = nullptr;
    for (const InMemoryTable* t : g_tables) {
        if (t->name == "customers" && t->numRows <= 10) custs   = t;
        if (t->name == "orders"    && t->numRows <= 10) orders_ = t;
    }
    if (!custs || !orders_) {
        std::cout << "  (skipping STEP 3/4 — demo tables replaced by CSV)\n";
        return;
    }

    printSep("STEP 3: Tidy Tuples — Produce/Consume  [paper Sections 2.1–2.4]");
    std::cout << "Query: SELECT o.id, o.amount, c.name\n"
                 "       FROM customers c, orders o\n"
                 "       WHERE o.customer_id = c.id AND o.amount > 100\n\n";

    CompilationContext ctx;
    ctx.program.beginFunction("query_main", IRType::Void, {});

    auto* custScan  = new ScanTranslator(ctx, custs);
    auto* orderScan = new ScanTranslator(ctx, orders_);
    IU* amountIU = orderScan->getIU("amount");
    auto* filt = new SelectTranslator(ctx, orderScan,
        [&ctx, amountIU](ConsumerScope& scope) -> SQLValue {
            return scope.get(amountIU).gt(makeIntSQLValue(ctx.codegen, 100));
        });
    IU* custIdIU = custScan->getIU("id");
    IU* ordCustIU = orderScan->getIU("customer_id");
    auto* join = new HashJoinTranslator(ctx, custScan, filt, {custIdIU}, {ordCustIU});

    struct PrintOp : OperatorTranslator {
        IU* oid; IU* amt; IU* nm;
        PrintOp(CompilationContext& c, IU* a, IU* b, IU* d)
            : OperatorTranslator(c), oid(a), amt(b), nm(d) {}
        void produce(OperatorTranslator*) override {}
        void consume(ConsumerScope& scope, OperatorTranslator*) override {
            using Fn = void(*)(int64_t,int64_t,int64_t);
            static Fn fn = [](int64_t o,int64_t a,int64_t n){
                std::cout << "  order=" << std::setw(3) << o
                          << "  amount=" << std::setw(4) << a
                          << "  name=" << n << "\n";
            };
            ctx->program.addCall(IRType::Void, reinterpret_cast<uint64_t>(fn),
                {scope.get(oid).valueRef, scope.get(amt).valueRef, scope.get(nm).valueRef});
        }
    };
    IU* orderIdIU  = orderScan->getIU("id");
    IU* custNameIU = custScan->getIU("name");
    auto* printer = new PrintOp(ctx, orderIdIU, amountIU, custNameIU);

    join->produce(printer);
    ctx.program.addReturn();

    size_t nb = 0;
    for (auto& f : ctx.program.functions) for (auto& b : f.blocks) nb += b.instructions.size();
    std::cout << "Code generation: " << ctx.program.instrData.size()
              << " bytes instrData, " << nb << " instructions\n\n";
    std::cout << "── Umbra IR (before DCE) ─────────────────────────────\n";
    printIR(ctx.program, std::cout);
    ctx.program.eliminateDeadCode();
    size_t na = 0;
    for (auto& f : ctx.program.functions) for (auto& b : f.blocks) na += b.instructions.size();
    std::cout << "── Umbra IR (after DCE, removed " << (nb - na) << ") ──────────────\n";
    printIR(ctx.program, std::cout);

    printSep("STEP 4: Flying Start — Real JIT Compilation  [paper Section 4]");
    FlyingStartBackend backend;
    auto fn = backend.compile(ctx.program, ctx.program.functions[0]);
    std::cout << "── Native assembly ────────────────────────────────────\n";
    backend.dumpAssembly(std::cout);
    std::cout << "\n── Executing ──────────────────────────────────────────\n";
    backend.execute(fn);
    backend.release(fn);
    delete custScan; delete orderScan; delete filt; delete join; delete printer;
}

static void demoFlyingStart() {
    printSep("STEP 5: Flying Start — Optimization Deep-Dive  [paper Section 4]");
    IRProgram prog;
    prog.beginFunction("simple_sum", IRType::Void,
        {{"a",IRType::Int64},{"b",IRType::Int64},{"c",IRType::Int64}});
    uint32_t fi = 0;
    uint32_t rh = prog.addBlock("ret_high"), rl = prog.addBlock("ret_low");
    IRValueRef a = IRProgram::paramRef(fi,0), b = IRProgram::paramRef(fi,1),
               c = IRProgram::paramRef(fi,2);
    IRValueRef ab  = prog.addBinary(Opcode::Add, IRType::Int64, a, b);
    IRValueRef abc = prog.addBinary(Opcode::Add, IRType::Int64, ab, c);
    IRValueRef ten = prog.addConstInt(10, IRType::Int64);
    IRValueRef cmp = prog.addBinary(Opcode::CmpGt, IRType::Bool, abc, ten);
    prog.addCondBranch(cmp, rh, rl);
    prog.setInsertionPoint(fi, rh); prog.addReturn();
    prog.setInsertionPoint(fi, rl); prog.addReturn();

    std::cout << "── Umbra IR ───────────────────────────────────────────\n";
    printIR(prog, std::cout);
    FlyingStartBackend backend;
    auto fn = backend.compile(prog, prog.functions[0]);
    std::cout << "── Native assembly ────────────────────────────────────\n";
    backend.dumpAssembly(std::cout);
    backend.release(fn);
}

static void demoSQLPipeline() {
    printSep("STEP 6: Full SQL Pipeline  [paper Sections 2–4 combined]");

    auto run = [&](const char* label, const char* sql) {
        std::cout << label << "\n\n";
        try {
            auto t0 = std::chrono::steady_clock::now();
            QueryResult r = runQuery(sql, g_tables);
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            printResult(r);
            std::cout << "  [" << std::fixed << std::setprecision(2) << ms << " ms]\n\n";
        } catch (const std::exception& ex) {
            std::cout << "  Error: " << ex.what() << "\n\n";
        }
    };

    run("Query 1: SELECT id, amount FROM orders o WHERE o.amount > 100",
        "SELECT o.id, o.amount FROM orders o WHERE o.amount > 100");

    run("Query 2: SELECT o.id, o.amount, c.name\n"
        "         FROM customers c, orders o\n"
        "         WHERE c.id = o.customer_id AND o.amount > 100",
        "SELECT o.id, o.amount, c.name "
        "FROM customers c, orders o "
        "WHERE c.id = o.customer_id AND o.amount > 100");

    run("Query 3: SELECT id, amount FROM orders o WHERE o.amount > 50",
        "SELECT o.id, o.amount FROM orders o WHERE o.amount > 50");
}

// ─── Main ─────────────────────────────────────────────────────────────────────

static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "  (no args)                   start interactive REPL\n"
        << "  -v                          verbose pipeline output\n"
        << "  --demo                      run paper demonstrations\n"
        << "  --validate [datadir]        run CSV validation suite\n"
        << "  --csv <file.csv> <table>    load CSV at startup (can repeat)\n"
        << "  --help                      show this message\n";
}

int main(int argc, char* argv[]) {
    bool doDemo     = false;
    bool doValidate = false;
    std::string validateDir = "data";
    std::vector<std::pair<std::string,std::string>> csvLoads;  // path, table

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            g_verbose = true;
        } else if (arg == "--demo") {
            doDemo = true;
        } else if (arg == "--validate") {
            doValidate = true;
            if (i + 1 < argc && argv[i+1][0] != '-')
                validateDir = argv[++i];
        } else if (arg == "--csv") {
            if (i + 2 >= argc) { std::cerr << "--csv needs <file> <table>\n"; return 1; }
            csvLoads.push_back({argv[i+1], argv[i+2]});
            i += 2;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    // Always add built-in demo tables first
    addBuiltinTables();

    // Load any --csv files specified on command line
    for (auto& [path, tbl] : csvLoads) {
        try {
            auto t0 = std::chrono::steady_clock::now();
            auto csv = std::make_unique<CSVTable>(loadCSV(path, tbl));
            auto t1  = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cerr << "Loaded " << tbl << ": " << csv->table.numRows
                      << " rows (" << std::fixed << std::setprecision(1) << ms << " ms)\n";
            registerCSVTable(std::move(csv));
        } catch (const std::exception& ex) {
            std::cerr << "Error loading " << path << ": " << ex.what() << "\n";
            return 1;
        }
    }

    if (doValidate) {
        runValidation(validateDir);
    } else if (doDemo) {
        std::cout << "╔═══════════════════════════════════════════════════════╗\n";
        std::cout << "║  Umbra v0: Tidy Tuples + Umbra IR + Flying Start\n";
        std::cout << "║  Based on: Kersten, Leis, Neumann — PVLDB 2021\n";
        std::cout << "╚═══════════════════════════════════════════════════════╝\n";
        demoUmbraIR();
        demoHashGeneration();
        demoTidyTuples();
        demoFlyingStart();
        demoSQLPipeline();
    } else {
        runREPL();
    }

    return 0;
}
