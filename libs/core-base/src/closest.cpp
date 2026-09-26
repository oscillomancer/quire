#include <supple/base/closest.h>
#include <algorithm>
#include <cctype>
#include <vector>
namespace supple {
std::string closestMatch(std::string_view input, std::span<const std::string_view> choices) {
    auto lower = [](std::string_view s) {
        std::string r(s);
        for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    };
    auto a = lower(input);
    int best = 100000;
    std::string out;
    for (auto choice : choices) {
        auto b = lower(choice);
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1), prevprev(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
        for (std::size_t i = 1; i <= a.size(); ++i) {
            cur[0] = static_cast<int>(i);
            for (std::size_t j = 1; j <= b.size(); ++j) {
                cur[j] = std::min(
                    {prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
                if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                    cur[j] = std::min(cur[j], prevprev[j - 2] + 1);
            }
            prevprev.swap(prev);
            prev.swap(cur);
        }
        int d = prev[b.size()];
        bool prefix = a.starts_with(b) || b.starts_with(a);
        if ((d <= std::max(2, static_cast<int>(a.size() / 3)) || prefix) && d < best) {
            best = d;
            out = std::string(choice);
        }
    }
    return out;
}
}  // namespace supple
