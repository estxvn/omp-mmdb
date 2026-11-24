#include "reader_cleanup_faults.h"
#include "store.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void check(bool condition, const char* expression, int line) {
	if (!condition) throw std::runtime_error("Line " + std::to_string(line) + ": " + expression);
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::string decoder_fixture() {
	const auto path = (std::filesystem::path(MMDB_FIXTURE_DIR) / "MaxMind-DB-test-decoder.mmdb").u8string();
	return {reinterpret_cast<const char*>(path.data()), path.size()};
}

void check_closed(const MMDB_s& database) {
	CHECK(reader_cleanup_counters()->live_allocations == 0);
	CHECK(database.filename == nullptr);
	CHECK(database.file_content == nullptr);
	CHECK(database.data_section == nullptr);
	CHECK(database.metadata.database_type == nullptr);
	CHECK(database.metadata.languages.names == nullptr);
	CHECK(database.metadata.description.descriptions == nullptr);
	CHECK(database.metadata.description.count == 0);
}

#ifdef _WIN32
void check_windows_cleanup(int startups, int views_unmapped) {
	const auto& counts = *reader_cleanup_counters();
	CHECK(counts.files_opened == 1 && counts.files_closed == 1);
	CHECK(counts.mappings_opened == 1 && counts.mappings_closed == 1);
	CHECK(counts.views_mapped == 1 && counts.views_unmapped == views_unmapped);
	CHECK(counts.startup_calls == 1 && counts.successful_startups == startups);
	CHECK(counts.cleanup_calls == (views_unmapped ? startups : 0));
}
#endif

void test_description_failures(const std::string& path) {
	for (const auto fault : {READER_CLEANUP_DESCRIPTION_ENTRY, READER_CLEANUP_DESCRIPTION_TEXT}) {
		reader_cleanup_reset(fault);
		MMDB_s database{};
		CHECK(MMDB_open(path.c_str(), MMDB_MODE_MMAP, &database) == MMDB_OUT_OF_MEMORY_ERROR);
		CHECK(reader_cleanup_counters()->injected_failures == 1);
		check_closed(database);
#ifdef _WIN32
		check_windows_cleanup(1, 1);
#endif
	}
}

void test_open_close(const std::string& path) {
	reader_cleanup_reset(READER_CLEANUP_NO_FAULT);
	MMDB_s database{};
	CHECK(MMDB_open(path.c_str(), MMDB_MODE_MMAP, &database) == MMDB_SUCCESS);
	CHECK(database.file_content != nullptr && database.file_size > 0);
	CHECK(database.metadata.description.count == 1);
	CHECK(database.metadata.description.descriptions != nullptr);
	CHECK(database.metadata.description.descriptions[0] != nullptr);
	CHECK(database.metadata.description.descriptions[0]->language != nullptr);
	CHECK(database.metadata.description.descriptions[0]->description != nullptr);
	const auto& description = *database.metadata.description.descriptions[0];
	CHECK(std::string_view(description.language) == "en");
	CHECK(std::string_view(description.description) == "MaxMind DB Decoder Test database - contains every MaxMind DB data type");
	CHECK(reader_cleanup_counters()->live_allocations > 0);
#ifdef _WIN32
	check_windows_cleanup(1, 0);
#endif
	MMDB_close(&database);
	check_closed(database);
	MMDB_close(&database);
	check_closed(database);
#ifdef _WIN32
	check_windows_cleanup(1, 1);
#endif
}

void test_early_failure(const std::string& path) {
	reader_cleanup_reset(READER_CLEANUP_NO_FAULT);
	MMDB_s database{};
	MMDB_description_s* unowned = nullptr;
	database.metadata.description.descriptions = &unowned;
	CHECK(MMDB_open((path + ".missing").c_str(), MMDB_MODE_MMAP, &database) == MMDB_FILE_OPEN_ERROR);
	check_closed(database);
#ifdef _WIN32
	CHECK(reader_cleanup_counters()->startup_calls == 0);
	CHECK(reader_cleanup_counters()->cleanup_calls == 0);
#endif
}

void test_failed_load_and_retry(const std::string& path) {
	mmdb::Service service;
	int owner = 0;
	CHECK(service.attach(&owner) == mmdb::Status::Ok);
	reader_cleanup_reset(READER_CLEANUP_DESCRIPTION_ENTRY);
	CHECK(service.load(&owner, "test", path) == mmdb::Status::ResourceLimit);
	CHECK(reader_cleanup_counters()->injected_failures == 1);
	CHECK(reader_cleanup_counters()->live_allocations == 0);
	CHECK(service.size() == 0);
	std::shared_ptr<const mmdb::Database> database;
	CHECK(service.database(&owner, "test", database) == mmdb::Status::NotLoaded);
	CHECK(!database);

	reader_cleanup_reset(READER_CLEANUP_NO_FAULT);
	CHECK(service.load(&owner, "test", path) == mmdb::Status::Ok);
	CHECK(service.database(&owner, "test", database) == mmdb::Status::Ok && database);
	CHECK(service.size() == 1);
	database.reset();
	CHECK(service.unload(&owner, "test") == mmdb::Status::Ok);
	CHECK(reader_cleanup_counters()->live_allocations == 0);
}

#ifdef _WIN32
void test_failed_winsock_startup(const std::string& path) {
	reader_cleanup_reset(READER_CLEANUP_WINSOCK_STARTUP);
	MMDB_s database{};
	const int status = MMDB_open(path.c_str(), MMDB_MODE_MMAP, &database);
	if (status == MMDB_SUCCESS) MMDB_close(&database);
	CHECK(reader_cleanup_counters()->injected_failures == 1);
	check_windows_cleanup(0, 1);
	CHECK(status == MMDB_IO_ERROR);
	check_closed(database);
	CHECK(database.file_size == 0);
}
#endif

} // namespace

int main(int argc, char* argv[]) {
	try {
		const auto path = decoder_fixture();
#ifdef _WIN32
		if (argc == 2 && std::string_view(argv[1]) == "winsock") {
			test_failed_winsock_startup(path);
			return 0;
		}
#else
		(void)argv;
#endif
		CHECK(argc == 1);
		test_description_failures(path);
		test_open_close(path);
		test_early_failure(path);
		test_failed_load_and_retry(path);
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
