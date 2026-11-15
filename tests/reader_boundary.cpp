#include "reader_boundary.hpp"

#include <maxminddb.h>
#include <cstring>

namespace {
using mmdb::test::ReaderFault;
thread_local const char* injected_token = nullptr;
thread_local ReaderFault injected_fault = ReaderFault::None;
}

namespace mmdb::test {

ScopedReaderFault::ScopedReaderFault(const char* token, ReaderFault fault) noexcept
	: previous_token_(injected_token), previous_fault_(injected_fault) {
	injected_token = token;
	injected_fault = fault;
}

ScopedReaderFault::~ScopedReaderFault() {
	injected_token = previous_token_;
	injected_fault = previous_fault_;
}

} // namespace mmdb::test

extern "C" int omp_mmdb_test_aget_value(
		MMDB_entry_s* entry, MMDB_entry_data_s* data, const char* const* path) {
	const int status = MMDB_aget_value(entry, data, path);
	if (status != MMDB_SUCCESS || injected_fault == ReaderFault::None) return status;
	const char* token = path[0];
	const bool matches = token && injected_token ? std::strcmp(token, injected_token) == 0 : token == injected_token;
	if (!matches) return status;

	switch (injected_fault) {
		case ReaderFault::NoData: *data = {}; break;
		// Leave populated output behind when the reader reports an error.
		case ReaderFault::InvalidData: return MMDB_INVALID_DATA_ERROR;
		case ReaderFault::UnsupportedType: data->type = MMDB_DATA_TYPE_END_MARKER; break;
		case ReaderFault::None: break;
	}
	return status;
}
