#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "reader_cleanup_faults.h"

#include <maxminddb.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static enum ReaderCleanupFault failure;
static struct ReaderCleanupCounters counters;
static int description_allocations_until_failure;

#ifdef _WIN32
static HANDLE open_file;
static HANDLE open_mapping;
static const void* mapped_view;
#endif

void reader_cleanup_reset(enum ReaderCleanupFault fault) {
	failure = fault;
	counters = (struct ReaderCleanupCounters){0};
	description_allocations_until_failure = 0;
#ifdef _WIN32
	open_file = NULL;
	open_mapping = NULL;
	mapped_view = NULL;
#endif
}

const struct ReaderCleanupCounters* reader_cleanup_counters(void) {
	return &counters;
}

static void* cleanup_test_calloc(size_t count, size_t size) {
	// The pinned reader uses this allocation shape only for description entries.
	if (count == 1 && size == sizeof(MMDB_description_s)) {
		if (failure == READER_CLEANUP_DESCRIPTION_ENTRY) {
			++counters.injected_failures;
			return NULL;
		}
		if (failure == READER_CLEANUP_DESCRIPTION_TEXT) {
			// The first entry copies its language and then its description text.
			description_allocations_until_failure = 2;
		}
	}
	void* allocation = calloc(count, size);
	if (allocation) ++counters.live_allocations;
	return allocation;
}

static void* cleanup_test_malloc(size_t size) {
	if (description_allocations_until_failure && --description_allocations_until_failure == 0) {
		++counters.injected_failures;
		return NULL;
	}
	void* allocation = malloc(size);
	if (allocation) ++counters.live_allocations;
	return allocation;
}

static void cleanup_test_free(void* allocation) {
	if (allocation) --counters.live_allocations;
	free(allocation);
}

#ifdef _WIN32
static HANDLE cleanup_test_create_file(
		LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
		DWORD disposition, DWORD flags, HANDLE template_file) {
	HANDLE result = CreateFileW(name, access, share, security, disposition, flags, template_file);
	if (result != INVALID_HANDLE_VALUE) {
		open_file = result;
		++counters.files_opened;
	}
	return result;
}

static HANDLE cleanup_test_create_mapping(
		HANDLE file, LPSECURITY_ATTRIBUTES security, DWORD protect, DWORD high, DWORD low, LPCTSTR name) {
	HANDLE result = CreateFileMapping(file, security, protect, high, low, name);
	if (result) {
		open_mapping = result;
		++counters.mappings_opened;
	}
	return result;
}

static BOOL cleanup_test_close_handle(HANDLE handle) {
	const BOOL result = CloseHandle(handle);
	if (result && handle == open_file) {
		open_file = NULL;
		++counters.files_closed;
	}
	if (result && handle == open_mapping) {
		open_mapping = NULL;
		++counters.mappings_closed;
	}
	return result;
}

static LPVOID cleanup_test_map_view(HANDLE mapping, DWORD access, DWORD high, DWORD low, SIZE_T size) {
	LPVOID result = MapViewOfFile(mapping, access, high, low, size);
	if (result) {
		mapped_view = result;
		++counters.views_mapped;
	}
	return result;
}

static BOOL cleanup_test_unmap_view(LPCVOID view) {
	const BOOL result = UnmapViewOfFile(view);
	if (result && view == mapped_view) {
		mapped_view = NULL;
		++counters.views_unmapped;
	}
	return result;
}

static int cleanup_test_wsa_startup(WORD version, LPWSADATA data) {
	++counters.startup_calls;
	if (failure == READER_CLEANUP_WINSOCK_STARTUP) {
		++counters.injected_failures;
		return WSASYSNOTREADY;
	}
	const int result = WSAStartup(version, data);
	if (!result) ++counters.successful_startups;
	return result;
}

static int cleanup_test_wsa_cleanup(void) {
	++counters.cleanup_calls;
	if (failure == READER_CLEANUP_WINSOCK_STARTUP) return WSANOTINITIALISED;
	return WSACleanup();
}

#define CreateFileW cleanup_test_create_file
#undef CreateFileMapping
#define CreateFileMapping cleanup_test_create_mapping
#define CloseHandle cleanup_test_close_handle
#define MapViewOfFile cleanup_test_map_view
#define UnmapViewOfFile cleanup_test_unmap_view
#define WSAStartup cleanup_test_wsa_startup
#define WSACleanup cleanup_test_wsa_cleanup
#endif

#define calloc cleanup_test_calloc
#define malloc cleanup_test_malloc
#define free cleanup_test_free
#include "generated/maxminddb.c"
