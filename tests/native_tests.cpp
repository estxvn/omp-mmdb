#include "pawn_bridge.hpp"
#include "floating_point_environment.hpp"
#include "reader_boundary.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace mmdb;

std::size_t checks = 0;
void check(bool value, int line) {
	++checks;
	if (!value) throw std::runtime_error("Native test failed at line " + std::to_string(line));
}
#define CHECK(value) check((value), __LINE__)

constexpr cell name = 0, file = 512, ip = 2048, path = 3072, out = 4108, text = 5000;

const AMX_NATIVE_INFO& native(const char* function) {
	for (int i = 0; i < native_count(); ++i)
		if (std::strcmp(native_table()[i].name, function) == 0) return native_table()[i];
	throw std::runtime_error(std::string("Missing native ") + function);
}

struct Vm {
	std::array<cell, 8192> data{};
	AMX amx{};
	Vm() {
		amx.data = reinterpret_cast<unsigned char*>(data.data());
		amx.hea = 16384;
		amx.stk = 24576;
		amx.stp = 32768;
	}
	cell& at(cell address) { return data[static_cast<std::size_t>(address) / sizeof(cell)]; }
	void string(cell address, std::string_view value) {
		for (std::size_t i = 0; i < value.size(); ++i)
			at(address + static_cast<cell>(i * sizeof(cell))) = static_cast<unsigned char>(value[i]);
		at(address + static_cast<cell>(value.size() * sizeof(cell))) = 0;
	}
	Status call(const char* function, std::initializer_list<cell> args) {
		auto* params = &at(amx.stk);
		params[0] = static_cast<cell>(args.size() * sizeof(cell));
		std::copy(args.begin(), args.end(), params + 1);
		return static_cast<Status>(native(function).func(&amx, params));
	}
};

struct InlineVm {
	static constexpr cell cell_size = static_cast<cell>(sizeof(cell));
	static constexpr cell data_offset = static_cast<cell>(sizeof(AMX_HEADER));
	static constexpr std::size_t cell_count = 16;
	static constexpr cell ceiling = static_cast<cell>(cell_count - 1) * cell_size;
	static_assert(data_offset % alignof(cell) == 0);

	alignas(AMX_HEADER) alignas(cell) unsigned char storage[data_offset + cell_count * sizeof(cell)]{};
	AMX_HEADER* header = ::new (static_cast<void*>(storage)) AMX_HEADER{};
	cell* data = ::new (static_cast<void*>(storage + data_offset)) cell[cell_count]{};
	AMX amx{};

	InlineVm() {
		header->magic = AMX_MAGIC;
		header->dat = data_offset;
		header->hea = data_offset + 4 * cell_size;
		header->size = header->hea;
		header->stp = data_offset + ceiling + cell_size;
		amx.base = storage;
		amx.hlw = amx.hea = header->hea - data_offset;
		amx.stk = ceiling - 3 * cell_size;
		amx.stp = ceiling;
	}

	cell& at(cell offset) { return data[static_cast<std::size_t>(offset) / sizeof(cell)]; }
};

void check_inline_stack_ceiling(PawnBridge& bridge) {
	InlineVm vm;
	CHECK(bridge.attach(&vm.amx) == Status::Ok);
	const auto is_loaded = native("mmdb_is_loaded").func;
	constexpr cell width = InlineVm::cell_size;
	constexpr cell output = 3 * width;
	const auto stack = vm.amx.stk;
	const std::array<cell, 3> args{2 * width, 0, output};
	auto* params = &vm.at(stack);
	std::copy(args.begin(), args.end(), params);
	vm.at(0) = 'd';
	vm.at(width) = 'b';
	const auto call = [&](const cell* block) {
		return static_cast<Status>(is_loaded(&vm.amx, block));
	};
	const auto rejects_without_output = [&](const cell* block) {
		vm.at(output) = -1;
		return call(block) == Status::InvalidArgument && vm.at(output) == -1;
	};

	vm.at(output) = -1;
	CHECK(call(params) == Status::Ok && vm.at(output) == 0);
	CHECK(rejects_without_output(params + 1));
	// Only the register changes. The parameters and output stay in live cells.
	vm.amx.stp += width;
	CHECK(rejects_without_output(params));
	vm.amx.stp = std::numeric_limits<cell>::max();
	CHECK(rejects_without_output(params));
	vm.amx.stp = std::numeric_limits<cell>::min();
	CHECK(rejects_without_output(params));
	vm.amx.stp = InlineVm::ceiling;

	const auto image_top = vm.header->stp;
	vm.header->stp = vm.header->dat;
	CHECK(rejects_without_output(params));
	vm.header->stp = std::numeric_limits<cell>::min();
	CHECK(rejects_without_output(params));
	vm.header->stp = image_top;
	vm.header->dat = std::numeric_limits<cell>::min();
	CHECK(rejects_without_output(params));
	vm.header->dat = InlineVm::data_offset;

	vm.amx.hea += 2 * width;
	vm.amx.stk -= 2 * width;
	params = &vm.at(vm.amx.stk);
	std::copy(args.begin(), args.end(), params);
	const cell grown_heap_output = 5 * width;
	params[2] = grown_heap_output;
	vm.at(grown_heap_output) = -1;
	CHECK(call(params) == Status::Ok && vm.at(grown_heap_output) == 0);
	vm.amx.hea -= 2 * width;
	vm.at(grown_heap_output) = -1;
	CHECK(call(params) == Status::InvalidArgument && vm.at(grown_heap_output) == -1);

	vm.amx.stp -= width;
	const auto stack_output = vm.amx.stp - width;
	params[2] = stack_output;
	vm.at(stack_output) = -1;
	CHECK(call(params) == Status::Ok && vm.at(stack_output) == 0);
	params[2] = output + 1;
	CHECK(rejects_without_output(params));
	params[2] = output;
	vm.amx.stp = vm.amx.stk + 2 * width;
	CHECK(rejects_without_output(params));

	// Separate data does not use the header's inline bounds.
	std::array<cell, InlineVm::cell_count> separate{};
	std::copy_n(vm.data, separate.size(), separate.begin());
	vm.amx.data = reinterpret_cast<unsigned char*>(separate.data());
	vm.amx.stp = InlineVm::ceiling;
	vm.header->stp = 0;
	const auto* separate_params = separate.data() + vm.amx.stk / width;
	CHECK(call(separate_params) == Status::Ok && separate[3] == 0 && vm.at(output) == -1);
	bridge.detach(&vm.amx);
}

void check_ip_natives(PawnBridge& bridge) {
	Vm vm;
	CHECK(bridge.attach(&vm.amx) == Status::Ok);
	const auto mixed = (std::filesystem::path(MMDB_FIXTURE_DIR) / "MaxMind-DB-test-mixed-24.mmdb").string();
	vm.string(name, "mixed");
	vm.string(file, mixed);
	CHECK(vm.call("mmdb_load", {name, file}) == Status::Ok);

	constexpr cell sentinel = -12345;
	for (const char* address : {"::ffff:192.010.2.1", "::ffff:1.01.1.3", "::ffff:1.1.1.03", "01.1.1.3",
			"::ffff:1.256.1.3", "::ffff:1.1.3", "::ffff:1.1.1.3.5"}) {
		vm.string(ip, address);
		for (const char* function : {"mmdb_has_record", "mmdb_get_prefix"}) {
			vm.at(out) = sentinel;
			CHECK(vm.call(function, {name, ip, out}) == Status::InvalidIp);
			CHECK(vm.at(out) == sentinel);
		}
	}
	const auto check_hit = [&](const char* address, cell prefix) {
		vm.string(ip, address);
		vm.at(out) = sentinel;
		CHECK(vm.call("mmdb_has_record", {name, ip, out}) == Status::Ok && vm.at(out) == 1);
		CHECK(vm.call("mmdb_get_prefix", {name, ip, out}) == Status::Ok && vm.at(out) == prefix);
	};
	check_hit("1.1.1.3", 31);
	check_hit("1.1.1.4", 30);
	check_hit("::ffff:1.1.1.3", 127);
	check_hit("0:0:0:0:0:FFFF:0101:0103", 127);
	vm.string(ip, "::ffff:192.10.2.1");
	CHECK(vm.call("mmdb_has_record", {name, ip, out}) == Status::Ok && vm.at(out) == 0);
	vm.at(out) = sentinel;
	CHECK(vm.call("mmdb_get_prefix", {name, ip, out}) == Status::NoRecord && vm.at(out) == sentinel);
	bridge.detach(&vm.amx);
}

void check_reader_errors(PawnBridge& bridge) {
	using test::ReaderFault;
	Vm vm;
	CHECK(bridge.attach(&vm.amx) == Status::Ok);
	const auto fixture = (std::filesystem::path(MMDB_FIXTURE_DIR) / "MaxMind-DB-test-nested.mmdb").u8string();
	vm.string(name, "reader");
	vm.string(file, {reinterpret_cast<const char*>(fixture.data()), fixture.size()});
	vm.string(ip, "1.1.1.1");
	CHECK(vm.call("mmdb_load", {name, file}) == Status::Ok);

	constexpr cell sentinel = -12345;
	constexpr auto cell_size = static_cast<cell>(sizeof(cell));
	constexpr char leaf[] = "/map1/map2/array/0/map3/c";
	struct Case { const char* token; ReaderFault fault; const char* path; };
	for (const auto& entry : {
			Case{"map2", ReaderFault::UnsupportedType, leaf},
			Case{nullptr, ReaderFault::UnsupportedType, leaf},
			Case{nullptr, ReaderFault::UnsupportedType, ""},
			Case{"c", ReaderFault::UnsupportedType, leaf},
			Case{nullptr, ReaderFault::NoData, leaf},
			Case{"c", ReaderFault::InvalidData, leaf}}) {
		{
			const test::ScopedReaderFault fault(entry.token, entry.fault);
			vm.string(path, entry.path);
			for (const auto* function : {"mmdb_has_field", "mmdb_get_int", "mmdb_get_float",
					"mmdb_get_bool", "mmdb_get_type", "mmdb_get_size"}) {
				vm.at(out) = sentinel;
				CHECK(vm.call(function, {name, ip, path, out}) == Status::DecoderError);
				CHECK(vm.at(out) == sentinel);
			}
			for (const auto* function : {"mmdb_get_string", "mmdb_get_text"}) {
				vm.at(out) = sentinel;
				vm.at(text) = sentinel;
				vm.at(text + cell_size) = sentinel;
				CHECK(vm.call(function, {name, ip, path, text, 2, out}) == Status::DecoderError);
				CHECK(vm.at(out) == sentinel && vm.at(text) == sentinel && vm.at(text + cell_size) == sentinel);
			}
		}

		vm.string(path, leaf);
		CHECK(vm.call("mmdb_has_field", {name, ip, path, out}) == Status::Ok && vm.at(out) == 1);
		CHECK(vm.call("mmdb_get_int", {name, ip, path, out}) == Status::Ok && vm.at(out) == 3);
	}

	for (const char* missing : {"/map1/map2/array/0/map3/c/child", "/map1/map2/array/0/map3/absent"}) {
		vm.string(path, missing);
		vm.at(out) = sentinel;
		CHECK(vm.call("mmdb_get_int", {name, ip, path, out}) == Status::NoField && vm.at(out) == sentinel);
		CHECK(vm.call("mmdb_has_field", {name, ip, path, out}) == Status::Ok && vm.at(out) == 0);
	}
	bridge.detach(&vm.amx);
}

void check_floating_point_environment() {
	constexpr std::uint32_t sentinel = 0x4b8ace11;
	constexpr std::array binary32{
		0x00000000U, 0x00000001U, 0x00000002U, 0x007ffffeU, 0x007fffffU, 0x00800000U,
		0x00800001U, 0x3f800001U, 0x7f7ffffeU, 0x7f7fffffU, 0x7f800000U, 0x7f800001U, 0x7fc00000U
	};
	struct NarrowingCase { std::uint64_t input; std::uint32_t expected; };
	constexpr NarrowingCase binary64[]{
		{0x0000000000000000, 0x00000000}, {0x0000000000000001, sentinel},
		// The contract rejects values outside the range before rounding.
		{0x369fffffffffffff, sentinel}, {0x36a0000000000000, 0x00000001}, {0x36a0000000000001, 0x00000001},
		// Midpoint between the first two subnormals and the doubles on either side.
		{0x36a7ffffffffffff, 0x00000001}, {0x36a8000000000000, 0x00000002}, {0x36a8000000000001, 0x00000002},
		{0x380fffffc0000000, 0x007fffff}, // Largest subnormal.
		{0x380fffffdfffffff, 0x007fffff}, {0x380fffffe0000000, 0x00800000}, {0x380fffffe0000001, 0x00800000},
		{0x380fffffffffffff, 0x00800000}, {0x3810000000000000, 0x00800000}, {0x3810000000000001, 0x00800000},
		// Normal midpoints that respectively round down and up to an even result.
		{0x3ff000000fffffff, 0x3f800000}, {0x3ff0000010000000, 0x3f800000}, {0x3ff0000010000001, 0x3f800001},
		{0x3ff000002fffffff, 0x3f800001}, {0x3ff0000030000000, 0x3f800002}, {0x3ff0000030000001, 0x3f800002},
		{0x3fffffffefffffff, 0x3fffffff}, {0x3ffffffff0000000, 0x40000000}, {0x3ffffffff0000001, 0x40000000},
		{0x47efffffdfffffff, 0x7f7fffff}, {0x47efffffe0000000, 0x7f7fffff}, {0x47efffffe0000001, sentinel},
		{0x7ff0000000000000, sentinel}, {0x7ff0000000000001, sentinel}, {0x7ff8000000000000, sentinel}
	};

	// The single 24-bit IPv4 node points both branches at the test map.
	std::string bytes("\0\0\x11\0\0\x11", 6);
	bytes.append(16, '\0');
	const auto byte = [&](unsigned number) { bytes.push_back(static_cast<char>(number)); };
	const auto field = [&](unsigned type, std::size_t size) {
		CHECK(size < 285);
		byte((type < 8 ? type << 5 : 0) | static_cast<unsigned>(std::min<std::size_t>(size, 29)));
		if (type >= 8) byte(type - 7);
		if (size >= 29) byte(static_cast<unsigned>(size - 29));
	};
	const auto string_field = [&](std::string_view value) {
		field(MMDB_DATA_TYPE_UTF8_STRING, value.size());
		bytes.append(value);
	};
	const auto big_endian = [&](std::uint64_t value, unsigned size) {
		for (auto shift = size * 8; shift != 0; shift -= 8) {
			byte(static_cast<unsigned>((value >> (shift - 8)) & 0xff));
		}
	};

	struct Query { std::string path; std::uint64_t input; std::uint32_t expected; Status status; };
	std::vector<Query> queries;
	const auto numeric_array = [&](std::string_view key, std::size_t size) {
		string_field(key);
		field(MMDB_DATA_TYPE_ARRAY, 2 * size);
	};
	const auto numeric_value = [&](unsigned type, unsigned size, std::uint64_t input,
			std::uint32_t expected, std::string_view key, std::size_t index) {
		for (const auto negative : {false, true}) {
			const auto bits = input | (std::uint64_t{negative} << (size * 8 - 1));
			field(type, size);
			big_endian(bits, size);
			const auto status = expected == sentinel ? Status::OutOfRange : Status::Ok;
			const auto result = status == Status::Ok && negative ? expected | 0x80000000U : expected;
			queries.push_back({"/" + std::string(key) + "/" + std::to_string(2 * index + negative), bits, result, status});
		}
	};
	field(MMDB_DATA_TYPE_MAP, 3);
	numeric_array("float", binary32.size());
	for (std::size_t i = 0; i < binary32.size(); ++i) {
		const auto word = binary32[i];
		numeric_value(MMDB_DATA_TYPE_FLOAT, 4, word, word < 0x7f800000U ? word : sentinel, "float", i);
	}
	numeric_array("double", std::size(binary64));
	for (std::size_t i = 0; i < std::size(binary64); ++i) {
		numeric_value(MMDB_DATA_TYPE_DOUBLE, 8, binary64[i].input, binary64[i].expected, "double", i);
	}
	string_field("other");
	field(MMDB_DATA_TYPE_BOOLEAN, 1);
	queries.push_back({"/other", 1, sentinel, Status::TypeMismatch});

	bytes.append("\xab\xcd\xefMaxMind.com");
	field(MMDB_DATA_TYPE_MAP, 9);
	const auto metadata_number = [&](std::string_view key, unsigned type, unsigned value) {
		string_field(key);
		field(type, value != 0 ? 1 : 0);
		if (value != 0) byte(value);
	};
	metadata_number("node_count", MMDB_DATA_TYPE_UINT32, 1);
	metadata_number("record_size", MMDB_DATA_TYPE_UINT16, 24);
	metadata_number("ip_version", MMDB_DATA_TYPE_UINT16, 4);
	string_field("database_type");
	string_field("FloatRegression");
	string_field("languages");
	field(MMDB_DATA_TYPE_ARRAY, 1);
	string_field("en");
	metadata_number("binary_format_major_version", MMDB_DATA_TYPE_UINT16, 2);
	metadata_number("binary_format_minor_version", MMDB_DATA_TYPE_UINT16, 0);
	metadata_number("build_epoch", MMDB_DATA_TYPE_UINT64, 1);
	string_field("description");
	field(MMDB_DATA_TYPE_MAP, 0);

	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto filename = std::filesystem::temp_directory_path() / ("omp-mmdb-float-" + std::to_string(stamp) + ".mmdb");
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	} cleanup{filename};
	{
		std::ofstream fixture(filename, std::ios::binary | std::ios::trunc);
		fixture.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		fixture.close();
		CHECK(fixture.good());
	}

	Service service;
	PawnBridge bridge(service);
	Vm vm;
	CHECK(bridge.attach(&vm.amx) == Status::Ok);
	vm.string(name, "floating");
	const auto utf8 = filename.u8string();
	vm.string(file, {reinterpret_cast<const char*>(utf8.data()), utf8.size()});
	vm.string(ip, "1.1.1.1");
	CHECK(vm.call("mmdb_load", {name, file}) == Status::Ok);
	std::size_t combinations = 0;
	const auto original_state = test::FloatingPointEnvironment{}.state();
	{
		test::FloatingPointEnvironment environment;
		combinations = environment.exercise([&](const auto before) {
			CHECK(environment.state() == before);
			for (const auto& query : queries) {
				vm.string(path, query.path);
				vm.at(out) = std::bit_cast<cell>(sentinel);
				const auto status = vm.call("mmdb_get_float", {name, ip, path, out});
				const auto after = environment.state();
				const auto result = std::bit_cast<std::uint32_t>(vm.at(out));
				++checks;
				if (status == query.status && result == query.expected && after == before) continue;
				std::ostringstream message;
				message << query.path << std::hex << " input 0x" << query.input
					<< " returned bits 0x" << result << " (expected 0x" << query.expected << ")"
					<< std::dec << " and status " << static_cast<int>(status) << " (expected " << static_cast<int>(query.status) << ")"
					<< std::hex << ". MXCSR 0x" << before.mxcsr << " -> 0x" << after.mxcsr
					<< ", x87 control word 0x" << before.x87_control << " -> 0x" << after.x87_control
					<< ", x87 exception flags 0x" << before.x87_exceptions << " -> 0x" << after.x87_exceptions;
				throw std::runtime_error(message.str());
			}
		});
	}
	CHECK(test::FloatingPointEnvironment{}.state() == original_state);
	std::cout << queries.size() << " floating-point queries passed in each of " << combinations << " environments\n";
	bridge.detach(&vm.amx);
}
} // namespace

int main() {
	try {
		check_floating_point_environment();
		Service service;
		PawnBridge bridge(service);
		Vm a, b;
		CHECK(bridge.attach(&a.amx) == Status::Ok);
		CHECK(bridge.attach(&b.amx) == Status::Ok);
		std::set<std::string> names;
		for (int i = 0; i < native_count(); ++i) {
			CHECK(names.insert(native_table()[i].name).second);
			CHECK(a.call(native_table()[i].name, {}) == Status::InvalidArgument);
		}
		CHECK(native_count() == 18);
		check_inline_stack_ceiling(bridge);
		check_ip_natives(bridge);
		check_reader_errors(bridge);

		const auto is_loaded = native("mmdb_is_loaded").func;
		constexpr auto cell_size = static_cast<cell>(sizeof(cell));
		constexpr cell heap_block = 64;
		const auto stack_start = a.amx.stk;
		const std::array<cell, 3> params{2 * cell_size, name, out};
		const auto call_is_loaded = [&](const cell* block) {
			return static_cast<Status>(is_loaded(&a.amx, block));
		};
		const auto call_at = [&](cell address) {
			auto* block = &a.at(address);
			std::copy(params.begin(), params.end(), block);
			return call_is_loaded(block);
		};
		a.string(name, "db");
		a.at(out) = -1;
		CHECK(call_at(heap_block) == Status::InvalidArgument && a.at(out) == -1);
		CHECK(call_at(stack_start + cell_size) == Status::InvalidArgument && a.at(out) == -1);
		CHECK(call_at(stack_start - cell_size) == Status::InvalidArgument && a.at(out) == -1);
		CHECK(call_is_loaded(params.data()) == Status::InvalidArgument);
		CHECK(call_is_loaded(nullptr) == Status::InvalidArgument);

		a.amx.stk = a.amx.stp - 3 * cell_size;
		CHECK(call_at(a.amx.stk) == Status::Ok && a.at(out) == 0);
		a.at(a.amx.stk) = std::numeric_limits<cell>::max();
		CHECK(call_is_loaded(&a.at(a.amx.stk)) == Status::InvalidArgument);
		a.at(a.amx.stk) = params[0];
		a.amx.stp -= cell_size;
		CHECK(call_is_loaded(&a.at(a.amx.stk)) == Status::InvalidArgument);
		a.amx.stp += cell_size;

		a.amx.stk = stack_start + 1;
		std::memcpy(a.amx.data + a.amx.stk, params.data(), params.size() * sizeof(cell));
		const auto* misaligned = reinterpret_cast<const cell*>(a.amx.data + a.amx.stk);
		CHECK(call_is_loaded(misaligned) == Status::InvalidArgument);
		a.amx.stk = heap_block;
		CHECK(call_at(heap_block) == Status::InvalidArgument);
		a.amx.stk = -cell_size;
		CHECK(call_at(heap_block) == Status::InvalidArgument);
		a.amx.stk = stack_start;

		const auto decoder = (std::filesystem::path(MMDB_FIXTURE_DIR) / "MaxMind-DB-test-decoder.mmdb").string();
		const auto ipv4 = (std::filesystem::path(MMDB_FIXTURE_DIR) / "MaxMind-DB-test-ipv4-24.mmdb").string();
		a.string(file, decoder);
		a.string(ip, "1.1.1.1");
		b.string(name, "db");
		b.string(file, ipv4);
		CHECK(a.call("mmdb_load", {name, file}) == Status::Ok);
		CHECK(a.call("mmdb_load", {name, file}) == Status::AlreadyLoaded);
		CHECK(a.call("mmdb_is_loaded", {name, out}) == Status::Ok && a.at(out) == 1);
		CHECK(b.call("mmdb_is_loaded", {name, out}) == Status::Ok && b.at(out) == 0);
		CHECK(b.call("mmdb_unload", {name}) == Status::NotLoaded);
		CHECK(b.call("mmdb_load", {name, file}) == Status::Ok);
		CHECK(b.call("mmdb_get_ip_version", {name, out}) == Status::Ok && b.at(out) == 4);
		CHECK(a.call("mmdb_get_ip_version", {name, out}) == Status::Ok && a.at(out) == 6);

		a.string(path, "/uint16");
		CHECK(a.call("mmdb_has_record", {name, ip, out}) == Status::Ok && a.at(out) == 1);
		CHECK(a.call("mmdb_get_int", {name, ip, path, out}) == Status::Ok && a.at(out) == 100);
		CHECK(a.call("mmdb_get_type", {name, ip, path, out}) == Status::Ok && a.at(out) == MMDB_DATA_TYPE_UINT16);
		CHECK(a.call("mmdb_get_text", {name, ip, path, text, 8, out}) == Status::Ok && a.at(text) == '1');
		a.string(path, "/utf8_string");
		a.at(text) = 777;
		CHECK(a.call("mmdb_get_string", {name, ip, path, text, 1, out}) == Status::BufferTooSmall);
		CHECK(a.at(text) == 777 && a.at(out) > 1);
		CHECK(a.call("mmdb_get_string", {name, ip, path, text, 100, out}) == Status::Ok);
		CHECK(a.call("mmdb_get_size", {name, ip, path, out}) == Status::Ok && a.at(out) > 0);
		CHECK(a.call("mmdb_get_string", {name, ip, path, text, 100, text}) == Status::InvalidArgument);
		CHECK(a.call("mmdb_get_string", {name, ip, path, 16380, 2, out}) == Status::InvalidArgument);
		CHECK(a.call("mmdb_get_string", {name, ip, path, 32764, 2, out}) == Status::InvalidArgument);
		CHECK(a.call("mmdb_get_string", {name, ip, path, text, -1, out}) == Status::InvalidArgument);
		CHECK(a.call("mmdb_get_int", {name, ip, path, 16384}) == Status::InvalidArgument);
		CHECK(a.call("mmdb_get_int", {name, ip, path, out + 1}) == Status::InvalidArgument);
		a.string(path, "/boolean");
		CHECK(a.call("mmdb_get_bool", {name, ip, path, out}) == Status::Ok && a.at(out) == 1);
		a.string(path, "/double");
		CHECK(a.call("mmdb_get_float", {name, ip, path, out}) == Status::Ok);
		a.string(path, "/absent");
		CHECK(a.call("mmdb_has_field", {name, ip, path, out}) == Status::Ok && a.at(out) == 0);
		a.string(path, "malformed");
		CHECK(a.call("mmdb_has_field", {name, ip, path, out}) == Status::InvalidPath);
		constexpr cell long_path_address = 8192;
		std::string long_path(1024, 'a');
		long_path.front() = '/';
		a.string(long_path_address, long_path);
		a.at(out) = -1;
		CHECK(a.call("mmdb_get_type", {name, ip, long_path_address, out}) == Status::NoField);
		CHECK(a.at(out) == -1);
		long_path.push_back('a');
		a.string(long_path_address, long_path);
		CHECK(a.call("mmdb_get_type", {name, ip, long_path_address, out}) == Status::InvalidArgument);
		CHECK(a.at(out) == -1);
		CHECK(a.call("mmdb_get_prefix", {name, ip, out}) == Status::Ok);
		CHECK(a.call("mmdb_get_database_type", {name, text, 100, out}) == Status::Ok);
		CHECK(a.call("mmdb_get_build_epoch", {name, text, 100, out}) == Status::Ok);
		CHECK(a.call("mmdb_get_status_message", {0, text, 100, out}) == Status::Ok);
		CHECK(a.call("mmdb_get_status_message", {999, text, 100, out}) == Status::InvalidArgument);
		a.string(ip, "localhost");
		CHECK(a.call("mmdb_has_record", {name, ip, out}) == Status::InvalidIp);

		// Packed strings and unterminated strings cannot cross the AMX heap gap.
		a.at(file) = 0x41424344;
		CHECK(a.call("mmdb_reload", {name, file}) == Status::InvalidArgument);
		a.at(16380) = 'a';
		CHECK(a.call("mmdb_reload", {name, 16380}) == Status::InvalidArgument);
		a.string(name, "bad/name");
		CHECK(a.call("mmdb_is_loaded", {name, out}) == Status::InvalidArgument);
		a.string(name, "db");
		a.string(file, "missing.mmdb");
		CHECK(a.call("mmdb_reload", {name, file}) == Status::OpenFailed);
		CHECK(a.call("mmdb_is_loaded", {name, out}) == Status::Ok && a.at(out) == 1);
		bridge.detach(&a.amx);
		CHECK(service.size() == 1);
		CHECK(a.call("mmdb_unload", {name}) == Status::InvalidArgument);
		CHECK(bridge.attach(&a.amx) == Status::Ok);
		CHECK(a.call("mmdb_is_loaded", {name, out}) == Status::Ok && a.at(out) == 0);
		CHECK(b.call("mmdb_is_loaded", {name, out}) == Status::Ok && b.at(out) == 1);
		CHECK(b.call("mmdb_unload", {name}) == Status::Ok);
		CHECK(service.size() == 0);
		std::cout << checks << " native checks passed\n";
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
