#pragma once
// scan_translator.hpp - TableScan operator translator
//
// Generates a loop that iterates over all rows of an in-memory table.
// For the POC, each column is an int64_t array.
// The translator calls:
//   for i = 0 to numRows:
//     bind each column's IU to a loaded value
//     parent->consume(scope, this)

#include "operator_base.hpp"
#include <string>
#include <vector>

// A simple in-memory table: column-oriented storage of int64_t values
struct InMemoryTable {
    std::string name;
    size_t      numRows;
    std::vector<std::string>   columnNames;
    std::vector<SQLType>        columnTypes;
    std::vector<std::vector<int64_t>> columns;  // columns[col][row]
};

class ScanTranslator : public OperatorTranslator {
    const InMemoryTable* table;
    std::vector<IU*> colIUs;

public:
    ScanTranslator(CompilationContext& ctx,
                   const InMemoryTable* tbl);

    void produce(OperatorTranslator* parent) override;
    void consume(ConsumerScope& scope, OperatorTranslator* child) override {
        // Scan has no child
        (void)scope; (void)child;
    }

    IU* getIU(const std::string& colName) const;

    // Introspection for EXPLAIN / operator tree printing
    const std::string& tableName() const { return table->name; }
    size_t numOutputIUs() const { return colIUs.size(); }
};
