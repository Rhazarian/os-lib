#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <queue>
#include <functional>
#include <memory>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <cstring>

#include "huffman.h"

namespace {

	typedef unsigned char byte_t;
	typedef uint64_t code_t;
	constexpr uint8_t bits_in_byte = std::numeric_limits<byte_t>::digits;
	constexpr uint8_t bits_in_code = std::numeric_limits<code_t>::digits;

	struct bit_istream {
		std::array<byte_t, 1u << 16u> buffer{};
		uint8_t bit_pos{};
		size_t pos{};
		size_t size{};
		std::istream& istream;

		explicit bit_istream(std::istream& istream) : istream(istream) {
			check_buffer();
		}

		void check_buffer()
		{
			if (size_t shift = size - pos; shift < 2) {
				for (size_t i = 0; i < shift; ++i) {
					buffer[i] = buffer[pos + i];
				}
				static_assert(sizeof(char) == sizeof(byte_t));
				istream.read(reinterpret_cast<char*>(buffer.data() + shift), buffer.size() - shift);
				size = shift + istream.gcount();
				pos = 0;
			}
		}

		bool read_bit()
		{
			assert(pos < size);
			bool bit = (buffer[pos] & (1 << (bits_in_byte - 1 - bit_pos))) != 0;
			++bit_pos;
			if (bit_pos == bits_in_byte) {
				bit_pos = 0;
				++pos;
				check_buffer();
			}
			return bit;
		};

		uint8_t read_8_bits()
		{
			assert(pos < size);
			uint8_t bits = (buffer[pos] << bit_pos) | (buffer[pos + 1] >> (bits_in_byte - bit_pos));
			++pos;
			check_buffer();
			return bits;
		}

		bool has_more()
		{
			return !istream.fail() || pos < size;
		}

		void rewind()
		{
			istream.clear();
			istream.seekg(std::ios::beg);
			pos = size = bit_pos = 0;
			check_buffer();
		}

	};

	struct bit_ostream {
		std::array<byte_t, 1u << 16u> buffer{};
		uint8_t bit_pos{};
		size_t size{};
		std::ostream& ostream;

		explicit bit_ostream(std::ostream& ostream) : ostream(ostream) { }

		void flush_buffer()
		{
			static_assert(sizeof(char) == sizeof(byte_t));
			ostream.write(reinterpret_cast<const char*>(buffer.data()), size + (bit_pos != 0));
			size = bit_pos = 0;
			buffer.fill(0);
		}

		void write_bits(code_t bits, uint8_t count)
		{
			assert(count <= bits_in_code);
			bits <<= bits_in_code - count;
			for (uint8_t i = 0; i < count; ++i) {
				decltype(bits) mask = (code_t(1) << (bits_in_code - 1 - i));
				buffer[size] |= (byte_t((bits & mask) != 0)) << (bits_in_byte - 1 - bit_pos);
				++bit_pos;
				if (bit_pos == bits_in_byte) {
					bit_pos = 0;
					++size;
					if (size == buffer.size()) {
						flush_buffer();
					}
				}
			}
		}

		~bit_ostream()
		{
			flush_buffer();
		}

	};

	constexpr size_t ALPHABET_SIZE = 1u << bits_in_byte;

	struct symbol {
		byte_t byte;
		size_t count;
		code_t code;
		uint8_t bits;

		symbol(byte_t byte, size_t count, code_t code, uint8_t bits) : byte(byte), count(count), code(code), bits(bits) { }
	};

	std::vector<symbol> build_histogram(bit_istream& istream)
	{
		std::vector<symbol> histogram;
		histogram.reserve(ALPHABET_SIZE);
		for (size_t i = 0; i < ALPHABET_SIZE; ++i) {
			histogram.emplace_back(i, 0, 0, 0);
		}
		while (istream.has_more()) {
			static_assert(bits_in_byte == 8);
			++histogram[istream.read_8_bits()].count;
		}
		istream.rewind();
		return histogram;
	}

	struct encode_node {
		byte_t byte{};
		size_t count{};
		std::shared_ptr<encode_node> left = nullptr;
		std::shared_ptr<encode_node> right = nullptr;

		encode_node() = default;

		encode_node(byte_t byte, size_t count) : byte(byte), count(count) { }

		encode_node(encode_node left, encode_node right) : count(left.count + right.count),
			left(std::make_shared<encode_node>(std::move(left))), right(std::make_shared<encode_node>(std::move(right))) { }
	};

	void store_tree(const encode_node& node, code_t code, uint8_t bits, std::vector<symbol>& histogram,
		bit_ostream& ostream)
	{
		assert(bits <= bits_in_code);
		if (!node.left && !node.right) {
			ostream.write_bits(1, 1);
			ostream.write_bits(node.byte, bits_in_byte);
			histogram[node.byte].code = code;
			histogram[node.byte].bits = bits;
			return;
		}
		else {
			ostream.write_bits(0, 1);
		}
		++bits;
		store_tree(*node.left, (code << 1) | 0, bits, histogram, ostream);
		store_tree(*node.right, (code << 1) | 1, bits, histogram, ostream);
	}

	constexpr size_t MAX_TREE_NODES = 511;

	void make_tree(std::vector<symbol>& histogram, bit_ostream& ostream)
	{
		std::array<encode_node, MAX_TREE_NODES> nodes{};
		std::function<bool(encode_node, encode_node)> comparator = [](encode_node const& lhs, encode_node const& rhs) {
			return lhs.count > rhs.count;
		};
		std::priority_queue<encode_node, std::vector<encode_node>, decltype(comparator)> queue(comparator);
		for (auto const& symbol : histogram) {
			if (symbol.count > 0) {
				queue.emplace(symbol.byte, symbol.count);
			}
		}
		while (queue.size() != 1) {
			auto lhs = queue.top();
			queue.pop();
			auto rhs = queue.top();
			queue.pop();
			queue.emplace(lhs, rhs);
		}
		auto root = queue.top();
		store_tree(root, 0, !root.left && !root.right, histogram, ostream);
	}

	struct decode_node {
		byte_t byte;
		std::shared_ptr<decode_node> children[2];

		explicit decode_node(byte_t byte) : byte(byte), children{ nullptr, nullptr } { }

		decode_node(decode_node left, decode_node right) : byte(0),
			children{ std::make_shared<decode_node>(std::move(left)), std::make_shared<decode_node>(std::move(right)) } { }
	};

	decode_node recover_tree(bit_istream& istream)
	{
		if (istream.read_bit()) {
			static_assert(bits_in_byte == 8);
			byte_t byte = istream.read_8_bits();
			return decode_node(byte);
		}
		auto left = recover_tree(istream);
		auto right = recover_tree(istream);
		return decode_node(left, right);
	}

}

void huffman_compress(std::istream& in, std::ostream& out)
{
	auto istream = bit_istream(in);
	auto histogram = build_histogram(istream);
	auto file_size = size_t(0);
	for (const auto& symbol : histogram) {
		file_size += symbol.count;
	}
	char bytes[sizeof(decltype(file_size))];
	std::memcpy(bytes, &file_size, sizeof(bytes));
	for (const char byte : bytes) {
		out.put(byte);
	}
	if (file_size == 0) {
		return;
	}
	auto ostream = bit_ostream(out);
	make_tree(histogram, ostream);
	while (istream.has_more()) {
		static_assert(bits_in_byte == 8);
		const auto byte = istream.read_8_bits();
		ostream.write_bits(histogram[byte].code, histogram[byte].bits);
	}
}

void huffman_decompress(std::istream& in, std::ostream& out)
{
	auto file_size = size_t();
	char bytes[sizeof(decltype(file_size))];
	for (char& byte : bytes) {
		if (const auto tmp = in.get(); tmp == std::char_traits<char>::eof()) {
			throw std::runtime_error("not enough data");
		}
		else {
			byte = static_cast<char>(tmp);
		}
	}
	std::memcpy(&file_size, bytes, sizeof(file_size));
	if (file_size == 0) {
		return;
	}
	auto istream = bit_istream(in);
	const auto root = recover_tree(istream);
	for (size_t i = 0; i < file_size; ++i) {
		if (!istream.has_more()) {
			throw std::runtime_error("not enough data");
		}
		auto* node = &root;
		while (!(!node->children[0] && !node->children[1])) {
			node = node->children[istream.read_bit()].get();
		}
		out.put(node->byte);
	}
}
