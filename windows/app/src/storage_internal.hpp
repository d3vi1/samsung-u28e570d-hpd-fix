#pragma once
#include "monitorfw/core.hpp"

namespace monitorfw { namespace detail {
// Pure identity key. Ports, EDID hashes and product codes may vary across inputs
// on one monitor, and therefore must not bypass a persistent recovery marker.
std::string recoveryKey(const MonitorIdentity& identity);
} }
