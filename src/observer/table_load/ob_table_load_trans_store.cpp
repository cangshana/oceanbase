/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SERVER

#include "observer/table_load/ob_table_load_trans_store.h"
#include "observer/omt/ob_tenant_timezone_mgr.h"
#include "observer/table_load/ob_table_load_autoinc_nextval.h"
#include "observer/table_load/ob_table_load_error_row_handler.h"
#include "observer/table_load/ob_table_load_data_row_handler.h"
#include "storage/direct_load/ob_direct_load_dml_row_handler.h"
#include "observer/table_load/ob_table_load_stat.h"
#include "observer/table_load/ob_table_load_store_ctx.h"
#include "observer/table_load/ob_table_load_table_ctx.h"
#include "observer/table_load/ob_table_load_trans_ctx.h"
#include "observer/table_load/ob_table_load_utils.h"
#include "observer/table_load/ob_table_load_store_table_ctx.h"
#include "sql/engine/cmd/ob_load_data_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/ob_sql_utils.h"
#include "share/ob_autoincrement_service.h"
#include "share/sequence/ob_sequence_cache.h"

namespace oceanbase
{
namespace observer
{
using namespace blocksstable;
using namespace common;
using namespace common::hash;
using namespace share::schema;
using namespace share;
using namespace sql;
using namespace storage;
using namespace table;

/**
 * ObTableLoadTransStore
 */

int ObTableLoadTransStore::init()
{
  int ret = OB_SUCCESS;
  const int32_t session_count = trans_ctx_->ctx_->param_.px_mode_?
                                1 : trans_ctx_->ctx_->param_.write_session_count_;
  SessionStore *session_store = nullptr;
  for (int32_t i = 0; OB_SUCC(ret) && i < session_count; ++i) {
    if (OB_ISNULL(session_store = OB_NEWx(SessionStore, (&trans_ctx_->allocator_)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to new SessionStore", KR(ret));
    } else {
      if (trans_ctx_->ctx_->param_.px_mode_) {
        session_store->session_id_ = (ATOMIC_FAA(&(trans_ctx_->ctx_->store_ctx_->next_session_id_), 1) % trans_ctx_->ctx_->param_.write_session_count_) + 1;
      } else {
        session_store->session_id_ = i + 1;
      }
      if (OB_FAIL(session_store_array_.push_back(session_store))) {
        LOG_WARN("fail to push back session store", KR(ret));
      }
    }
    if (OB_FAIL(ret)) {
      if (nullptr != session_store) {
        session_store->~SessionStore();
        trans_ctx_->allocator_.free(session_store);
        session_store = nullptr;
      }
    }
  }
  return ret;
}

void ObTableLoadTransStore::reset()
{
  for (int64_t i = 0; i < session_store_array_.count(); ++i) {
    SessionStore *session_store = session_store_array_.at(i);
    // free partition tables
    for (int64_t j = 0; j < session_store->partition_table_array_.count(); ++j) {
      ObIDirectLoadPartitionTable *table = session_store->partition_table_array_.at(j);
      table->~ObIDirectLoadPartitionTable();
      session_store->allocator_.free(table);
    }
    session_store->partition_table_array_.reset();
    // free session_store
    session_store->~SessionStore();
    trans_ctx_->allocator_.free(session_store);
  }
  session_store_array_.reset();
}

/**
 * ObTableLoadTransStoreWriter
 */

ObTableLoadTransStoreWriter::SessionContext::SessionContext(int32_t session_id, uint64_t tenant_id, ObDataTypeCastParams cast_params)
  : session_id_(session_id),
    cast_allocator_("TLD_TS_Caster"),
    cast_params_(cast_params),
    last_receive_sequence_no_(0),
    extra_buf_(nullptr),
    extra_buf_size_(0)
{
  cast_allocator_.set_tenant_id(MTL_ID());
}

ObTableLoadTransStoreWriter::SessionContext::~SessionContext()
{
  datum_row_.reset();
}

ObTableLoadTransStoreWriter::ObTableLoadTransStoreWriter(ObTableLoadTransStore *trans_store)
  : trans_store_(trans_store),
    trans_ctx_(trans_store->trans_ctx_),
    store_ctx_(trans_ctx_->ctx_->store_ctx_),
    param_(trans_ctx_->ctx_->param_),
    allocator_("TLD_TSWriter"),
    table_data_desc_(nullptr),
    cast_mode_(CM_NONE),
    lob_inrow_threshold_(0),
    ref_count_(0),
    is_inited_(false)
{
  allocator_.set_tenant_id(MTL_ID());
  column_schemas_.set_tenant_id(MTL_ID());
}

ObTableLoadTransStoreWriter::~ObTableLoadTransStoreWriter()
{
  if (nullptr != session_ctx_array_) {
    int32_t session_count = param_.px_mode_? 1 : param_.write_session_count_;
    for (int64_t i = 0; i < session_count; ++i) {
      SessionContext *session_ctx = session_ctx_array_ + i;
      if (OB_NOT_NULL(session_ctx->extra_buf_)) {
        allocator_.free(session_ctx->extra_buf_);
        session_ctx->extra_buf_ = nullptr;
      }
      session_ctx->~SessionContext();
    }
    allocator_.free(session_ctx_array_);
    session_ctx_array_ = nullptr;
  }
}

int ObTableLoadTransStoreWriter::init()
{
  int ret = OB_SUCCESS;
  int32_t session_count = param_.px_mode_? 1 : param_.write_session_count_;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObTableLoadTransStoreWriter init twice", KR(ret), KP(this));
  } else if (OB_UNLIKELY(trans_store_->session_store_array_.count() != session_count)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), KPC(trans_store_));
  } else {
    table_data_desc_ = &store_ctx_->data_store_table_ctx_->table_data_desc_;
    collation_type_ = store_ctx_->data_store_table_ctx_->schema_->collation_type_;
    if (OB_FAIL(ObSQLUtils::get_default_cast_mode(store_ctx_->ctx_->session_info_, cast_mode_))) {
      LOG_WARN("fail to get_default_cast_mode", KR(ret));
    } else if (OB_FAIL(init_session_ctx_array())) {
      LOG_WARN("fail to init session ctx array", KR(ret));
    } else if (OB_FAIL(init_column_schemas_and_lob_info())) {
      LOG_WARN("fail to init column schemas and lob info", KR(ret));
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::init_column_schemas_and_lob_info()
{
  int ret = OB_SUCCESS;
  const ObIArray<ObColDesc> &column_descs = store_ctx_->data_store_table_ctx_->schema_->column_descs_;
  const ObTableSchema *table_schema = nullptr;
  if (OB_FAIL(ObTableLoadSchema::get_table_schema(param_.tenant_id_, param_.table_id_, schema_guard_,
                                                  table_schema))) {
    LOG_WARN("fail to get table schema", KR(ret), K(param_));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_descs.count(); ++i) {
    const ObColumnSchemaV2 *column_schema =
      table_schema->get_column_schema(column_descs.at(i).col_id_);
    if (column_schema->is_hidden()) {
    } else if (OB_FAIL(column_schemas_.push_back(column_schema))) {
      LOG_WARN("failed to push back column schema", K(ret), K(i), KPC(column_schema));
    }
  }
  if (OB_SUCC(ret)) {
    lob_inrow_threshold_ = table_schema->get_lob_inrow_threshold();
  }
  return ret;
}

int ObTableLoadTransStoreWriter::init_session_ctx_array()
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  int32_t session_count = param_.px_mode_? 1 : param_.write_session_count_;
  ObDataTypeCastParams cast_params(trans_ctx_->ctx_->session_info_->get_timezone_info());
  if (OB_ISNULL(buf = allocator_.alloc(sizeof(SessionContext) * session_count))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to allocate memory", KR(ret));
  } else if (OB_FAIL(time_cvrt_.init(cast_params.get_nls_format(ObDateTimeType)))) {
    LOG_WARN("fail to init time converter", KR(ret));
  } else {
    session_ctx_array_ = static_cast<SessionContext *>(buf);
    for (int64_t i = 0; i < session_count; ++i) {
      new (session_ctx_array_ + i)
        SessionContext(i + 1, param_.tenant_id_, cast_params);
    }
  }
  ObDirectLoadTableStoreParam param;
  param.table_data_desc_ = *table_data_desc_;
  param.datum_utils_ = &(store_ctx_->data_store_table_ctx_->schema_->datum_utils_);
  param.file_mgr_ = store_ctx_->tmp_file_mgr_;
  param.is_multiple_mode_ = store_ctx_->data_store_table_ctx_->is_multiple_mode_;
  param.is_fast_heap_table_ = store_ctx_->data_store_table_ctx_->is_fast_heap_table_;
  param.insert_table_ctx_ = store_ctx_->data_store_table_ctx_->insert_table_ctx_;
  param.dml_row_handler_ = store_ctx_->data_store_table_ctx_->row_handler_;
  for (int64_t i = 0; OB_SUCC(ret) && i < session_count; ++i) {
    SessionContext *session_ctx = session_ctx_array_ + i;
    if (param_.px_mode_) {
      session_ctx->extra_buf_size_ = table_data_desc_->extra_buf_size_;
      if (OB_ISNULL(session_ctx->extra_buf_ =
                      static_cast<char *>(allocator_.alloc(session_ctx->extra_buf_size_)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc memory", KR(ret));
      } else {
        param.extra_buf_ = session_ctx->extra_buf_;
        param.extra_buf_size_ = session_ctx->extra_buf_size_;
      }
    } else {
      param.extra_buf_ = store_ctx_->session_ctx_array_[i].extra_buf_;
      param.extra_buf_size_ = store_ctx_->session_ctx_array_[i].extra_buf_size_;
    }
    if (OB_SUCC(ret)) {
      // init table_store_
      if (OB_FAIL(session_ctx->table_store_.init(param))) {
        LOG_WARN("fail to init table store", KR(ret));
      }
      // init datum_row_
      else if (OB_FAIL(session_ctx->datum_row_.init(table_data_desc_->column_count_))) {
        LOG_WARN("fail to init datum row", KR(ret));
      } else {
        session_ctx->datum_row_.row_flag_.set_flag(ObDmlFlag::DF_INSERT);
        session_ctx->datum_row_.mvcc_row_flag_.set_last_multi_version_row(true);
      }
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::advance_sequence_no(int32_t session_id, uint64_t sequence_no,
                                                     ObTableLoadMutexGuard &guard)
{
  int ret = OB_SUCCESS;
  int32_t session_count = param_.px_mode_? 1 : param_.write_session_count_;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTransStoreWriter not init", KR(ret));
  } else if (OB_UNLIKELY(session_id < 1 || session_id > session_count)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(session_id));
  } else {
    SessionContext &session_ctx = session_ctx_array_[session_id - 1];
    if (OB_UNLIKELY(sequence_no != session_ctx.last_receive_sequence_no_ + 1)) {
      if (OB_UNLIKELY(sequence_no != session_ctx.last_receive_sequence_no_)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid sequence no", KR(ret), K(sequence_no),
                 K(session_ctx.last_receive_sequence_no_));
      } else {
        ret = OB_ENTRY_EXIST;
      }
    } else {
      session_ctx.last_receive_sequence_no_ = sequence_no;
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::write(int32_t session_id,
                                       const ObTableLoadTabletObjRowArray &row_array)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTransStoreWriter not init", KR(ret));
  } else if (OB_UNLIKELY(session_id < 1 || session_id > param_.write_session_count_) ||
             row_array.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(session_id), K(row_array.empty()));
  } else {
    SessionContext &session_ctx = session_ctx_array_[session_id - 1];
    for (int64_t i = 0; OB_SUCC(ret) && i < row_array.count(); ++i) {
      const ObTableLoadTabletObjRow &row = row_array.at(i);
      ObNewRow new_row(row.obj_row_.cells_, row.obj_row_.count_);
      if (OB_FAIL(cast_row(session_ctx.cast_allocator_, session_ctx.cast_params_, new_row, session_ctx.datum_row_,
                           session_id))) {
        if (OB_UNLIKELY(OB_EAGAIN != ret)) {
          LOG_WARN("fail to cast row", KR(ret), K(session_id), K(row.tablet_id_), K(i));
        } else {
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(write_row_to_table_store(session_ctx.table_store_, row.tablet_id_, row.obj_row_.seq_no_, session_ctx.datum_row_))) {
        LOG_WARN("fail to write row", KR(ret), K(session_id), K(row.tablet_id_), K(i));
      }
    }
    if (OB_SUCC(ret)) {
      ATOMIC_AAF(&trans_ctx_->ctx_->job_stat_->store_.processed_rows_, row_array.count());
    }
    session_ctx.cast_allocator_.reuse();
  }
  return ret;
}

int ObTableLoadTransStoreWriter::px_write(const ObTabletID &tablet_id, const blocksstable::ObDatumRow &row)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTransStoreWriter not init", KR(ret));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || !row.is_valid() ||
                         row.count_ != table_data_desc_->column_count_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(tablet_id), K(row), KPC(table_data_desc_));
  } else {
    ObTableLoadSequenceNo seq_no(0); // pdml导入的行目前不存在主键冲突，先都用一个默认的seq_no
    SessionContext &session_ctx = session_ctx_array_[0];
    if (OB_FAIL(write_row_to_table_store(session_ctx.table_store_,
                                          tablet_id,
                                          seq_no,
                                          row))) {
      LOG_WARN("fail to write row", KR(ret), K(tablet_id), K(row));
    } else {
      ATOMIC_AAF(&trans_ctx_->ctx_->job_stat_->store_.processed_rows_, 1);
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::flush(int32_t session_id)
{
  int ret = OB_SUCCESS;
  int32_t session_count = param_.px_mode_? 1 : param_.write_session_count_;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTransStoreWriter not init", KR(ret));
  } else if (OB_UNLIKELY(session_id < 1 || session_id > session_count)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(session_id));
  } else {
    SessionContext &session_ctx = session_ctx_array_[session_id - 1];
    ObTableLoadTransStore::SessionStore *session_store =
      trans_store_->session_store_array_.at(session_id - 1);
    if (OB_FAIL(session_ctx.table_store_.close())) {
      LOG_WARN("fail to close table store", KR(ret), K(session_id));
    } else if (OB_FAIL(session_ctx.table_store_.get_tables(session_store->partition_table_array_,
                                                           session_store->allocator_))) {
      LOG_WARN("fail to get tables", KR(ret));
    } else {
      session_ctx.table_store_.clean_up();
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::clean_up(int32_t session_id)
{
  int ret = OB_SUCCESS;
  int32_t session_count = param_.px_mode_? 1 : param_.write_session_count_;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTransStoreWriter not init", KR(ret));
  } else if (OB_UNLIKELY(session_id < 1 || session_id > session_count)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(session_id));
  } else {
    SessionContext &session_ctx = session_ctx_array_[session_id - 1];
    session_ctx.table_store_.clean_up();
  }
  return ret;
}

int ObTableLoadTransStoreWriter::cast_row(ObArenaAllocator &cast_allocator,
                                          ObDataTypeCastParams cast_params, const ObNewRow &row,
                                          ObDatumRow &datum_row, int32_t session_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(row.count_ != table_data_desc_->column_count_)) {
    ret = OB_ERR_INVALID_COLUMN_NUM;
    LOG_WARN("column count not match", KR(ret), K(row.count_), K(table_data_desc_->column_count_));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table_data_desc_->column_count_; ++i) {
    const ObColumnSchemaV2 *column_schema = column_schemas_.at(i);
    const ObObj &obj = row.cells_[i];
    ObStorageDatum &datum = datum_row.storage_datums_[i];
    if (OB_FAIL(cast_column(cast_allocator, cast_params, column_schema, obj, datum, session_id))) {
      LOG_WARN("fail to cast column", KR(ret), K(i), K(obj), KPC(column_schema));
    }
  }
  if (OB_FAIL(ret)) {
    ObTableLoadErrorRowHandler *error_row_handler =
      trans_ctx_->ctx_->store_ctx_->error_row_handler_;
    if (OB_FAIL(error_row_handler->handle_error_row(ret))) {
      LOG_WARN("failed to handle error row", K(ret), K(row));
    } else {
      ret = OB_EAGAIN;
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::cast_row(int32_t session_id,
                                          const ObNewRow &new_row,
                                          ObDatumRow &datum_row)
{
  int ret = OB_SUCCESS;
  SessionContext &session_ctx = session_ctx_array_[session_id - 1];
  session_ctx.cast_allocator_.reuse();
  if (OB_FAIL(cast_row(session_ctx.cast_allocator_, session_ctx.cast_params_, new_row, datum_row,
                        session_id))) {
    if (OB_UNLIKELY(OB_EAGAIN != ret)) {
      LOG_WARN("fail to cast row", KR(ret), K(session_id));
    } else {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::cast_column(
    ObArenaAllocator &cast_allocator,
    ObDataTypeCastParams cast_params,
    const ObColumnSchemaV2 *column_schema,
    const ObObj &obj,
    ObStorageDatum &datum,
    int32_t session_id)
{
  int ret = OB_SUCCESS;
  ObCastCtx cast_ctx(&cast_allocator, &cast_params, cast_mode_, column_schema->get_collation_type());
  ObTableLoadCastObjCtx cast_obj_ctx(param_, &time_cvrt_, &cast_ctx, true);
  ObObj out_obj;
  if (column_schema->is_autoincrement()) {
    if (obj.is_null() || obj.is_nop_value()) {
      out_obj = obj;
    } else if (OB_FAIL(ObTableLoadObjCaster::cast_obj(cast_obj_ctx,
                                                      column_schema,
                                                      obj,
                                                      out_obj))) {
      LOG_WARN("fail to cast obj", KR(ret), K(obj), KPC(column_schema));
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(handle_autoinc_column(column_schema, out_obj, datum, session_id))) {
        LOG_WARN("fail to handle autoinc column", KR(ret), K(out_obj));
      }
    }
  } else if (column_schema->is_identity_column()) {
    if (column_schema->is_tbl_part_key_column()) {
      // 自增列是分区键, 在分区计算的时候就已经确定值了
      out_obj = obj;
    } else {
      // 生成的seq_value是number, 可能需要转换成decimal int
      ObObj tmp_obj;
      if (OB_FAIL(handle_identity_column(column_schema, obj, tmp_obj, cast_allocator))) {
        LOG_WARN("fail to handle identity column", KR(ret), K(obj));
      } else if (OB_FAIL(ObTableLoadObjCaster::cast_obj(cast_obj_ctx, column_schema, tmp_obj, out_obj))) {
        LOG_WARN("fail to cast obj and check", KR(ret), K(tmp_obj));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(datum.from_obj_enhance(out_obj))) {
        LOG_WARN("fail to from obj enhance", KR(ret), K(out_obj));
      }
    }
  } else {
    // 普通列
    if (OB_FAIL(ObTableLoadObjCaster::cast_obj(cast_obj_ctx, column_schema, obj, out_obj))) {
      LOG_WARN("fail to cast obj and check", KR(ret), K(obj));
    } else if (OB_FAIL(datum.from_obj_enhance(out_obj))) {
      LOG_WARN("fail to from obj enhance", KR(ret), K(out_obj));
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::handle_autoinc_column(const ObColumnSchemaV2 *column_schema,
                                                       const ObObj &obj,
                                                       ObStorageDatum &datum,
                                                       int32_t session_id)
{
  int ret = OB_SUCCESS;
  const ObObjTypeClass &tc = column_schema->get_meta_type().get_type_class();
  if (OB_FAIL(datum.from_obj_enhance(obj))) {
    LOG_WARN("fail to from obj enhance", KR(ret), K(obj));
  } else if (OB_FAIL(ObTableLoadAutoincNextval::eval_nextval(
        &(store_ctx_->session_ctx_array_[session_id - 1].autoinc_param_), datum, tc,
        store_ctx_->ctx_->session_info_->get_sql_mode()))) {
    LOG_WARN("fail to get auto increment next value", KR(ret));
  }
  return ret;
}

int ObTableLoadTransStoreWriter::handle_identity_column(const ObColumnSchemaV2 *column_schema,
                                                        const ObObj &obj,
                                                        ObObj &out_obj,
                                                        ObArenaAllocator &cast_allocator)
{
  int ret = OB_SUCCESS;
  // 1. generated always as identity : 不能指定此列导入
  // 2. generated by default as identity : 不指定时自动生成, 不能导入null
  // 3. generated by default on null as identity : 不指定或者指定null会自动生成
  if (OB_UNLIKELY(column_schema->is_always_identity_column() && !obj.is_nop_value())) {
    ret = OB_ERR_INSERT_INTO_GENERATED_ALWAYS_IDENTITY_COLUMN;
    LOG_USER_ERROR(OB_ERR_INSERT_INTO_GENERATED_ALWAYS_IDENTITY_COLUMN);
  } else if (OB_UNLIKELY(column_schema->is_default_identity_column() && obj.is_null())) {
    ret = OB_BAD_NULL_ERROR;
    LOG_WARN("default identity column cannot insert null", KR(ret));
  } else {
    // 不论用户有没有指定自增列的值, 都取一个seq_value, 行为与insert into保持一致
    // 取seq_value的性能受表的参数cache影响
    ObSequenceValue seq_value;
    if (OB_FAIL(ObSequenceCache::get_instance().nextval(trans_ctx_->ctx_->store_ctx_->sequence_schema_,
                                                        cast_allocator,
                                                        seq_value))) {
      LOG_WARN("fail get nextval for seq", KR(ret));
    } else if (obj.is_nop_value() || obj.is_null()) {
      ObNumber number;
      if (OB_FAIL(number.from(seq_value.val(), cast_allocator))) {
        LOG_WARN("fail deep copy value", KR(ret), K(seq_value));
      } else {
        out_obj.set_number(number);
      }
    } else {
      out_obj = obj;
    }
  }
  return ret;
}

int ObTableLoadTransStoreWriter::write_row_to_table_store(ObDirectLoadTableStore &table_store,
                                                          const ObTabletID &tablet_id,
                                                          const ObTableLoadSequenceNo &seq_no,
                                                          const ObDatumRow &datum_row)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(table_store.append_row(tablet_id, seq_no, datum_row))) {
    LOG_WARN("fail to append row", KR(ret), K(datum_row));
  }
  if (OB_FAIL(ret)) {
    ObTableLoadErrorRowHandler *error_row_handler =
      trans_ctx_->ctx_->store_ctx_->error_row_handler_;
    ObDirectLoadDMLRowHandler *data_row_handler = trans_ctx_->ctx_->store_ctx_->data_store_table_ctx_->row_handler_;
    if (OB_LIKELY(OB_ERR_PRIMARY_KEY_DUPLICATE == ret)) {
      if (OB_FAIL(data_row_handler->handle_update_row(datum_row))) {
        LOG_WARN("fail to handle update row", KR(ret), K(datum_row));
      }
    } else if (OB_LIKELY(OB_ROWKEY_ORDER_ERROR == ret)) {
      if (OB_FAIL(error_row_handler->handle_error_row(ret))) {
        LOG_WARN("fail to handle error row", KR(ret), K(tablet_id), K(datum_row));
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
