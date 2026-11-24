#ifndef OMP_MMDB_READER_CLEANUP_FAULTS_H
#define OMP_MMDB_READER_CLEANUP_FAULTS_H

#ifdef __cplusplus
extern "C" {
#endif

enum ReaderCleanupFault {
	READER_CLEANUP_NO_FAULT,
	READER_CLEANUP_DESCRIPTION_ENTRY,
	READER_CLEANUP_DESCRIPTION_TEXT,
	READER_CLEANUP_WINSOCK_STARTUP,
};

struct ReaderCleanupCounters {
	int injected_failures;
	int live_allocations;
#ifdef _WIN32
	int files_opened;
	int files_closed;
	int mappings_opened;
	int mappings_closed;
	int views_mapped;
	int views_unmapped;
	int startup_calls;
	int successful_startups;
	int cleanup_calls;
#endif
};

void reader_cleanup_reset(enum ReaderCleanupFault fault);
const struct ReaderCleanupCounters* reader_cleanup_counters(void);

#ifdef __cplusplus
}
#endif

#endif
