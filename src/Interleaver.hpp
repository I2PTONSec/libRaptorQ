/*
 * Copyright (c) 2015, Luca Fulchir<luca@fulchir.it>, All rights reserved.
 *
 * This file is part of "libRaptorQ".
 *
 * libRaptorQ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * libRaptorQ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * and a copy of the GNU Lesser General Public License
 * along with libRaptorQ.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RAPTORQ_INTERLEAVER_HPP
#define RAPTORQ_INTERLEAVER_HPP

#include "common.hpp"
#include "multiplication.hpp"
#include "table2.hpp"
#include <cassert>
#include <cmath>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

// force promotion to double in division
namespace {
float RAPTORQ_LOCAL div_floor (const float a, const float b);
float RAPTORQ_LOCAL div_ceil (const float a, const float b);

float div_floor (const float a, const float b)
{
	return std::floor (a / b);
}
float div_ceil (const float a, const float b)
{
	return std::ceil (a / b);
}
}

namespace RaptorQ {
namespace Impl {

void test (void);

//
// Partition: see RFC6330: each object is partitioned in
//		N1 blocks of size S1, plus N2 blocks of size S2. This class tracks it
//
class RAPTORQ_API Partition
{
public:
	Partition() = default;
	// partition something into "num1" partitions of "size1" and "num2"
	// of "size2"
	// still better than the TL, TS, NL, NL in RFC6330...
	Partition (const uint64_t obj_size, const uint8_t partitions)
	{
		uint16_t size_1, size_2, blocks_1, blocks_2;

		size_1 = static_cast<uint16_t> (div_ceil (obj_size, partitions));
		size_2 = static_cast<uint16_t> (div_floor (obj_size, partitions));
		blocks_1 = obj_size - size_2 * partitions;
		blocks_2 = partitions - blocks_1;

		if (blocks_1 == 0)
			size_1 = 0;
		part1 = {blocks_1, size_1};
		part2 = {blocks_2, size_2};
	}

	uint16_t size (const uint8_t part_number) const
	{
		assert(part_number < 2 && "partition: only two partitions exists");
		if (part_number == 0)
			return std::get<1>(part1);
		return std::get<1>(part2);
	}
	uint16_t num (const uint8_t part_number) const
	{
		assert(part_number < 2 && "partition: only two partitions exists");
		if (part_number == 0)
			return std::get<0>(part1);
		return std::get<0>(part2);
	}
	uint16_t tot (const uint8_t part_number) const
	{
		// num * size
		if (part_number == 0)
			return std::get<0>(part1) * std::get<1>(part1);
		return std::get<0>(part2) * std::get<1>(part2);
	}
private:
	// PAIR: amount, size
	std::pair<uint16_t, uint16_t> part1, part2;
};

template <typename T>
class RAPTORQ_LOCAL Symbol_Wrap
{
public:
	Symbol_Wrap (const uint8_t *raw, const uint16_t size) : _raw (raw),
																	_size (size)
	{}

	Symbol_Wrap<T>& operator= (const Symbol_Wrap<T> &a)
	{
		assert (_raw != nullptr && "Encoded_Symbol raw == nullptr");
		for (size_t i = 0; i < _size * sizeof(T); ++i)
			_raw[i] = a._raw[i];
		return *this;
	}
	Symbol_Wrap<T>& operator+= (const Symbol_Wrap<T> &a)
	{
		assert (_raw != nullptr && "Encoded_Symbol raw == nullptr");
		for (size_t i = 0; i < _size * sizeof(T); ++i)
			_raw[i] ^= a._raw[i];
		return *this;
	}
	Symbol_Wrap<T>& operator*= (const Symbol_Wrap<T> &a)
	{
		assert (_raw != nullptr && "Encoded_Symbol raw == nullptr");
		for (size_t i = 0; i < _size * sizeof(T); ++i) {
			if (_raw[i] == 0 || a._raw[i] == 0) {
				_raw[i] = 0;
			} else {
				_raw[i] = Impl::oct_exp[Impl::oct_log[_raw[i]] +
											Impl::oct_exp[a._raw[i]]];
			}
		}
		return *this;
	}
	Symbol_Wrap<T>& operator/= (const Symbol_Wrap<T> &a)
	{
		assert (_raw != nullptr && "Encoded_Symbol raw == nullptr");
		for (size_t i = 0; i < _size * sizeof(T); ++i) {
			if (_raw[i] != 0) {
				_raw[i] = Impl::oct_exp[Impl::oct_log[_raw[i]] -
										Impl::oct_exp[a._raw[i]] + 255];
			}
		}
		return *this;
	}
private:
	const uint8_t *_raw = nullptr;
	const uint16_t _size;
};

//
// Symbol:
//		Basic unit later on. This is a block of interneaved sub-symbols.
//		see RFC 6330 for details
//		Padding is included here
//
template <typename T>
class RAPTORQ_LOCAL Symbol_it
{
public:
	Symbol_it ();
	Symbol_it (const std::vector<T> *raw, const size_t start,
										const size_t end, const size_t idx,
										const Partition sub_blocks,
										const uint16_t symbol_size,
										const uint16_t symbol_id,
										const uint16_t k)
			:_raw (raw), _start (start), _end (end), _idx(idx),
				_sub_blocks (sub_blocks), _symbol_size (symbol_size),
				_symbol_id (symbol_id), _k(k)
	{}

	constexpr Symbol_it<T> begin() const
	{
		return Symbol_it<T> (_raw, _start, _end, 0, _sub_blocks, _symbol_size,
																_symbol_id, _k);
	}
	constexpr Symbol_it<T> end() const
	{
		return Symbol_it<T> (_raw, _start, _end,
									 _sub_blocks.tot (0) + _sub_blocks.tot (1),
									_sub_blocks, _symbol_size, _symbol_id, _k);
	}
	T operator[] (const size_t pos) const
	{
		size_t i;
		if (pos < _sub_blocks.tot (0)) {
			auto sub_blk_id = pos / _sub_blocks.size (0);
			i = _start +
					sub_blk_id * _k * _sub_blocks.size (0) +// right sub block
					_symbol_id * _sub_blocks.size (0) +	// get right subsymbol
					pos % _sub_blocks.size (0);			// get right alignment
		} else {
			auto pos_part2 = pos - _sub_blocks.tot (0);
			auto sub_blk_id = pos_part2 / _sub_blocks.size (1);
			i = _start + _sub_blocks.tot (0) * _k +	// skip previous partition
					sub_blk_id * _k * _sub_blocks.size (1) +// right sub block
					_symbol_id * _sub_blocks.size (1) +	// get right subsymbol
					pos_part2 % _sub_blocks.size (1);	// get right alignment
		}
		if (i >= _raw->size())
			return 0;	// PADDING.
		return (*_raw)[i];
	}
	T operator* () const
	{
		return (*this)[_idx];
	}
	Symbol_it<T> operator++ (int i) const
	{
		if (_idx + i >=  _sub_blocks.tot (0) + _sub_blocks.tot (1))
			return end();
		return Symbol_it<T> (_raw, _start, _end, _idx + i, _sub_blocks,
												_symbol_size, _symbol_id, _k);
	}
	Symbol_it<T>& operator++()
	{
		if (_idx <  _sub_blocks.tot (0) + _sub_blocks.tot (1))
			++_idx;
		return *this;
	}
	bool operator== (const Symbol_it<T> &s) const
	{
		return _idx == s._idx;
	}
	bool operator!= (const Symbol_it<T> &s) const
	{
		return _idx != s._idx;
	}

private:
	const std::vector<T> *_raw;
	const size_t _start, _end;
	size_t _idx;
	const Partition _sub_blocks;
	const uint16_t _symbol_size, _symbol_id, _k;
};

//
// Source_Block:
//		First unit of partitioning for the object to be transferred.
//
template <typename T>
class RAPTORQ_LOCAL Source_Block
{
public:
	Source_Block (const std::vector<T> *raw, const size_t start,
										const size_t end, const size_t idx,
										const Partition sub_blocks,
										const uint16_t symbol_size)
			:_raw (raw), _start (start), _end (end), _idx(idx),
			  _sub_blocks(sub_blocks), _symbol_size (symbol_size),
			  _symbols ((end - start) / symbol_size)
	{}

	constexpr Source_Block<T> begin() const
	{
		return Source_Block (_raw, _start, _end, 0, _sub_blocks, _symbol_size);
	}
	constexpr Source_Block<T> end() const
	{
		return Source_Block<T> (_raw, _start, _end, _end,
													_sub_blocks, _symbol_size);
	}
	const Symbol_it<T> operator[] (const size_t symbol_id) const
	{
		if (symbol_id <  _symbols) {
			return Symbol_it<T> (_raw, _start, _end, 0,
										_sub_blocks, _symbol_size, symbol_id,
																	_symbols);
		}
		// out of range.
		return Symbol_it<T> (_raw, 0, 0, 0, _sub_blocks, _symbol_size, 0, 0);
	}
	const Symbol_it<T> operator* () const
	{
		return (*this)[_idx];
	}
	const Source_Block<T> operator++ (int i) const
	{
		if (_idx + i >= _symbols)
			return end();
		return Source_Block<T> (_raw, _start, _end, _idx + i,
													_sub_blocks, _symbol_size);
	}
	const Source_Block<T>& operator++ ()
	{
		if (_idx < _symbols)
			++_idx;
		return *this;
	}
private:
	const std::vector<T> *_raw;
	const size_t _start, _end;
	size_t _idx;
	const Partition _sub_blocks;
	const uint16_t _symbol_size, _symbols;
};


//
// Interleaver
//		Take an object file, and handle the source block, sub block, sub symbol
//		and symbol division and interleaving, and padding.
//
template <typename T>
class RAPTORQ_API Interleaver
{
public:
	operator bool() const;	// true => all ok

	Interleaver (const std::vector<T> *raw, const uint16_t min_subsymbol_size,
											const size_t max_block_decodable,
											const uint16_t symbol_syze);

	Source_Block<T>& begin() const;
	Source_Block<T>& end() const;
	Interleaver<T>& operator++();
	Source_Block<T> operator*() const;
	Source_Block<T> operator[] (uint8_t source_block_id) const;
	Partition get_partition() const;
	uint16_t source_symbols(const uint8_t SBN) const;
	uint8_t blocks () const;
	uint16_t sub_blocks () const;
	uint16_t symbol_size() const;
protected:
private:
	const std::vector<T> *_raw;
	uint16_t _sub_blocks, _source_symbols, iterator_idx = 0;
	const uint16_t _symbol_size;
	uint8_t _alignment, _source_blocks;

	// Please everyone take a moment to tank the RFC6330 guys for
	// giving such wonderfully self-explanatory names to *everything*.
	// Same names are kept to better track the rfc
	// (SIZE, SIZE, BLOCKNUM, BLOCKNUM) for:
	Partition _source_part, _sub_part;
};


//TODO: constexpr K'_max = 56403 in some .hpp

///////////////////////////////////
//
// IMPLEMENTATION OF ABOVE TEMPLATE
//
///////////////////////////////////

template <typename T>
Interleaver<T>::Interleaver (const std::vector<T> *raw,
											const uint16_t min_subsymbol_size,
											const size_t max_block_decodable,
											const uint16_t symbol_size)
	:_raw (raw), _symbol_size (symbol_size), _alignment (sizeof(T))
{
	// all parameters are in octets
	static_assert(std::is_unsigned<T>::value,
					"RaptorQ:Interleaver can only be used with unsigned types");
	assert(_symbol_size >= _alignment &&
					"RaptorQ: symbol_size must be >= alignment");
	assert((_symbol_size % _alignment) == 0 &&
					"RaptorQ: symbol_size must be multiple of alignment");
	assert(min_subsymbol_size >= _alignment &&
					"RaptorQ: minimum subsymbol must be at least aligment");
	assert((min_subsymbol_size % _alignment) == 0 &&
				"RaptorQ: minimum subsymbol must be multiple of alignment");
	// derive number of source blocks and sub blocks. seed RFC 6330, pg 8
	std::vector<uint16_t> sizes;
	const double Kt = div_ceil(raw->size() * sizeof(T), symbol_size);
	const size_t N_max = static_cast<size_t> (div_floor (_symbol_size,
														min_subsymbol_size));

	// symbol_size must be a multiple of our alignment
	if (_symbol_size % _alignment != 0 || min_subsymbol_size < _alignment ||
									(min_subsymbol_size % _alignment) != 0 ||
											min_subsymbol_size > symbol_size) {
		// nonsense configurations. refuse to work.
		_alignment = 0;
		return;
	}

	// rfc 6330, pg 8
	size_t tmp;
	sizes.reserve (N_max);
	// find our KL(n), for each n
	for (tmp = 1; tmp <= N_max; ++tmp) {
		auto upper_bound = max_block_decodable / (_alignment *
									div_ceil (_symbol_size, _alignment * tmp));
		size_t idx;
		for (idx = 0; idx < RaptorQ::Impl::K_padded.size(); ++idx) {
			if (RaptorQ::Impl::K_padded[idx] > upper_bound)
				break;
		}
		// NOTE: tmp starts from 1, but "sizes" stores from 0.
		sizes.push_back (RaptorQ::Impl::K_padded[idx == 0 ? 0 : --idx]);
	}
	_source_blocks = static_cast<uint16_t> (div_ceil (Kt, sizes[N_max - 1]));
	tmp = static_cast<size_t> (div_ceil (Kt, _source_blocks));
	for (size_t i = 0; i < sizes.size(); ++i) {
		// rfc: ceil (Kt / Z) <= KL(n)
		if (tmp <= sizes[i]) {
			_sub_blocks = i + 1;	// +1: see above note
			break;
		}
	}
	assert(div_ceil (div_ceil (_raw->size(), _symbol_size),
													_source_blocks) <= 56403 &&
						"RaptorQ: RFC: ceil(ceil(F/T)/Z must be <= K'_max");
	if (_source_blocks == 0 || _sub_blocks == 0 ||
					symbol_size < _alignment || symbol_size % _alignment != 0 ||
							div_ceil (div_ceil ( _raw->size(), _symbol_size),
													_source_blocks) > 56403) {
		_alignment = 0;
		return;
	}
	// blocks and size for source block partitioning
	_source_part = Partition (static_cast<uint64_t> (Kt), _source_blocks);

	_source_symbols = _source_part.size(0) + _source_part.size(1);

	// blocks and size for sub-block partitioning
	_sub_part = Partition (_symbol_size / _alignment, _sub_blocks);
}

template <typename T>
Interleaver<T>::operator bool() const
{
	// true => all ok
	return _alignment != 0;
}

template <typename T>
Source_Block<T> Interleaver<T>::operator[] (uint8_t source_block_id) const
{
	// now we start working with multiples of T.
	// identify the start and end of the requested block.
	auto al_symbol_size = _symbol_size / sizeof(T);

	if (source_block_id < _source_part.num(0)) {
		auto sb_start = source_block_id * _source_part.size(0) * al_symbol_size;
		auto sb_end = (source_block_id + 1) * _source_part.size(0) *
																al_symbol_size;

		return Source_Block<T> (_raw, sb_start, sb_end, 0, _sub_part,
																al_symbol_size);
	} else if (source_block_id - _source_part.num(0) < _source_part.num(1)) {
		// start == all the previous partition
		auto sb_start = _source_part.tot(0) * al_symbol_size +
									// plus some blocks of the new partition
									(source_block_id - _source_part.num(0)) *
										_source_part.size(1) * al_symbol_size;
		auto sb_end =  sb_start + _source_part.size(1) * al_symbol_size;

		return Source_Block<T> (_raw, sb_start, sb_end, 0, _sub_part,
																al_symbol_size);
	} else  {
		assert(false && "RaptorQ: source_block_id out of range");
		return Source_Block<T> (_raw, 0, 0, 0, _sub_part, al_symbol_size);
	}
}

template <typename T>
uint16_t Interleaver<T>::symbol_size() const
{
	// return the number of alignments, to make things easier
	return _symbol_size / sizeof(T);
}

template <typename T>
Partition Interleaver<T>::get_partition() const
{
	return _source_part;
}


template <typename T>
uint16_t Interleaver<T>::source_symbols (const uint8_t SBN) const
{
	if (SBN < _source_part.num (0))
		return _source_part.size (0);
	if (SBN - _source_part.num (0) < _source_part.num (1))
		return _source_part.size (1);
	return 0;
}

template <typename T>
uint8_t Interleaver<T>::blocks () const
{
	return _source_part.num (0) + _source_part.num (1);
}

template <typename T>
uint16_t Interleaver<T>::sub_blocks () const
{
	return _sub_part.num (0) + _sub_part.num (1);
}

template <typename T>
Source_Block<T>& Interleaver<T>::begin() const
{
	return this[0];
}

template <typename T>
Source_Block<T>& Interleaver<T>::end() const
{
	return this[_source_blocks + 1];
}

template <typename T>
Interleaver<T>& Interleaver<T>::operator++()
{
	++iterator_idx;
	return *this;
}

template <typename T>
Source_Block<T> Interleaver<T>::operator*() const
{
	return this[iterator_idx];
}

}	// namespace Impl
}	// namespace RaptorQ

#endif
