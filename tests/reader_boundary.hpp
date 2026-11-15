#pragma once

namespace mmdb::test {

enum class ReaderFault { None, NoData, InvalidData, UnsupportedType };

class ScopedReaderFault final {
public:
	// A null token selects the root read.
	ScopedReaderFault(const char* token, ReaderFault fault) noexcept;
	~ScopedReaderFault();
	ScopedReaderFault(const ScopedReaderFault&) = delete;
	ScopedReaderFault& operator=(const ScopedReaderFault&) = delete;

private:
	const char* previous_token_;
	ReaderFault previous_fault_;
};

} // namespace mmdb::test
