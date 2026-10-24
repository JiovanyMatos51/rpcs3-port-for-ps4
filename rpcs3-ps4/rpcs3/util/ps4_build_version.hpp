#pragma once

// Packaging verifies this marker against -Version before producing a PKG.
inline constexpr char ps4_build_version[] = "0.40";

// Release date of this build (YYYY-MM-DD). A console clock earlier than this date (minus one day) cannot be right, so
// offline checks report a wrong clock instead of a network failure. It must never be later than the day the package
// is built: a later date would call a correct clock wrong.
inline constexpr char ps4_build_date[] = "2026-10-23";

namespace ps4_build_detail
{
    constexpr bool is_iso_date(const char (&s)[11]) noexcept
    {
        for (int i = 0; i < 10; i++)
        {
            const bool dash = i == 4 || i == 7;
            if (dash ? s[i] != '-' : (s[i] < '0' || s[i] > '9'))
                return false;
        }
        const int month = (s[5] - '0') * 10 + (s[6] - '0');
        const int day = (s[8] - '0') * 10 + (s[9] - '0');
        return s[10] == '\0' && month >= 1 && month <= 12 && day >= 1 && day <= 31;
    }
}

static_assert(ps4_build_detail::is_iso_date(ps4_build_date), "ps4_build_date must be YYYY-MM-DD");
