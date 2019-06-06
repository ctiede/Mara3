/**
 ==============================================================================
 Copyright 2019, Jonathan Zrake

 Permission is hereby granted, free of charge, to any person obtaining a copy of
 this software and associated documentation files (the "Software"), to deal in
 the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.

 ==============================================================================
*/




#pragma once
#include <vector>
#include <thread>
#include "core_ndarray.hpp"




//=============================================================================
namespace mara
{
    template<std::size_t NumThreads>
    inline auto evaluate_on();

    template<std::size_t Rank>
    inline auto propose_block_decomposition(std::size_t number_of_subdomains);

    template<std::size_t Rank>
    inline auto create_access_pattern_array(nd::shape_t<Rank> global_shape, nd::shape_t<Rank> blocks_shape);

    template<std::size_t NumPartitions, std::size_t Rank>
    auto partition_shape(nd::shape_t<Rank> shape)
    {
        constexpr std::size_t distributed_axis = 0;
        auto result = sq::sequence_t<nd::access_pattern_t<Rank>, NumPartitions>();

        for (std::size_t n = 0; n < NumPartitions; ++n)
        {
            auto p = nd::make_access_pattern(shape);
            p.start[distributed_axis] = (n + 0) * shape[distributed_axis] / NumPartitions;
            p.final[distributed_axis] = (n + 1) * shape[distributed_axis] / NumPartitions;
            result[n] = p;
        }
        return result;
    }
}




//=============================================================================
namespace mara::parallel::detail
{
    inline std::pair<int, int> factor_once(int num);
    inline void prime_factors_impl(std::vector<int>& result, int num);
    inline std::vector<int> prime_factors(int num);
}




/**
 * @brief      Return a shared-memory nd::array evaluator for the given number
 *             of cores.
 *
 * @tparam     NumThreads  The number of cores to use
 *
 * @return     The evaluator
 *
 * @note       mara::evaluate_on<CoreCount>() is a drop-in replacement for
 *             nd::to_shared().
 */
template<std::size_t NumThreads>
auto mara::evaluate_on()
{
    return [] (auto array)
    {
        using value_type = typename decltype(array)::value_type;
        auto provider = nd::unique_provider_t<value_type, array.rank()>(array.shape());
        auto evaluate_partial = [&] (auto accessor)
        {
            return [accessor, array, &provider]
            {
                for (auto index : accessor)
                {
                    provider(index) = array(index);
                }
            };
        };
        auto threads = sq::sequence_t<std::thread, NumThreads>();
        auto regions = partition_shape<NumThreads>(array.shape());

        for (std::size_t i = 0; i < NumThreads; ++i)
            threads[i] = std::thread(evaluate_partial(regions[i]));

        for (auto& thread : threads)
            thread.join();

        return nd::make_array(std::move(provider).shared());
    };
}




/**
 * @brief      Return an nd::shape_t whose volume is equal to the given number
 *             of subdomains, and whose sizes on each axis are as similar as
 *             possible.
 *
 * @param[in]  number_of_subdomains  The number of subdomains
 *
 * @tparam     Rank                  The rank of the decomposed domain
 *
 * @return     The shape of the decomposed blocks
 */
template<std::size_t Rank>
auto mara::propose_block_decomposition(std::size_t number_of_subdomains)
{
    // auto mult = nd::transform([] (auto g) { return nd::accumulate(g, 1, std::multiplies<>()); });
    auto result = nd::shape_t<Rank>();
    // auto n = 0;

    throw;
    // for (auto dim : nd::divvy(Rank)(parallel::detail::prime_factors(number_of_subdomains)) | mult)
    // {
    //     result[n++] = dim;
    // }
    return result;
}




/**
 * @brief      Creates an N-dimensional array of N-dimensional
 *             nd::access_pattern_t instances, representing a block
 *             decomposition of an N-dimensional array.
 *
 * @param[in]  global_shape  The shape of the array to decompose
 * @param[in]  blocks_shape  The shape of the decomposed subgrid blocks
 *
 * @tparam     Rank          The dimensionality N
 *
 * @return     The array
 */
template<std::size_t Rank>
auto mara::create_access_pattern_array(nd::shape_t<Rank> global_shape, nd::shape_t<Rank> blocks_shape)
{
    sq::sequence_t<std::vector<std::size_t>, Rank> block_start_indexes;
    sq::sequence_t<std::vector<std::size_t>, Rank> block_sizes;

    for (auto axis : nd::range(Rank))
    {
        for (auto index_group : nd::range(global_shape[axis]) | nd::divvy(blocks_shape[axis]))
        {
            if (index_group.size() == 0)
            {
                throw std::logic_error("too many blocks for global domain size");
            }
            block_sizes        [axis].push_back( index_group.size());
            block_start_indexes[axis].push_back(*index_group.begin());
        }
    }

    auto mapping = [=] (auto index)
    {
        auto block = nd::access_pattern_t<Rank>();

        for (auto axis : nd::range(Rank))
        {
            block.start[axis] = block_start_indexes[axis][index[axis]];
            block.final[axis] = block.start[axis] + block_sizes[axis][index[axis]];
        }
        return block;
    };
    return nd::make_array(mapping, blocks_shape) | nd::bounds_check();
}




//=============================================================================
std::pair<int, int> mara::parallel::detail::factor_once(int num)
{
    for (int d = 2; ; ++d)
    {
        if (num % d == 0)
        {
            return {d, num / d};
        }
        if (d * d > num)
        {
            break;
        }
    }
    return {num, 1};
}

void mara::parallel::detail::prime_factors_impl(std::vector<int>& result, int num)
{
    auto once = factor_once(num);

    if (once.second == 1)
    {
        result.push_back(once.first);
    }
    else
    {
        prime_factors_impl(result, once.first);
        prime_factors_impl(result, once.second);
    }
}

std::vector<int> mara::parallel::detail::prime_factors(int num)
{
    std::vector<int> result;
    prime_factors_impl(result, num);
    return result;
}
