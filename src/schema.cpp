#include "schema.hpp"

#include <string>
#include <utility>
#include <vector>

#include "token.hpp"  // lowercase

Column makeColumn(std::string name, Value::Type type) {
    Column column;
    column.key = lowercase(name);
    column.name = std::move(name);
    column.type = type;
    return column;
}

TableSchema makeTableSchema(std::string name, std::vector<Column> columns) {
    TableSchema schema;
    schema.key = lowercase(name);
    schema.name = std::move(name);
    schema.columns = std::move(columns);
    return schema;
}

int TableSchema::indexOf(const std::string& columnName) const {
    // 两边都过一遍 lowercase 再比。
    // 用 == 直接比列名是个很常见的错，而且只在用户写了
    // `SELECT Name FROM users`（而建表时写的是 name）时才犯，
    // 平时测不出来。
    const std::string key = lowercase(columnName);

    for (size_t i = 0; i < columns.size(); i++) {
        if (columns[i].key == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string TableSchema::columnList() const {
    std::string out;
    for (size_t i = 0; i < columns.size(); i++) {
        if (i > 0) out += ", ";
        out += columns[i].name;
    }
    return out;
}

IndexSchema makeIndexSchema(std::string name, std::string tableName,
                            std::string tableKey, int column,
                            Value::Type columnType) {
    IndexSchema schema;
    schema.key = lowercase(name);
    schema.name = std::move(name);
    schema.tableKey = std::move(tableKey);
    schema.tableName = std::move(tableName);
    schema.column = column;
    schema.columnType = columnType;
    return schema;
}

std::string describeIndex(const IndexSchema& schema,
                          const TableSchema& table) {
    // 列号越界只在文件坏了的时候发生，但还是要说人话，
    // 而不是让调用方去解引用一个空 vector。
    std::string columnName = "?";
    if (schema.column >= 0 &&
        static_cast<size_t>(schema.column) < table.columns.size()) {
        columnName = table.columns[static_cast<size_t>(schema.column)].name;
    }
    return "索引 `" + schema.name + "`（建在 `" + table.name + "` 的 " +
           columnName + " 列上）";
}
