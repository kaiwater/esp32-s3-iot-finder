#pragma once

#include <stdint.h>

/* Returns a vendor name from the generated IEEE OUI table, or NULL when the
 * address is locally administered or unknown. */
const char *oui_lookup(const uint8_t mac[6]);
