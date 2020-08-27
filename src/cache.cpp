// Copyright (c) 2019-2020, Tom Westerhout
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "cache.hpp"
#include "error_handling.hpp"
#include <omp.h>
#include <algorithm>
#include <forward_list>
#include <numeric>

namespace lattice_symmetries {

namespace {
    auto generate_ranges(tcb::span<uint64_t const> states, unsigned const bits)
    {
        LATTICE_SYMMETRIES_ASSERT(0 < bits && bits <= 32, "invalid bits");
        constexpr auto empty = std::make_pair(~uint64_t{0}, uint64_t{0});
        auto const     size  = uint64_t{1} << bits;
        auto const     mask  = size - 1U;

        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        ranges.reserve(size);
        auto const*       first = states.data();
        auto const* const last  = first + states.size();
        auto const* const begin = first;
        for (auto i = uint64_t{0}; i < size; ++i) {
            auto element = empty;
            if (first != last && ((*first) & mask) == i) {
                element.first = static_cast<uint64_t>(first - begin);
                ++first;
                ++element.second;
                while (first != last && ((*first) & mask) == i) {
                    ++element.second;
                    ++first;
                }
            }
            ranges.push_back(element);
        }
        return ranges;
    }

    template <bool FixedHammingWeight> auto next_state(uint64_t const v) noexcept -> uint64_t
    {
        if constexpr (FixedHammingWeight) {
            auto const t = v | (v - 1U); // t gets v's least significant 0 bits set to 1
            // Next set to 1 the most significant bit to change,
            // set to 0 the least significant ones, and add the necessary 1 bits.
            return (t + 1U)
                   | (((~t & -~t) - 1U) >> (static_cast<unsigned>(__builtin_ctzl(v)) + 1U));
        }
        else {
            return v + 1;
        }
    }

    template <bool FixedHammingWeight>
    auto generate_states_task(uint64_t current, uint64_t const upper_bound,
                              tcb::span<batched_small_symmetry_t const> batched_symmetries,
                              tcb::span<small_symmetry_t const>         symmetries,
                              std::vector<uint64_t>&                    states) -> void
    {
        if constexpr (FixedHammingWeight) {
            LATTICE_SYMMETRIES_ASSERT(popcount(current) == popcount(upper_bound),
                                      "current and upper_bound must have the same Hamming weight");
        }
        auto const handle = [&batched_symmetries, &symmetries,
                             &states](uint64_t const x) noexcept -> void {
            uint64_t             repr;
            std::complex<double> character;
            double               norm;
            get_state_info(batched_symmetries, symmetries, x, repr, character, norm);
            if (repr == x && norm > 0.0) { states.push_back(x); }
        };

        for (; current < upper_bound; current = next_state<FixedHammingWeight>(current)) {
            handle(current);
        }
        LATTICE_SYMMETRIES_ASSERT(current == upper_bound, "");
        handle(current);
    }

    auto generate_states_task(bool fixed_hamming_weight, uint64_t current,
                              uint64_t const                            upper_bound,
                              tcb::span<batched_small_symmetry_t const> batched_symmetries,
                              tcb::span<small_symmetry_t const>         symmetries,
                              std::vector<uint64_t>&                    states) -> void
    {
        if (fixed_hamming_weight) {
            return generate_states_task<true>(current, upper_bound, batched_symmetries, symmetries,
                                              states);
        }
        return generate_states_task<false>(current, upper_bound, batched_symmetries, symmetries,
                                           states);
    }

    template <bool FixedHammingWeight>
    auto split_into_tasks(uint64_t current, uint64_t const bound, unsigned chunk_size)
        -> std::vector<std::pair<uint64_t, uint64_t>>
    {
        --chunk_size;
        auto const hamming_weight = popcount(current);
        auto       ranges         = std::vector<std::pair<uint64_t, uint64_t>>{};
        for (;;) {
            if (bound - current <= chunk_size) {
                ranges.emplace_back(current, bound);
                break;
            }
            auto next = FixedHammingWeight ? closest_hamming(current + chunk_size, hamming_weight)
                                           : current + chunk_size;
            LATTICE_SYMMETRIES_ASSERT(next >= current, "");
            if (next >= bound) {
                ranges.emplace_back(current, bound);
                break;
            }
            ranges.emplace_back(current, next);
            current = next_state<FixedHammingWeight>(next);
        }
        return ranges;
    }

    auto get_bounds(unsigned const number_spins, std::optional<unsigned> hamming_weight) noexcept
        -> std::pair<uint64_t, uint64_t>
    {
        if (hamming_weight.has_value()) {
            if (*hamming_weight == 0U) { return {0U, 0U}; }
            if (*hamming_weight == 64U) { return {~uint64_t{0}, ~uint64_t{0}}; }
            auto const current = ~uint64_t{0} >> (64U - *hamming_weight);
            auto const bound   = number_spins > *hamming_weight
                                   ? (current << (number_spins - *hamming_weight))
                                   : current;
            return {current, bound};
        }
        auto const current = uint64_t{0};
        auto const bound   = ~uint64_t{0} >> (64U - number_spins);
        return {current, bound};
    }
} // namespace

auto split_into_tasks(unsigned number_spins, std::optional<unsigned> hamming_weight,
                      unsigned const chunk_size) -> std::vector<std::pair<uint64_t, uint64_t>>
{
    LATTICE_SYMMETRIES_ASSERT(0 < number_spins && number_spins <= 64, "invalid number of spins");
    LATTICE_SYMMETRIES_ASSERT(!hamming_weight.has_value() || *hamming_weight <= number_spins,
                              "invalid hamming weight");
    auto const [current, bound] = get_bounds(number_spins, hamming_weight);
    if (hamming_weight.has_value()) { return split_into_tasks<true>(current, bound, chunk_size); }
    return split_into_tasks<false>(current, bound, chunk_size);
}

auto closest_hamming(uint64_t x, unsigned const hamming_weight) noexcept -> uint64_t
{
    LATTICE_SYMMETRIES_ASSERT(hamming_weight <= 64, "invalid hamming weight");
    auto weight = popcount(x);
    if (weight > hamming_weight) {
        auto const max_value =
            hamming_weight == 0 ? uint64_t{0} : (~uint64_t{0} << (64U - hamming_weight));
        // Keep clearing lowest bits until we reach the desired Hamming weight.
        for (auto i = 0U; weight > hamming_weight; ++i) {
            LATTICE_SYMMETRIES_ASSERT(i < 64U, "index out of bounds");
            if (test_bit(x, i)) {
                clear_bit(x, i);
                --weight;
            }
        }
        if (x < max_value) { x = next_state<true>(x); }
    }
    else if (weight < hamming_weight) {
        // Keep setting lowest bits until we reach the desired Hamming weight.
        for (auto i = 0U; weight < hamming_weight; ++i) {
            LATTICE_SYMMETRIES_ASSERT(i < 64U, "index out of bounds");
            if (!test_bit(x, i)) {
                set_bit(x, i);
                ++weight;
            }
        }
    }
    return x;
}

auto generate_states(tcb::span<batched_small_symmetry_t const> batched,
                     tcb::span<small_symmetry_t const> other, unsigned const number_spins,
                     std::optional<unsigned> const hamming_weight)
    -> std::forward_list<std::vector<uint64_t>>
{
    LATTICE_SYMMETRIES_CHECK(0 < number_spins && number_spins <= 64, "invalid number of spins");
    LATTICE_SYMMETRIES_CHECK(!hamming_weight.has_value() || *hamming_weight <= number_spins,
                             "invalid hamming weight");

    auto const chunk_size = [number_spins, hamming_weight]() {
        auto const number_chunks    = 100U * static_cast<unsigned>(omp_get_max_threads());
        auto const [current, bound] = get_bounds(number_spins, hamming_weight);
        return std::max((bound - current) / number_chunks, 1UL);
    }();
    auto const ranges = split_into_tasks(number_spins, hamming_weight, chunk_size);
    auto       states = std::forward_list<std::vector<uint64_t>>{};
#pragma omp parallel default(none) firstprivate(hamming_weight)                                    \
    shared(batched, other, ranges, states)
    {
        for (auto const [current, bound] : ranges) {
#pragma omp single nowait
            {
                auto* chunk = std::addressof(states.emplace_front());
                chunk->reserve(1048576UL / sizeof(uint64_t));
#pragma omp task default(none) firstprivate(hamming_weight, current, bound, chunk)                 \
    shared(batched, other)
                {
                    generate_states_task(hamming_weight.has_value(), current, bound, batched, other,
                                         *chunk);
                }
            }
        }
    }
    // we were pushing to the front
    states.reverse();
    return states;
}

namespace {
    auto concatenate(std::forward_list<std::vector<uint64_t>> const& chunks)
    {
        auto r = std::vector<uint64_t>{};
        r.reserve(std::accumulate(std::begin(chunks), std::end(chunks), size_t{0},
                                  [](auto acc, auto const& x) { return acc + x.size(); }));
        std::for_each(std::begin(chunks), std::end(chunks),
                      [&r](auto const& x) { r.insert(std::end(r), std::begin(x), std::end(x)); });
        return r;
    }
} // namespace

basis_cache_t::basis_cache_t(tcb::span<batched_small_symmetry_t const> batched,
                             tcb::span<small_symmetry_t const> other, unsigned number_spins,
                             std::optional<unsigned> hamming_weight,
                             std::vector<uint64_t>   _unsafe_states)
    : _states{_unsafe_states.empty()
                  ? concatenate(generate_states(batched, other, number_spins, hamming_weight))
                  : std::move(_unsafe_states)}
    , _ranges{generate_ranges(_states, bits)}
{}

auto basis_cache_t::states() const noexcept -> tcb::span<uint64_t const> { return _states; }

auto basis_cache_t::number_states() const noexcept -> uint64_t { return _states.size(); }

auto basis_cache_t::index(uint64_t const x) const noexcept -> outcome::result<uint64_t>
{
    using std::begin, std::end;
    constexpr auto mask  = (1U << bits) - 1U;
    auto const&    range = _ranges[x & mask];
    auto const     first = std::next(begin(_states), static_cast<ptrdiff_t>(range.first));
    auto const     last  = std::next(first, static_cast<ptrdiff_t>(range.second));
    auto const     i     = std::lower_bound(first, last, x);
    if (i == last) { return outcome::failure(LS_NOT_A_REPRESENTATIVE); }
    LATTICE_SYMMETRIES_ASSERT(*i == x, "");
    return static_cast<uint64_t>(std::distance(begin(_states), i));
}

} // namespace lattice_symmetries
