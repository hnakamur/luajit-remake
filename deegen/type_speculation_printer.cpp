#include "tvalue.h"

#include <sstream>

namespace
{

template<typename T>
struct get_type_speculation_defs;

template<typename... Args>
struct get_type_speculation_defs<std::tuple<Args...>>
{
    static constexpr size_t count = sizeof...(Args);

    template<typename T>
    static constexpr std::array<std::pair<TypeSpeculationMask, std::string_view>, 1> get_one()
    {
        return std::array<std::pair<TypeSpeculationMask, std::string_view>, 1> { std::make_pair(x_typeSpeculationMaskFor<T>, __stringify_type__<T>()) };
    }

    template<typename First, typename... Remaining>
    static constexpr auto get_impl()
    {
        if constexpr(sizeof...(Remaining) == 0)
        {
            return get_one<First>();
        }
        else
        {
            return constexpr_std_array_concat(get_one<First>(), get_impl<Remaining...>());
        }
    }

    static constexpr auto get()
    {
        return get_impl<Args...>();
    }

    static constexpr std::array<std::pair<TypeSpeculationMask, std::string_view>, count> value = get();

    static constexpr auto get_sorted()
    {
        using ElementT = std::pair<TypeSpeculationMask, std::string_view>;
        auto arr = value;
        std::sort(arr.begin(), arr.end(), [](const ElementT& lhs, const ElementT& rhs) -> bool {
            if (lhs.first != rhs.first)
            {
                return lhs.first > rhs.first;
            }
            return lhs.second < rhs.second;
        });
        return arr;
    }

    static constexpr std::array<std::pair<TypeSpeculationMask, std::string_view>, count> sorted_value = get_sorted();
};

}   // anonymous namespace

std::string DumpHumanReadableTypeSpeculationDefinitions()
{
    using ElementT = std::pair<TypeSpeculationMask, std::string_view>;
    constexpr size_t n = get_type_speculation_defs<TypeSpecializationList>::count;
    std::array<ElementT, n> arr = get_type_speculation_defs<TypeSpecializationList>::value;

    std::sort(arr.begin(), arr.end(), [](const ElementT& lhs, const ElementT& rhs) -> bool {
        int lhsOnes = std::popcount(lhs.first);
        int rhsOnes = std::popcount(rhs.first);
        if (lhsOnes != rhsOnes)
        {
            return lhsOnes < rhsOnes;
        }
        if (lhs.first != rhs.first)
        {
            return lhs.first < rhs.first;
        }
        return lhs.second < rhs.second;
    });

    std::string_view* names[n];
    for (size_t i = 0; i < n; i++) { names[i] = nullptr; }

    int maxk = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (std::popcount(arr[i].first) == 1)
        {
            int k = std::countr_zero(arr[i].first);
            assert(0 <= k && k < static_cast<int>(n));
            assert(names[k] == nullptr);
            names[k] = &arr[i].second;
            maxk = std::max(maxk, k + 1);
        }
    }
    for (int i = 0; i < maxk; i++)
    {
        assert(names[i] != nullptr);
    }

    std::stringstream ss;
    ss << "== Type Speculation Mask Defintions ==" << std::endl << std::endl;
    ss << "Defintion for each bit:" << std::endl;
    for (size_t i = 0; i < n; i++)
    {
        if (arr[i].first == 0)
        {
            // skip tBottom
            //
            continue;
        }
        int numOnes = std::popcount(arr[i].first);
        if (numOnes == 1)
        {
            ss << "Bit " << std::countr_zero(arr[i].first) << ": " << arr[i].second << " (0x" << std::hex << arr[i].first << std::dec << ")" << std::endl;
        }
        else
        {
            if (i > 0 && std::popcount(arr[i - 1].first) == 1)
            {
                ss << std::endl << "Compound Mask Definitions:" << std::endl;
            }
            ss << arr[i].second << " (0x" << std::hex << arr[i].first << std::dec << "): ";

            bool first = true;
            for (int k = 0; k < maxk; k++)
            {
                if (arr[i].first & (static_cast<TypeSpeculationMask>(1) << k))
                {
                    if (!first)
                    {
                        ss << " | ";
                    }
                    first = false;
                    ss << *names[k];
                }
            }
            ss << std::endl;
        }
    }
    std::string result = ss.str();
    return result;
}

std::string WARN_UNUSED DumpHumanReadableTypeSpeculation(TypeSpeculationMask mask)
{
    using ElementT = std::pair<TypeSpeculationMask, std::string_view>;
    constexpr size_t n = get_type_speculation_defs<TypeSpecializationList>::count;
    constexpr std::array<ElementT, n> arr = get_type_speculation_defs<TypeSpecializationList>::sorted_value;

    std::stringstream ss;
    if (mask == 0)
    {
        ss << "tBottom (0x0)";
    }
    else
    {
        TypeSpeculationMask originalVal = mask;
        bool first = true;
        for (size_t i = 0; i < n; i++)
        {
            TypeSpeculationMask curMask = arr[i].first;
            if (curMask == 0) { continue; }
            if ((mask & curMask) == curMask)
            {
                if (!first)
                {
                    ss << " | ";
                }
                first = false;
                ss << arr[i].second;
                mask ^= curMask;
            }
        }
        ReleaseAssert(mask == 0);
        ss << " (0x" << std::hex << originalVal << std::dec << ")";
    }
    std::string result = ss.str();
    return result;
}
