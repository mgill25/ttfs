#pragma once
// csv_loader.hpp - Load a CSV file into an InMemoryTable
//
// Design:
//   - All column values are int64_t (SQL integer type)
//   - String columns are interned: each unique string gets a stable integer ID
//     (IDs are 1-based; 0 = NULL / unknown)
//   - Header row determines column names
//   - Blank cells and non-numeric values use the string intern table
//   - Returns the loaded table + a string dictionary for display

#include "../operators/scan_translator.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <cctype>

// String interning dictionary: string → int64_t ID (1-based)
struct StringDict {
    std::unordered_map<std::string, int64_t> strToId;
    std::vector<std::string> idToStr;  // idToStr[id-1] = string

    int64_t intern(const std::string& s) {
        auto it = strToId.find(s);
        if (it != strToId.end()) return it->second;
        int64_t id = static_cast<int64_t>(idToStr.size()) + 1;
        strToId[s] = id;
        idToStr.push_back(s);
        return id;
    }

    const std::string& lookup(int64_t id) const {
        static const std::string empty = "(null)";
        if (id < 1 || id > (int64_t)idToStr.size()) return empty;
        return idToStr[id - 1];
    }
};

// Result of loading a CSV
struct CSVTable {
    InMemoryTable  table;
    StringDict     strings;   // per-column string dictionaries
    // Which columns are string (non-numeric) type
    std::vector<bool> isStringCol;
};

// Trim whitespace from both ends
static inline std::string csvTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\"");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n\"");
    return s.substr(a, b - a + 1);
}

// Parse one CSV line into fields, handling quoted fields with commas.
static inline std::vector<std::string> csvSplitLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool inQuote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (inQuote && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"'; ++i;  // escaped quote
            } else {
                inQuote = !inQuote;
            }
        } else if (c == ',' && !inQuote) {
            fields.push_back(csvTrim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    fields.push_back(csvTrim(cur));
    return fields;
}

// Try to parse a string as int64_t. Returns true on success.
static inline bool tryParseInt(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    size_t start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (start == s.size()) return false;
    for (size_t i = start; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return false;
    try {
        out = std::stoll(s);
        return true;
    } catch (...) {
        return false;
    }
}

// Load a CSV file. The first row must be a header.
// tableName: the SQL table name to use.
// Returns a CSVTable with the loaded data.
inline CSVTable loadCSV(const std::string& path, const std::string& tableName) {
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Cannot open CSV file: " + path);

    CSVTable result;
    InMemoryTable& tbl = result.table;
    tbl.name    = tableName;
    tbl.numRows = 0;

    std::string line;

    // ── Header row ────────────────────────────────────────────────────────────
    if (!std::getline(file, line))
        throw std::runtime_error("CSV file is empty: " + path);

    // Strip BOM if present
    if (line.size() >= 3 &&
        (unsigned char)line[0] == 0xEF &&
        (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF)
        line = line.substr(3);

    std::vector<std::string> headers = csvSplitLine(line);
    size_t numCols = headers.size();
    if (numCols == 0)
        throw std::runtime_error("CSV has no columns: " + path);

    tbl.columnNames = headers;
    tbl.columns.resize(numCols);
    result.isStringCol.resize(numCols, false);

    // ── Data rows — two-pass ──────────────────────────────────────────────────
    // Pass 1: detect column types by scanning the first 200 rows.
    // If any value in a column is non-numeric, it's a string column.
    std::vector<std::vector<std::string>> rawRows;
    rawRows.reserve(4096);

    size_t typeCheckRows = 0;
    std::vector<bool> sawNonInt(numCols, false);

    while (std::getline(file, line)) {
        if (line.empty() || line == "\r") continue;
        auto fields = csvSplitLine(line);
        // Pad or truncate to numCols
        fields.resize(numCols);
        rawRows.push_back(std::move(fields));

        if (typeCheckRows < 200) {
            for (size_t c = 0; c < numCols; ++c) {
                int64_t dummy;
                if (!rawRows.back()[c].empty() &&
                    !tryParseInt(rawRows.back()[c], dummy))
                    sawNonInt[c] = true;
            }
            ++typeCheckRows;
        }
    }

    // Assign column types
    for (size_t c = 0; c < numCols; ++c) {
        result.isStringCol[c] = sawNonInt[c];
        tbl.columnTypes.push_back(SQLType::Integer);
    }

    // Pass 2: fill columns
    tbl.numRows = rawRows.size();
    for (size_t c = 0; c < numCols; ++c)
        tbl.columns[c].resize(tbl.numRows, 0);

    for (size_t row = 0; row < rawRows.size(); ++row) {
        for (size_t c = 0; c < numCols; ++c) {
            const std::string& cell = rawRows[row][c];
            if (result.isStringCol[c]) {
                tbl.columns[c][row] = cell.empty() ? 0
                                                    : result.strings.intern(cell);
            } else {
                int64_t v = 0;
                tryParseInt(cell, v);
                tbl.columns[c][row] = v;
            }
        }
    }

    return result;
}
