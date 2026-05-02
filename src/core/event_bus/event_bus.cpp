#include "core/event_bus/event_bus.h"

namespace launcher::core {
EventBus& EventBus::instance() { static EventBus s; return s; }
}
