# These substitutions apply to the pinned libmaxminddb 1.14.1 source.
function(omp_mmdb_replace_reader_text source_var before after)
	string(FIND "${${source_var}}" "${before}" first)
	string(FIND "${${source_var}}" "${before}" last REVERSE)
	if(first EQUAL -1 OR NOT first EQUAL last)
		message(FATAL_ERROR "The pinned libmaxminddb source changed. Review the generated reader substitution for: ${before}")
	endif()
	string(REPLACE "${before}" "${after}" source "${${source_var}}")
	set("${source_var}" "${source}" PARENT_SCOPE)
endfunction()

function(omp_mmdb_use_bitwise_decoder)
	get_target_property(reader_root maxminddb SOURCE_DIR)
	set(input "${reader_root}/src/maxminddb.c")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
	file(READ "${input}" decoder)
	string(REPLACE "\r\n" "\n" decoder "${decoder}")

	# An out-of-line float or double return can use x87 registers on x86.
	# Copy the swapped integer directly into the entry's floating-point storage.
	foreach(type IN ITEMS float double)
		string(SUBSTRING "${type}" 0 1 local)
		set(helper "get_ieee754_${type}")
		set(before "static ${type} ${helper}(const uint8_t *restrict p)")
		set(after "static void ${helper}(const uint8_t *restrict p, ${type} *out)")
		omp_mmdb_replace_reader_text(decoder "${before};" "${after};")
		omp_mmdb_replace_reader_text(decoder "${before} {\n    ${type} ${local};" "${after} {")
		omp_mmdb_replace_reader_text(decoder
			"memcpy(&${local}, &i, sizeof(${type}));\n\n    return ${local};"
			"memcpy(out, &i, sizeof(i));")
		omp_mmdb_replace_reader_text(decoder
			"entry_data->${type}_value = ${helper}(&mem[offset]);\n        DEBUG_MSGF(\"${type} value: %f\", entry_data->${type}_value);"
			"${helper}(&mem[offset], &entry_data->${type}_value);")
	endforeach()

	# A description array can be allocated before any of its entries are initialized.
	omp_mmdb_replace_reader_text(decoder
		"mmdb->metadata.languages.names = NULL;\n    mmdb->metadata.description.count = 0;"
		"mmdb->metadata.languages.names = NULL;\n    mmdb->metadata.description.count = 0;\n    mmdb->metadata.description.descriptions = NULL;")
	omp_mmdb_replace_reader_text(decoder
		"if (!mmdb->metadata.description.count) {"
		"if (!mmdb->metadata.description.descriptions) {")

	# Publish the Windows view only after Winsock is initialized. Until then
	# map_file owns the view and its cleanup path still owns both handles.
	omp_mmdb_replace_reader_text(decoder [=[
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

]=] "")
	omp_mmdb_replace_reader_text(decoder [=[
        (uint8_t *)MapViewOfFile(mmh, FILE_MAP_READ, 0, 0, 0);
    if (file_content == NULL) {
        status = MMDB_IO_ERROR;
        goto cleanup;
    }
]=] [=[
        (uint8_t *)MapViewOfFile(mmh, FILE_MAP_READ, 0, 0, 0);
    if (file_content == NULL) {
        status = MMDB_IO_ERROR;
        goto cleanup;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        UnmapViewOfFile(file_content);
        status = MMDB_IO_ERROR;
        goto cleanup;
    }
]=])
	omp_mmdb_replace_reader_text(decoder [=[
        /* Winsock is only initialized if open was successful so we only have
         * to cleanup then. */
]=] [=[
        /* A non-NULL view is published only after WSAStartup succeeds. */
]=])

	set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/maxminddb.c")
	file(CONFIGURE OUTPUT "${output}" CONTENT "@decoder@" @ONLY NEWLINE_STYLE LF)
	get_target_property(sources maxminddb SOURCES)
	if(NOT "src/maxminddb.c" IN_LIST sources)
		message(FATAL_ERROR "The pinned libmaxminddb target no longer uses src/maxminddb.c")
	endif()
	list(REMOVE_ITEM sources "src/maxminddb.c")
	list(APPEND sources "${output}")
	set_property(TARGET maxminddb PROPERTY SOURCES "${sources}")
	target_include_directories(maxminddb PRIVATE "${reader_root}/src")
endfunction()
