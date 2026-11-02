#include "oui.h"

#include "oui_table.h"

const char *oui_lookup(const uint8_t mac[6])
{
    if (mac[0] & 0x02) return 0; /* local/random address, no OUI */

    const uint32_t key = ((uint32_t)mac[0] << 16) |
                         ((uint32_t)mac[1] << 8) |
                         (uint32_t)mac[2];

    int lo = 0;
    int hi = (int)OUI_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = OUI_KEY[mid];
        if (v == key) return &OUI_NAMES[OUI_OFF[mid]];
        if (v < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}
