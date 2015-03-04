#include "sfp/system_error.hpp"

namespace sfp {

const char* ErrorCategory::name () const BOOST_NOEXCEPT {
    return "sfp";
}

std::string ErrorCategory::message (int ev) const BOOST_NOEXCEPT {
    switch (Status(ev)) {
#define ITEM(x) case Status::x: return #x;
        ITEM(OK)
        ITEM(HANDSHAKE_FAILED)
#undef ITEM
        default: return "(unknown status)";
    }
}

const boost::system::error_category& errorCategory () {
    static ErrorCategory instance;
    return instance;
}

boost::system::error_code make_error_code (Status status) {
    return boost::system::error_code(static_cast<int>(status),
        errorCategory());
}

boost::system::error_condition make_error_condition (Status status) {
    return boost::system::error_condition(static_cast<int>(status),
        errorCategory());
}

} // namespace sfp