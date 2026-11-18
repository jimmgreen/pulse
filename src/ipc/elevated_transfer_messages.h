#pragma once
#include "elevated_transfer_protocol.h"
#include "../ops/ops_manager.h"

namespace pulse::elevated {
inline void WriteStatus(Writer& writer, const ops::OpStatus& status) {
    writer.Number(static_cast<uint32_t>(status.phase));
    writer.Number(status.percent);
    writer.Number(status.total_bytes); writer.Number(status.transferred_bytes);
    writer.Number(status.total_items); writer.Number(status.completed_items);
    writer.Number(status.bytes_per_second); writer.Number(status.peak_bytes_per_second);
    writer.Number(status.eta_seconds);
    writer.Number<uint32_t>(status.can_pause ? 1 : 0);
    writer.Text(status.current_item); writer.Text(status.summary); writer.Text(status.last_error);
    writer.Number<uint32_t>(status.enumeration_complete ? 1 : 0);
}
inline ops::OpStatus ReadStatus(Reader& reader) {
    ops::OpStatus status;
    const auto phase = reader.Number<uint32_t>();
    if (phase > static_cast<uint32_t>(ops::OpPhase::Failed)) reader.good = false;
    status.phase = static_cast<ops::OpPhase>(phase);
    status.percent = reader.Number<float>();
    status.total_bytes = reader.Number<uint64_t>(); status.transferred_bytes = reader.Number<uint64_t>();
    status.total_items = reader.Number<uint64_t>(); status.completed_items = reader.Number<uint64_t>();
    status.bytes_per_second = reader.Number<double>(); status.peak_bytes_per_second = reader.Number<double>();
    status.eta_seconds = reader.Number<uint64_t>();
    status.can_pause = reader.Number<uint32_t>() != 0;
    status.current_item = reader.Text(); status.summary = reader.Text(); status.last_error = reader.Text();
    const auto enumeration_complete = reader.Number<uint32_t>();
    if (enumeration_complete > 1) reader.good = false;
    status.enumeration_complete = enumeration_complete == 1;
    return status;
}
inline void WriteConflict(Writer& writer, const ops::ConflictItemInfo& item) {
    writer.Number(item.token); writer.Text(item.source); writer.Text(item.destination);
    writer.Number(item.source_size); writer.Number(item.destination_size);
    writer.Number(item.source_modified); writer.Number(item.destination_modified);
    writer.Number<uint32_t>(item.source_is_directory ? 1 : 0);
    writer.Number<uint32_t>(item.destination_is_directory ? 1 : 0);
    writer.Number<uint64_t>(item.remaining);
}
inline ops::ConflictItemInfo ReadConflict(Reader& reader) {
    ops::ConflictItemInfo item;
    item.token = reader.Number<uint64_t>(); item.source = reader.Text(); item.destination = reader.Text();
    item.source_size = reader.Number<uint64_t>(); item.destination_size = reader.Number<uint64_t>();
    item.source_modified = reader.Number<FILETIME>(); item.destination_modified = reader.Number<FILETIME>();
    item.source_is_directory = reader.Number<uint32_t>() != 0;
    item.destination_is_directory = reader.Number<uint32_t>() != 0;
    item.remaining = static_cast<size_t>(reader.Number<uint64_t>());
    return item;
}
}
