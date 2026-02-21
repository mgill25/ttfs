#pragma once
// sql_catalog.hpp - Table registry
//
// Maps table names to InMemoryTable pointers.
// The planner uses this to resolve FROM clauses.

#include "../operators/scan_translator.hpp"
#include <string>
#include <unordered_map>
#include <stdexcept>

class SQLCatalog {
    std::unordered_map<std::string, const InMemoryTable*> tables;

public:
    void registerTable(const InMemoryTable* tbl) {
        tables[tbl->name] = tbl;
    }

    const InMemoryTable* lookup(const std::string& name) const {
        auto it = tables.find(name);
        if (it == tables.end())
            throw std::runtime_error("Unknown table: " + name);
        return it->second;
    }

    bool has(const std::string& name) const {
        return tables.count(name) > 0;
    }
};
