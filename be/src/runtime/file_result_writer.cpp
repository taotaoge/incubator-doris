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

#include "runtime/file_result_writer.h"

#include "exec/broker_writer.h"
#include "exec/local_file_writer.h"
#include "exec/parquet_writer.h"
#include "exec/s3_writer.h"
#include "exec/hdfs_writer.h"
#include "exprs/expr.h"
#include "exprs/expr_context.h"
#include "gen_cpp/PaloInternalService_types.h"
#include "runtime/buffer_control_block.h"
#include "runtime/primitive_type.h"
#include "runtime/raw_value.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"
#include "runtime/tuple_row.h"
#include "runtime/string_value.h"
#include "service/backend_options.h"
#include "util/date_func.h"
#include "util/file_utils.h"
#include "util/mysql_row_buffer.h"
#include "util/types.h"
#include "util/uid_util.h"

namespace doris {

const size_t FileResultWriter::OUTSTREAM_BUFFER_SIZE_BYTES = 1024 * 1024;

// deprecated
FileResultWriter::FileResultWriter(const ResultFileOptions* file_opts,
                                   const std::vector<ExprContext*>& output_expr_ctxs,
                                   RuntimeProfile* parent_profile, BufferControlBlock* sinker)
        : _file_opts(file_opts),
          _output_expr_ctxs(output_expr_ctxs),
          _parent_profile(parent_profile),
          _sinker(sinker) {
        if (_file_opts->is_local_file) {
            _storage_type = TStorageBackendType::LOCAL;
        } else {
            _storage_type = TStorageBackendType::BROKER;
        }
        // The new file writer needs to use fragment instance id as part of the file prefix.
        // But during the upgrade process, the old version of fe will be called to the new version of be,
        // resulting in no such attribute. So we need a mock here.
        _fragment_instance_id.hi = 12345678987654321;
        _fragment_instance_id.lo = 98765432123456789;
    }

FileResultWriter::FileResultWriter(const ResultFileOptions* file_opts,
                                   const TStorageBackendType::type storage_type,
                                   const TUniqueId fragment_instance_id,
                                   const std::vector<ExprContext*>& output_expr_ctxs,
                                   RuntimeProfile* parent_profile, BufferControlBlock* sinker,
                                   RowBatch* output_batch)
        : _file_opts(file_opts),
          _storage_type(storage_type),
          _fragment_instance_id(fragment_instance_id),
          _output_expr_ctxs(output_expr_ctxs),
          _parent_profile(parent_profile),
          _sinker(sinker),
          _output_batch(output_batch) {}

FileResultWriter::~FileResultWriter() {
    _close_file_writer(true);
}

Status FileResultWriter::init(RuntimeState* state) {
    _state = state;
    _init_profile();
    return _create_next_file_writer();
}

void FileResultWriter::_init_profile() {
    RuntimeProfile* profile = _parent_profile->create_child("FileResultWriter", true, true);
    _append_row_batch_timer = ADD_TIMER(profile, "AppendBatchTime");
    _convert_tuple_timer = ADD_CHILD_TIMER(profile, "TupleConvertTime", "AppendBatchTime");
    _file_write_timer = ADD_CHILD_TIMER(profile, "FileWriteTime", "AppendBatchTime");
    _writer_close_timer = ADD_TIMER(profile, "FileWriterCloseTime");
    _written_rows_counter = ADD_COUNTER(profile, "NumWrittenRows", TUnit::UNIT);
    _written_data_bytes = ADD_COUNTER(profile, "WrittenDataBytes", TUnit::BYTES);
}

Status FileResultWriter::_create_success_file() {
    std::string file_name;
    RETURN_IF_ERROR(_get_success_file_name(&file_name));
    RETURN_IF_ERROR(_create_file_writer(file_name));
    return _close_file_writer(true, true);
}

Status FileResultWriter::_get_success_file_name(std::string* file_name) {
    std::stringstream ss;
    ss << _file_opts->file_path << _file_opts->success_file_name;
    *file_name = ss.str();
    if (_storage_type == TStorageBackendType::LOCAL) {
        // For local file writer, the file_path is a local dir.
        // Here we do a simple security verification by checking whether the file exists.
        // Because the file path is currently arbitrarily specified by the user,
        // Doris is not responsible for ensuring the correctness of the path.
        // This is just to prevent overwriting the existing file.
        if (FileUtils::check_exist(*file_name)) {
            return Status::InternalError("File already exists: " + *file_name +
                                         ". Host: " + BackendOptions::get_localhost());
        }
    }

    return Status::OK();
}

Status FileResultWriter::_create_next_file_writer() {
    std::string file_name;
    RETURN_IF_ERROR(_get_next_file_name(&file_name));
    return _create_file_writer(file_name);
}

Status FileResultWriter::_create_file_writer(const std::string& file_name) {
    if (_storage_type == TStorageBackendType::LOCAL) {
        _file_writer = new LocalFileWriter(file_name, 0 /* start offset */);
    } else if (_storage_type == TStorageBackendType::BROKER) {
        _file_writer =
                new BrokerWriter(_state->exec_env(), _file_opts->broker_addresses,
                                 _file_opts->broker_properties, file_name, 0 /*start offset*/);
    } else if (_storage_type == TStorageBackendType::S3) {
        _file_writer =  new S3Writer(_file_opts->broker_properties, file_name, 0 /* offset */);
    } else if (_storage_type == TStorageBackendType::HDFS) {
        _file_writer = new HDFSWriter(const_cast<std::map<std::string, std::string>&>(_file_opts->broker_properties), file_name);
    }
    RETURN_IF_ERROR(_file_writer->open());
    switch (_file_opts->file_format) {
    case TFileFormatType::FORMAT_CSV_PLAIN:
        // just use file writer is enough
        break;
    case TFileFormatType::FORMAT_PARQUET:
        _parquet_writer = new ParquetWriterWrapper(_file_writer, _output_expr_ctxs,
                                                   _file_opts->file_properties, _file_opts->schema);
        break;
    default:
        return Status::InternalError(
                strings::Substitute("unsupported file format: $0", _file_opts->file_format));
    }
    LOG(INFO) << "create file for exporting query result. file name: " << file_name
              << ". query id: " << print_id(_state->query_id())
              << " format:" << _file_opts->file_format;
    return Status::OK();
}

// file name format as: my_prefix_{fragment_instance_id}_0.csv
Status FileResultWriter::_get_next_file_name(std::string* file_name) {
    std::stringstream ss;
    ss << _file_opts->file_path << print_id(_fragment_instance_id)
       << "_" << (_file_idx++) << "." << _file_format_to_name();
    *file_name = ss.str();
    if (_storage_type == TStorageBackendType::LOCAL) {
        // For local file writer, the file_path is a local dir.
        // Here we do a simple security verification by checking whether the file exists.
        // Because the file path is currently arbitrarily specified by the user,
        // Doris is not responsible for ensuring the correctness of the path.
        // This is just to prevent overwriting the existing file.
        if (FileUtils::check_exist(*file_name)) {
            return Status::InternalError("File already exists: " + *file_name +
                                         ". Host: " + BackendOptions::get_localhost());
        }
    }

    return Status::OK();
}

// file url format as:
// LOCAL: file:///localhost_address/{file_path}{fragment_instance_id}_
// S3: {file_path}{fragment_instance_id}_
// BROKER: {file_path}{fragment_instance_id}_

Status FileResultWriter::_get_file_url(std::string* file_url) {
    std::stringstream ss;
    if (_storage_type == TStorageBackendType::LOCAL) {
        ss << "file:///" << BackendOptions::get_localhost();
    }
    ss << _file_opts->file_path;
    ss << print_id(_fragment_instance_id) << "_";
    *file_url = ss.str();
    return Status::OK();
}

std::string FileResultWriter::_file_format_to_name() {
    switch (_file_opts->file_format) {
    case TFileFormatType::FORMAT_CSV_PLAIN:
        return "csv";
    case TFileFormatType::FORMAT_PARQUET:
        return "parquet";
    default:
        return "unknown";
    }
}

Status FileResultWriter::append_row_batch(const RowBatch* batch) {
    if (nullptr == batch || 0 == batch->num_rows()) {
        return Status::OK();
    }

    SCOPED_TIMER(_append_row_batch_timer);
    if (_parquet_writer != nullptr) {
        RETURN_IF_ERROR(_write_parquet_file(*batch));
    } else {
        RETURN_IF_ERROR(_write_csv_file(*batch));
    }

    _written_rows += batch->num_rows();
    return Status::OK();
}

Status FileResultWriter::_write_parquet_file(const RowBatch& batch) {
    RETURN_IF_ERROR(_parquet_writer->write(batch));
    // split file if exceed limit
    return _create_new_file_if_exceed_size();
}

Status FileResultWriter::_write_csv_file(const RowBatch& batch) {
    int num_rows = batch.num_rows();
    for (int i = 0; i < num_rows; ++i) {
        TupleRow* row = batch.get_row(i);
        RETURN_IF_ERROR(_write_one_row_as_csv(row));
    }
    return _flush_plain_text_outstream(true);
}

// actually, this logic is same as `ExportSink::gen_row_buffer`
// TODO(cmy): find a way to unify them.
Status FileResultWriter::_write_one_row_as_csv(TupleRow* row) {
    {
        SCOPED_TIMER(_convert_tuple_timer);
        int num_columns = _output_expr_ctxs.size();
        for (int i = 0; i < num_columns; ++i) {
            void* item = _output_expr_ctxs[i]->get_value(row);

            if (item == nullptr) {
                _plain_text_outstream << NULL_IN_CSV;
                if (i < num_columns - 1) {
                    _plain_text_outstream << _file_opts->column_separator;
                }
                continue;
            }

            switch (_output_expr_ctxs[i]->root()->type().type) {
            case TYPE_BOOLEAN:
            case TYPE_TINYINT:
                _plain_text_outstream << (int)*static_cast<int8_t*>(item);
                break;
            case TYPE_SMALLINT:
                _plain_text_outstream << *static_cast<int16_t*>(item);
                break;
            case TYPE_INT:
                _plain_text_outstream << *static_cast<int32_t*>(item);
                break;
            case TYPE_BIGINT:
                _plain_text_outstream << *static_cast<int64_t*>(item);
                break;
            case TYPE_LARGEINT:
                _plain_text_outstream << reinterpret_cast<PackedInt128*>(item)->value;
                break;
            case TYPE_FLOAT: {
                char buffer[MAX_FLOAT_STR_LENGTH + 2];
                float float_value = *static_cast<float*>(item);
                buffer[0] = '\0';
                int length = FloatToBuffer(float_value, MAX_FLOAT_STR_LENGTH, buffer);
                DCHECK(length >= 0) << "gcvt float failed, float value=" << float_value;
                _plain_text_outstream << buffer;
                break;
            }
            case TYPE_DOUBLE: {
                // To prevent loss of precision on float and double types,
                // they are converted to strings before output.
                // For example: For a double value 27361919854.929001,
                // the direct output of using std::stringstream is 2.73619e+10,
                // and after conversion to a string, it outputs 27361919854.929001
                char buffer[MAX_DOUBLE_STR_LENGTH + 2];
                double double_value = *static_cast<double*>(item);
                buffer[0] = '\0';
                int length = DoubleToBuffer(double_value, MAX_DOUBLE_STR_LENGTH, buffer);
                DCHECK(length >= 0) << "gcvt double failed, double value=" << double_value;
                _plain_text_outstream << buffer;
                break;
            }
            case TYPE_DATE:
            case TYPE_DATETIME: {
                char buf[64];
                const DateTimeValue* time_val = (const DateTimeValue*)(item);
                time_val->to_string(buf);
                _plain_text_outstream << buf;
                break;
            }
            case TYPE_VARCHAR:
            case TYPE_CHAR:
            case TYPE_STRING: {
                const StringValue* string_val = (const StringValue*)(item);
                if (string_val->ptr == NULL) {
                    if (string_val->len != 0) {
                        _plain_text_outstream << NULL_IN_CSV;
                    }
                } else {
                    _plain_text_outstream << std::string(string_val->ptr, string_val->len);
                }
                break;
            }
            case TYPE_DECIMALV2: {
                const DecimalV2Value decimal_val(
                        reinterpret_cast<const PackedInt128*>(item)->value);
                std::string decimal_str;
                int output_scale = _output_expr_ctxs[i]->root()->output_scale();
                decimal_str = decimal_val.to_string(output_scale);
                _plain_text_outstream << decimal_str;
                break;
            }
            default: {
                // not supported type, like BITMAP, HLL, just export null
                _plain_text_outstream << NULL_IN_CSV;
            }
            }
            if (i < num_columns - 1) {
                _plain_text_outstream << _file_opts->column_separator;
            }
        } // end for columns
        _plain_text_outstream << _file_opts->line_delimiter;
    }

    // write one line to file
    return _flush_plain_text_outstream(false);
}

Status FileResultWriter::_flush_plain_text_outstream(bool eos) {
    SCOPED_TIMER(_file_write_timer);
    size_t pos = _plain_text_outstream.tellp();
    if (pos == 0 || (pos < OUTSTREAM_BUFFER_SIZE_BYTES && !eos)) {
        return Status::OK();
    }

    const std::string& buf = _plain_text_outstream.str();
    size_t written_len = 0;
    RETURN_IF_ERROR(_file_writer->write(reinterpret_cast<const uint8_t*>(buf.c_str()), buf.size(),
                                        &written_len));
    COUNTER_UPDATE(_written_data_bytes, written_len);
    _current_written_bytes += written_len;

    // clear the stream
    _plain_text_outstream.str("");
    _plain_text_outstream.clear();

    // split file if exceed limit
    return _create_new_file_if_exceed_size();
}

Status FileResultWriter::_create_new_file_if_exceed_size() {
    if (_current_written_bytes < _file_opts->max_file_size_bytes) {
        return Status::OK();
    }
    // current file size exceed the max file size. close this file
    // and create new one
    {
        SCOPED_TIMER(_writer_close_timer);
        RETURN_IF_ERROR(_close_file_writer(false));
    }
    _current_written_bytes = 0;
    return Status::OK();
}

Status FileResultWriter::_close_file_writer(bool done, bool only_close) {
    if (_parquet_writer != nullptr) {
        _parquet_writer->close();
        _current_written_bytes = _parquet_writer->written_len();
        COUNTER_UPDATE(_written_data_bytes, _current_written_bytes);
        delete _parquet_writer;
        _parquet_writer = nullptr;
        delete _file_writer;
        _file_writer = nullptr;
    } else if (_file_writer != nullptr) {
        _file_writer->close();
        delete _file_writer;
        _file_writer = nullptr;
    }

    if (only_close) {
        return Status::OK();
    }

    if (!done) {
        // not finished, create new file writer for next file
        RETURN_IF_ERROR(_create_next_file_writer());
    } else {
        // All data is written to file, send statistic result
        if (_file_opts->success_file_name != "") {
            // write success file, just need to touch an empty file
            RETURN_IF_ERROR(_create_success_file());
        }
        if (_output_batch == nullptr) {
            RETURN_IF_ERROR(_send_result());
        } else {
            RETURN_IF_ERROR(_fill_result_batch());
        }
    }
    return Status::OK();
}

Status FileResultWriter::_send_result() {
    if (_is_result_sent) {
        return Status::OK();
    }
    _is_result_sent = true;

    // The final stat result include:
    // FileNumber, TotalRows, FileSize and URL
    // The type of these field should be conssitent with types defined
    // in OutFileClause.java of FE.
    MysqlRowBuffer row_buffer;
    row_buffer.push_int(_file_idx);                         // file number
    row_buffer.push_bigint(_written_rows_counter->value()); // total rows
    row_buffer.push_bigint(_written_data_bytes->value());   // file size
    std::string file_url;
    _get_file_url(&file_url);
    row_buffer.push_string(file_url.c_str(), file_url.length()); // url

    std::unique_ptr<TFetchDataResult> result = std::make_unique<TFetchDataResult>();
    result->result_batch.rows.resize(1);
    result->result_batch.rows[0].assign(row_buffer.buf(), row_buffer.length());
    RETURN_NOT_OK_STATUS_WITH_WARN(_sinker->add_batch(result), "failed to send outfile result");
    return Status::OK();
}

Status FileResultWriter::_fill_result_batch() {
    if (_is_result_sent) {
        return Status::OK();
    }
    _is_result_sent = true;

    TupleDescriptor* tuple_desc = _output_batch->row_desc().tuple_descriptors()[0];
    Tuple* tuple = (Tuple*)_output_batch->tuple_data_pool()->allocate(tuple_desc->byte_size());
    _output_batch->get_row(_output_batch->add_row())->set_tuple(0, tuple);
    memset(tuple, 0, tuple_desc->byte_size());

    MemPool* tuple_pool = _output_batch->tuple_data_pool();
    RawValue::write(&_file_idx, tuple, tuple_desc->slots()[0], tuple_pool);
    int64_t written_rows = _written_rows_counter->value();
    RawValue::write(&written_rows, tuple, tuple_desc->slots()[1], tuple_pool);
    int64_t written_data_bytes = _written_data_bytes->value();
    RawValue::write(&written_data_bytes, tuple, tuple_desc->slots()[2], tuple_pool);

    StringValue* url_str_val =
        reinterpret_cast<StringValue*>(tuple->get_slot(tuple_desc->slots()[3]->tuple_offset()));
    std::string file_url;
    _get_file_url(&file_url);
    url_str_val->ptr = (char*)_output_batch->tuple_data_pool()->allocate(file_url.length());
    url_str_val->len = file_url.length();
    memcpy(url_str_val->ptr, file_url.c_str(), url_str_val->len);
    
    _output_batch->commit_last_row();
    return Status::OK();
}

Status FileResultWriter::close() {
    // the following 2 profile "_written_rows_counter" and "_writer_close_timer"
    // must be outside the `_close_file_writer()`.
    // because `_close_file_writer()` may be called in deconstructor,
    // at that time, the RuntimeState may already been deconstructed,
    // so does the profile in RuntimeState.
    COUNTER_SET(_written_rows_counter, _written_rows);
    SCOPED_TIMER(_writer_close_timer);
    return _close_file_writer(true, false);
}

} // namespace doris
