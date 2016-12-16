// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "yb/client/yb_op.h"

#include <assert.h>

#include "yb/client/client.h"
#include "yb/common/encoded_key.h"
#include "yb/common/row.h"
#include "yb/common/wire_protocol.pb.h"
#include "yb/common/redis_protocol.pb.h"
#include "yb/common/ysql_protocol.pb.h"
#include "yb/common/ysql_rowblock.h"

namespace yb {
namespace client {

using std::shared_ptr;
using std::unique_ptr;

RowOperationsPB_Type ToInternalWriteType(YBOperation::Type type) {
  switch (type) {
    case YBOperation::INSERT: return RowOperationsPB_Type_INSERT;
    case YBOperation::UPDATE: return RowOperationsPB_Type_UPDATE;
    case YBOperation::DELETE: return RowOperationsPB_Type_DELETE;
    default: LOG(FATAL) << "Unexpected write operation type: " << type;
  }
}

// WriteOperation --------------------------------------------------------------

YBOperation::YBOperation(const shared_ptr<YBTable>& table)
  : table_(table),
    row_(table->schema().schema_) {
}

YBOperation::~YBOperation() {}

int64_t YBOperation::SizeInBuffer() const {
  const Schema* schema = row_.schema();
  int size = 1; // for the operation type

  // Add size of isset bitmap (always present).
  size += BitmapSize(schema->num_columns());
  // Add size of null bitmap (present if the schema has nullables)
  size += ContiguousRowHelper::null_bitmap_size(*schema);
  // The column data itself:
  for (int i = 0; i < schema->num_columns(); i++) {
    if (row_.IsColumnSet(i) && !row_.IsNull(i)) {
      size += schema->column(i).type_info()->size();
      if (schema->column(i).type_info()->physical_type() == BINARY) {
        ContiguousRow row(schema, row_.row_data_);
        Slice bin;
        memcpy(&bin, row.cell_ptr(i), sizeof(bin));
        size += bin.size();
      }
    }
  }
  return size;
}

// Insert -----------------------------------------------------------------------

YBInsert::YBInsert(const shared_ptr<YBTable>& table) : YBOperation(table) {
}

YBInsert::~YBInsert() {
}

// Update -----------------------------------------------------------------------

YBUpdate::YBUpdate(const shared_ptr<YBTable>& table) : YBOperation(table) {
}

YBUpdate::~YBUpdate() {
}

// Delete -----------------------------------------------------------------------

YBDelete::YBDelete(const shared_ptr<YBTable>& table) : YBOperation(table) {
}

YBDelete::~YBDelete() {
}

// YBRedisWriteOp -----------------------------------------------------------------

YBRedisWriteOp::YBRedisWriteOp(const shared_ptr<YBTable>& table)
    : YBOperation(table), redis_write_request_(new RedisWriteRequestPB()) {
}

YBRedisWriteOp::~YBRedisWriteOp() {}

std::string YBRedisWriteOp::ToString() const {
  return "REDIS_WRITE " + redis_write_request_->set_request().key_value().key();
}

RedisResponsePB* YBRedisWriteOp::mutable_response() {
  if (!redis_response_) {
    redis_response_.reset(new RedisResponsePB());
  }
  return redis_response_.get();
}

// YBRedisReadOp -----------------------------------------------------------------

YBRedisReadOp::YBRedisReadOp(const shared_ptr<YBTable>& table)
    : YBOperation(table), redis_read_request_(new RedisReadRequestPB()) {
}

YBRedisReadOp::~YBRedisReadOp() {}

std::string YBRedisReadOp::ToString() const {
  return "REDIS_READ " + redis_read_request_->get_request().key_value().key();
}

const RedisResponsePB& YBRedisReadOp::response() const {
  // Cannot use CHECK or DCHECK here, or client_samples-test will fail.
  assert(redis_response_ != nullptr);
  return *redis_response_;
}

RedisResponsePB* YBRedisReadOp::mutable_response() {
  if (!redis_response_) {
    redis_response_.reset(new RedisResponsePB());
  }
  return redis_response_.get();
}

// YBSqlOp -----------------------------------------------------------------
YBSqlOp::YBSqlOp(const shared_ptr<YBTable>& table) : YBOperation(table) {
}

YBSqlOp::~YBSqlOp() {
}

// YBSqlWriteOp -----------------------------------------------------------------

YBSqlWriteOp::YBSqlWriteOp(const shared_ptr<YBTable>& table)
    : YBSqlOp(table),
      ysql_write_request_(new YSQLWriteRequestPB()),
      ysql_response_(new YSQLResponsePB()) {
}

YBSqlWriteOp::~YBSqlWriteOp() {}

std::string YBSqlWriteOp::ToString() const {
  return "YSQL_WRITE " + ysql_write_request_->DebugString();
}

Status SetColumn(YBPartialRow* row, const int32 column_id, const YSQLValuePB& value) {
  const auto column_idx = row->schema()->find_column_by_id(ColumnId(column_id));
  CHECK_NE(column_idx, Schema::kColumnNotFound);
  CHECK(value.has_datatype());
  switch (value.datatype()) {
    case INT8:
      return value.has_int8_value() ?
          row->SetInt8(column_idx, static_cast<int8_t>(value.int8_value())) :
          row->SetNull(column_idx);
    case INT16:
      return value.has_int16_value() ?
          row->SetInt16(column_idx, static_cast<int16_t>(value.int32_value())) :
          row->SetNull(column_idx);
    case INT32:
      return value.has_int32_value() ?
          row->SetInt32(column_idx, value.int32_value()) : row->SetNull(column_idx);
    case INT64:
      return value.has_int64_value() ?
          row->SetInt64(column_idx, value.int64_value()) : row->SetNull(column_idx);
    case FLOAT:
      return value.has_float_value() ?
          row->SetFloat(column_idx, value.float_value()) : row->SetNull(column_idx);
    case DOUBLE:
      return value.has_double_value() ?
          row->SetDouble(column_idx, value.double_value()) : row->SetNull(column_idx);
    case STRING:
      return value.has_string_value() ?
          row->SetString(column_idx, Slice(value.string_value())) : row->SetNull(column_idx);
    case BOOL:
      return value.has_bool_value() ?
          row->SetBool(column_idx, value.bool_value()) : row->SetNull(column_idx);
    case TIMESTAMP:
      return value.has_timestamp_value() ?
          row->SetTimestamp(column_idx, value.timestamp_value()) : row->SetNull(column_idx);

    case UINT8:  FALLTHROUGH_INTENDED;
    case UINT16: FALLTHROUGH_INTENDED;
    case UINT32: FALLTHROUGH_INTENDED;
    case UINT64: FALLTHROUGH_INTENDED;
    case BINARY: FALLTHROUGH_INTENDED;
    case UNKNOWN_DATA:
      break;

    // default: fall through
  }

  LOG(ERROR) << "Internal error: unsupported datatype " << value.datatype();
  return STATUS(RuntimeError, "unsupported datatype");
}

Status YBSqlWriteOp::SetKey() {
  // Set the row key from the hashed columns
  for (const auto& column_value : ysql_write_request_->hashed_column_values()) {
    RETURN_NOT_OK(SetColumn(mutable_row(), column_value.column_id(), column_value.value()));
  }
  return Status::OK();
}

// YBSqlReadOp -----------------------------------------------------------------

YBSqlReadOp::YBSqlReadOp(const shared_ptr<YBTable>& table)
    : YBSqlOp(table),
      ysql_read_request_(new YSQLReadRequestPB()),
      ysql_response_(new YSQLResponsePB()),
      rows_data_(new string()) {
}

YBSqlReadOp::~YBSqlReadOp() {}

std::string YBSqlReadOp::ToString() const {
  return "YSQL_READ " + ysql_read_request_->DebugString();
}

Status YBSqlReadOp::SetKey() {
  // Set the row key from the hashed columns
  for (const auto& column_value : ysql_read_request_->hashed_column_values()) {
    RETURN_NOT_OK(SetColumn(mutable_row(), column_value.column_id(), column_value.value()));
  }
  return Status::OK();
}

YSQLRowBlock* YBSqlReadOp::GetRowBlock() const {
  vector<ColumnId> column_ids;
  for (const auto column_id : ysql_read_request_->column_ids()) {
    column_ids.emplace_back(column_id);
  }
  unique_ptr<YSQLRowBlock> rowblock(new YSQLRowBlock(*table_->schema().schema_, column_ids));
  Slice data(*rows_data_);
  rowblock->Deserialize(ysql_read_request_->client(), &data);
  return rowblock.release();
}

}  // namespace client
}  // namespace yb
