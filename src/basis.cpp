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

#include "basis.hpp"
#include "cache.hpp"
#include "cpu/state_info.hpp"
#include <algorithm>

namespace lattice_symmetries {

namespace {
    template <class T> auto extract(tcb::span<ls_symmetry const> symmetries) -> std::vector<T>
    {
        auto r = std::vector<T>{};
        r.reserve(symmetries.size());
        std::transform(std::begin(symmetries), std::end(symmetries), std::back_inserter(r),
                       [](auto const& x) { return std::get<T>(x.payload); });
        return r;
    }

    auto split_into_batches(tcb::span<small_symmetry_t const> symmetries)
        // -> std::tuple<std::vector<batched_small_symmetry_t>, std::vector<small_symmetry_t>>
        -> std::tuple<std::vector<batched_small_symmetry_t>,
                      std::optional<batched_small_symmetry_t>, unsigned>
    {
        constexpr auto batch_size = batched_small_symmetry_t::batch_size;
        auto           offset     = 0UL;

        std::vector<batched_small_symmetry_t> batched;
        for (; offset + batch_size <= symmetries.size(); offset += batch_size) {
            batched.emplace_back(symmetries.subspan(offset, batch_size));
        }

        std::vector<small_symmetry_t> other;
        other.reserve(batch_size); // Make sure other is never reallocated
        std::copy(std::next(std::begin(symmetries), static_cast<ptrdiff_t>(offset)),
                  std::end(symmetries), std::back_inserter(other));
        if (other.empty()) { return std::make_tuple(std::move(batched), std::nullopt, 0U); }
        auto const count = other.size();
        for (auto const& s = other.back(); other.size() != batch_size;) {
            other.push_back(s);
        }
        return std::make_tuple(std::move(batched), std::optional{batched_small_symmetry_t{other}},
                               count);
    }
} // namespace

small_basis_t::small_basis_t(ls_group const& group) : cache{nullptr}
{
    auto symmetries = extract<small_symmetry_t>(
        tcb::span{ls_group_get_symmetries(&group), ls_get_group_size(&group)});
    std::tie(batched_symmetries, other_symmetries, number_other_symmetries) =
        split_into_batches(symmetries);
}

big_basis_t::big_basis_t(ls_group const& group)
    : symmetries{extract<big_symmetry_t>(
        tcb::span{ls_group_get_symmetries(&group), ls_get_group_size(&group)})}
{}

} // namespace lattice_symmetries

using namespace lattice_symmetries;

struct ls_spin_basis {
    basis_base_t                             header;
    std::variant<small_basis_t, big_basis_t> payload;

    template <class T>
    explicit ls_spin_basis(std::in_place_type_t<T> tag, ls_group const& group,
                           unsigned const                number_spins,
                           std::optional<unsigned> const hamming_weight, int const spin_inversion)
        : header{{},
                 number_spins,
                 hamming_weight,
                 spin_inversion,
                 ls_get_group_size(&group) > 1 || spin_inversion != 0}
        , payload{tag, group}
    {}

    ls_spin_basis(ls_spin_basis const&) = delete;
    ls_spin_basis(ls_spin_basis&&)      = delete;
    auto operator=(ls_spin_basis const&) -> ls_spin_basis& = delete;
    auto operator=(ls_spin_basis&&) -> ls_spin_basis& = delete;

    ~ls_spin_basis()
    {
        LATTICE_SYMMETRIES_CHECK(load(header.refcount) == 0, "there remain references to object");
    }
};

struct ls_states {
    tcb::span<uint64_t const> payload;
    ls_spin_basis*            parent;

    ls_states(tcb::span<uint64_t const> states, ls_spin_basis const* owner)
        : payload{states}, parent{ls_copy_spin_basis(owner)}
    {}

    ls_states(ls_states const&) = delete;
    ls_states(ls_states&&)      = delete;
    auto operator=(ls_states const&) -> ls_states& = delete;
    auto operator=(ls_states&&) -> ls_states& = delete;

    ~ls_states() { ls_destroy_spin_basis(parent); }
};

struct ls_flat_group {
    unsigned  shape[3];
    uint64_t* masks;
    uint64_t* shifts;
    double*   eigenvalues_real;
    double*   eigenvalues_imag;
    unsigned* sectors;
    unsigned* periodicities;
};

struct ls_flat_spin_basis {
    mutable atomic_count_t refcount;
    unsigned               number_spins;
    int                    hamming_weight;
    int                    spin_inversion;
    ls_flat_group          group;
};

namespace lattice_symmetries {

namespace {
    auto masks_shape(ls_spin_basis const& basis) noexcept -> std::array<unsigned, 3>
    {
        struct get_shape_fn_t {
            auto operator()(small_basis_t const& b) const noexcept
            {
                auto network_depth = 0U;
                if (!b.batched_symmetries.empty()) {
                    network_depth = b.batched_symmetries.front().network.depth;
                }
                else if (b.other_symmetries.has_value()) {
                    network_depth = b.other_symmetries->network.depth;
                }
                auto number_permutations = static_cast<unsigned>(
                    b.batched_symmetries.size() * batched_small_symmetry_t::batch_size
                    + b.number_other_symmetries);
                auto mask_width = 1U;
                return std::array<unsigned, 3>{{network_depth, number_permutations, mask_width}};
            }

            auto operator()(big_basis_t const& b) const noexcept
            {
                auto network_depth = 0U;
                if (!b.symmetries.empty()) { network_depth = b.symmetries.front().network.depth; }
                auto number_permutations = static_cast<unsigned>(b.symmetries.size());
                auto mask_width          = 8U;
                return std::array<unsigned, 3>{{network_depth, number_permutations, mask_width}};
            }
        };
        return std::visit(get_shape_fn_t{}, basis.payload);
    }
} // namespace

struct init_flat_group_contents_fn_t {
    ls_flat_group* g;

    auto operator()(small_basis_t const& b) const noexcept -> void
    {
        // Initializing masks
        auto offset = ptrdiff_t{0};
        for (auto depth = 0U; depth < g->shape[0]; ++depth) {
            for (auto const& s : b.batched_symmetries) {
                for (auto i = 0U; i < batched_small_symmetry_t::batch_size; ++i) {
                    g->masks[offset] = s.network.masks[depth][i];
                    ++offset;
                }
            }
            for (auto i = 0U; i < b.number_other_symmetries; ++i) {
                g->masks[offset] = b.other_symmetries->network.masks[depth][i];
                ++offset;
            }
        }

        // Initializing shifts
        if (g->shifts != nullptr) {
            auto const& s =
                !b.batched_symmetries.empty() ? b.batched_symmetries.front() : *b.other_symmetries;
            for (auto depth = 0U; depth < g->shape[0]; ++depth) {
                g->shifts[depth] = s.network.deltas[depth];
            }
        }

        // Initializing eigenvalues, sectors, and periodicities
        offset = ptrdiff_t{0};
        for (auto const& s : b.batched_symmetries) {
            for (auto i = 0U; i < batched_small_symmetry_t::batch_size; ++i) {
                g->eigenvalues_real[offset] = s.eigenvalues_real[i];
                g->eigenvalues_imag[offset] = s.eigenvalues_imag[i];
                g->sectors[offset]          = s.sectors[i];
                g->periodicities[offset]    = s.periodicities[i];
                ++offset;
            }
        }
        for (auto i = 0U; i < b.number_other_symmetries; ++i) {
            auto const& s               = *b.other_symmetries;
            g->eigenvalues_real[offset] = s.eigenvalues_real[i];
            g->eigenvalues_imag[offset] = s.eigenvalues_imag[i];
            g->sectors[offset]          = s.sectors[i];
            g->periodicities[offset]    = s.periodicities[i];
            ++offset;
        }
    }

    auto operator()(big_basis_t const& b) const noexcept -> void
    {
        // Initializing masks
        auto offset = ptrdiff_t{0};
        for (auto depth = 0U; depth < g->shape[0]; ++depth) {
            for (auto const& s : b.symmetries) {
                for (auto i = 0; i < 8; ++i) {
                    g->masks[offset] = s.network.masks[depth].words[i];
                    ++offset;
                }
            }
        }

        // Initializing shifts
        if (g->shifts != nullptr) {
            auto const& s = b.symmetries.front();
            for (auto depth = 0U; depth < g->shape[0]; ++depth) {
                g->shifts[depth] = s.network.deltas[depth];
            }
        }

        // Initializing eigenvalues, sectors, and periodicities
        offset = ptrdiff_t{0};
        for (auto const& s : b.symmetries) {
            for (auto i = 0U; i < batched_small_symmetry_t::batch_size; ++i) {
                g->eigenvalues_real[offset] = s.eigenvalue.real();
                g->eigenvalues_imag[offset] = s.eigenvalue.imag();
                g->sectors[offset]          = s.sector;
                g->periodicities[offset]    = s.periodicity;
                ++offset;
            }
        }
    }
};

namespace {
    auto init_flat_group_contents(ls_flat_group* group, ls_spin_basis const& basis) noexcept -> void
    {
        std::visit(init_flat_group_contents_fn_t{group}, basis.payload);
    }
} // namespace

struct free_deleter_fn_t {
    template <class T> auto operator()(T* ptr) const noexcept -> void
    {
        if (ptr != nullptr) { std::free(ptr); }
    }
};

template <class T>
auto alloc_aligned_unitialized(size_t alignment, size_t count) noexcept
    -> std::unique_ptr<T, free_deleter_fn_t>
{
    auto* ptr = count > 0 ? std::aligned_alloc(alignment, count * sizeof(T)) : nullptr;
    return std::unique_ptr<T, free_deleter_fn_t>{static_cast<T*>(ptr)};
}

template <class T>
auto alloc_aligned_unitialized(size_t count = 1) noexcept -> std::unique_ptr<T, free_deleter_fn_t>
{
    return alloc_aligned_unitialized<T>(alignof(std::max_align_t), count);
}
} // namespace lattice_symmetries

extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code
ls_convert_to_flat_spin_basis(ls_flat_spin_basis** ptr, ls_spin_basis const* basis)
{
    using namespace lattice_symmetries;
    constexpr auto alignment  = 64U;
    auto           status     = LS_SUCCESS;
    auto const     shape      = masks_shape(*basis);
    auto           flat_basis = alloc_aligned_unitialized<ls_flat_spin_basis>();
    if (flat_basis == nullptr) { return LS_OUT_OF_MEMORY; }
    auto masks  = alloc_aligned_unitialized<uint64_t>(alignment, shape[0] * shape[1] * shape[2]);
    auto shifts = alloc_aligned_unitialized<uint64_t>(alignment, shape[0]);
    auto eigenvalues_real = alloc_aligned_unitialized<double>(alignment, shape[1]);
    auto eigenvalues_imag = alloc_aligned_unitialized<double>(alignment, shape[1]);
    auto sectors          = alloc_aligned_unitialized<unsigned>(alignment, shape[1]);
    auto periodicities    = alloc_aligned_unitialized<unsigned>(alignment, shape[1]);
    if (shape[1] != 0
        && (masks == nullptr || shifts == nullptr || eigenvalues_real == nullptr
            || eigenvalues_imag == nullptr || sectors == nullptr || periodicities == nullptr)) {
        return LS_OUT_OF_MEMORY;
    }
    new (flat_basis.get()) ls_flat_spin_basis{{},
                                              ls_get_number_spins(basis),
                                              ls_get_hamming_weight(basis),
                                              ls_get_spin_inversion(basis),
                                              ls_flat_group{
                                                  {shape[0], shape[1], shape[2]},
                                                  masks.get(),
                                                  shifts.get(),
                                                  eigenvalues_real.get(),
                                                  eigenvalues_imag.get(),
                                                  sectors.get(),
                                                  periodicities.get(),
                                              }};
    init_flat_group_contents(&flat_basis->group, *basis);
    *ptr = flat_basis.release();
    masks.release();
    shifts.release();
    eigenvalues_real.release();
    eigenvalues_imag.release();
    sectors.release();
    periodicities.release();
    return LS_SUCCESS;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_create_spin_basis(ls_spin_basis** ptr,
                                                                        ls_group const* group,
                                                                        unsigned const number_spins,
                                                                        int const hamming_weight,
                                                                        int const spin_inversion)
{
    // NOLINTNEXTLINE: 512 is the max supported system size (i.e. number of bits in ls_bits512)
    if (number_spins == 0 || number_spins > 512) { return LS_INVALID_NUMBER_SPINS; }
    if (auto const n = ls_group_get_number_spins(group);
        n != -1 && number_spins != static_cast<unsigned>(n)) {
        return LS_INVALID_NUMBER_SPINS;
    }
    if (!(hamming_weight == -1
          || (0 <= hamming_weight && hamming_weight <= static_cast<int>(number_spins)))) {
        return LS_INVALID_HAMMING_WEIGHT;
    }
    if (spin_inversion != -1 && spin_inversion != 0 && spin_inversion != 1) {
        return LS_INVALID_SPIN_INVERSION;
    }
    // It's not possible to enforce spin inversion if number of spins up is not exactly
    // number_spins / 2
    if (spin_inversion != 0 && hamming_weight != -1
        && 2 * hamming_weight != static_cast<int>(number_spins)) {
        return LS_INVALID_SPIN_INVERSION;
    }

    auto const using_trivial_group = ls_get_group_size(group) == 0 && spin_inversion != 0;
    ls_group*  trivial_group       = nullptr;
    if (using_trivial_group) {
        auto const status = ls_create_trivial_group(&trivial_group, number_spins);
        LATTICE_SYMMETRIES_CHECK(status == LS_SUCCESS, "failed to create trivial group");
    }
    auto const& group_ref =
        using_trivial_group ? *static_cast<ls_group const*>(trivial_group) : *group;

    auto const need_big = number_spins > 64; // NOLINT: 64 is number of bits in uint64_t
    auto const _hamming_weight =
        hamming_weight == -1 ? std::nullopt : std::optional<unsigned>{hamming_weight};
    auto p = need_big
                 ? std::make_unique<ls_spin_basis>(std::in_place_type_t<big_basis_t>{}, group_ref,
                                                   number_spins, _hamming_weight, spin_inversion)
                 : std::make_unique<ls_spin_basis>(std::in_place_type_t<small_basis_t>{}, group_ref,
                                                   number_spins, _hamming_weight, spin_inversion);
    if (using_trivial_group) { ls_destroy_group(trivial_group); }
    increment(p->header.refcount);
    *ptr = p.release();
    return LS_SUCCESS;
}

extern "C" LATTICE_SYMMETRIES_EXPORT ls_spin_basis* ls_copy_spin_basis(ls_spin_basis const* basis)
{
    LATTICE_SYMMETRIES_ASSERT(load(basis->header.refcount) > 0,
                              "refcount cannot be increased from zero");
    increment(basis->header.refcount);
    // NOLINTNEXTLINE: We really do want const_cast here since the only non-const operation on
    // NOLINTNEXTLINE: ls_spin_basis is ls_build which may be called from on any instance
    return const_cast<ls_spin_basis*>(basis);
}

extern "C" LATTICE_SYMMETRIES_EXPORT void ls_destroy_spin_basis(ls_spin_basis* basis)
{
    if (decrement(basis->header.refcount) == 0) {
        LATTICE_SYMMETRIES_LOG_DEBUG("Destroying basis %p\n", static_cast<void*>(basis));
        std::default_delete<ls_spin_basis>{}(basis);
    }
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT unsigned ls_get_number_spins(ls_spin_basis const* basis)
{
    return basis->header.number_spins;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT unsigned ls_get_number_bits(ls_spin_basis const* basis)
{
    if (std::holds_alternative<big_basis_t>(basis->payload)) {
        return 512U; // NOLINT: number of bits in ls_bits512
    }
    return 64U; // NOLINT: number of bits in uint64_t
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT int ls_get_hamming_weight(ls_spin_basis const* basis)
{
    auto const& m = basis->header.hamming_weight;
    return m.has_value() ? static_cast<int>(*m) : -1;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT int ls_get_spin_inversion(ls_spin_basis const* basis)
{
    return basis->header.spin_inversion;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT bool ls_has_symmetries(ls_spin_basis const* basis)
{
    return basis->header.has_symmetries;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_get_number_states(ls_spin_basis const* basis,
                                                                        uint64_t*            out)
{
    auto const* p = std::get_if<small_basis_t>(&basis->payload);
    if (LATTICE_SYMMETRIES_UNLIKELY(p == nullptr)) { return LS_WRONG_BASIS_TYPE; }
    if (LATTICE_SYMMETRIES_UNLIKELY(p->cache == nullptr)) { return LS_CACHE_NOT_BUILT; }
    *out = p->cache->number_states();
    return LS_SUCCESS;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_get_index(ls_spin_basis const* basis,
                                                                uint64_t const       bits,
                                                                uint64_t*            index)
{
    auto const* p = std::get_if<small_basis_t>(&basis->payload);
    if (LATTICE_SYMMETRIES_UNLIKELY(p == nullptr)) { return LS_WRONG_BASIS_TYPE; }
    if (LATTICE_SYMMETRIES_UNLIKELY(p->cache == nullptr)) { return LS_CACHE_NOT_BUILT; }
    return p->cache->index(bits, index);
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_build(ls_spin_basis* basis)
{
    auto* p = std::get_if<small_basis_t>(&basis->payload);
    if (p == nullptr) { return LS_WRONG_BASIS_TYPE; }
    if (p->cache == nullptr) { p->cache = std::make_unique<basis_cache_t>(basis->header, *p); }
    return LS_SUCCESS;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_build_unsafe(ls_spin_basis* basis,
                                                                   uint64_t const size,
                                                                   uint64_t const representatives[])
{
    auto* p = std::get_if<small_basis_t>(&basis->payload);
    if (p == nullptr) { return LS_WRONG_BASIS_TYPE; }
    if (p->cache == nullptr) {
        std::vector<uint64_t> rs{representatives, representatives + size};
        p->cache = std::make_unique<basis_cache_t>(basis->header, *p, std::move(rs));
    }
    return LS_SUCCESS;
}

namespace lattice_symmetries {
namespace {
    struct get_state_info_visitor_t {
        basis_base_t const&     header;
        ls_bits512 const* const bits;
        ls_bits512* const       representative;
        std::complex<double>&   character;
        double&                 norm;

        auto operator()(small_basis_t const& payload) const noexcept
        {
            get_state_info_64(header, payload, bits->words[0], representative->words[0], character,
                              norm);
        }
        auto operator()(big_basis_t const& payload) const noexcept
        {
            get_state_info_512(header, payload, *bits, *representative, character, norm);
        }
    };
} // namespace
} // namespace lattice_symmetries

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT void ls_get_state_info(ls_spin_basis const* basis,
                                                            ls_bits512 const*    bits,
                                                            ls_bits512*          representative,
                                                            void* character, double* norm)
{
    auto& ch = *reinterpret_cast<std::complex<double>*>(character); // NOLINT
    std::visit(get_state_info_visitor_t{basis->header, bits, representative, ch, *norm},
               basis->payload);
}

extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_is_representative(ls_spin_basis const* basis,
                                                                        uint64_t const       count,
                                                                        uint64_t const       bits[],
                                                                        uint8_t              out[])
{
    auto const* payload = std::get_if<small_basis_t>(&basis->payload);
    if (LATTICE_SYMMETRIES_UNLIKELY(payload == nullptr)) { return LS_WRONG_BASIS_TYPE; }
    for (auto i = uint64_t{0}; i < count; ++i) {
        out[i] = static_cast<uint8_t>(is_representative_64(basis->header, *payload, bits[i]));
    }
    return LS_SUCCESS;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_get_states(ls_states**          ptr,
                                                                 ls_spin_basis const* basis)
{
    auto const* small_basis = std::get_if<small_basis_t>(&basis->payload);
    if (LATTICE_SYMMETRIES_UNLIKELY(small_basis == nullptr)) { return LS_WRONG_BASIS_TYPE; }
    if (LATTICE_SYMMETRIES_UNLIKELY(small_basis->cache == nullptr)) { return LS_CACHE_NOT_BUILT; }
    auto const states = small_basis->cache->states();
    auto       p      = std::make_unique<ls_states>(states, basis);
    *ptr              = p.release();
    return LS_SUCCESS;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT void ls_destroy_states(ls_states* states)
{
    std::default_delete<ls_states>{}(states);
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT uint64_t const* ls_states_get_data(ls_states const* states)
{
    return states->payload.data();
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT uint64_t ls_states_get_size(ls_states const* states)
{
    return states->payload.size();
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_save_cache(ls_spin_basis const* basis,
                                                                 char const*          filename)
{
    auto const* small_basis = std::get_if<small_basis_t>(&basis->payload);
    if (small_basis == nullptr) { return LS_WRONG_BASIS_TYPE; }
    if (small_basis->cache == nullptr) { return LS_CACHE_NOT_BUILT; }
    auto const states = small_basis->cache->states();
    auto const r      = save_states(states, filename);
    if (!r) {
        if (r.error().category() == get_error_category()) {
            return static_cast<ls_error_code>(r.error().value());
        }
        return LS_SYSTEM_ERROR;
    }
    return LS_SUCCESS;
}

// cppcheck-suppress unusedFunction
extern "C" LATTICE_SYMMETRIES_EXPORT ls_error_code ls_load_cache(ls_spin_basis* basis,
                                                                 char const*    filename)
{
    auto* p = std::get_if<small_basis_t>(&basis->payload);
    if (p == nullptr) { return LS_WRONG_BASIS_TYPE; }
    // Cache already built
    if (p->cache != nullptr) { return LS_SUCCESS; }

    auto&& r = load_states(filename);
    if (!r) {
        if (r.error().category() == get_error_category()) {
            return static_cast<ls_error_code>(r.error().value());
        }
        return LS_SYSTEM_ERROR;
    }
    p->cache = std::make_unique<basis_cache_t>(basis->header, *p, r.value());
    return LS_SUCCESS;
}

namespace lattice_symmetries {
auto is_real(ls_spin_basis const& basis) noexcept -> bool
{
    struct visitor_fn_t {
        auto operator()(small_basis_t const& x) const noexcept -> bool
        {
            auto const batched =
                std::all_of(std::begin(x.batched_symmetries), std::end(x.batched_symmetries),
                            [](auto const& s) { return is_real(s); });
            auto const other = x.other_symmetries.has_value() ? is_real(*x.other_symmetries) : true;
            return batched && other;
        }

        auto operator()(big_basis_t const& x) const noexcept -> bool
        {
            return std::all_of(std::begin(x.symmetries), std::end(x.symmetries),
                               [](auto const& s) { return is_real(s); });
        }
    };
    return std::visit(visitor_fn_t{}, basis.payload);
}
} // namespace lattice_symmetries
