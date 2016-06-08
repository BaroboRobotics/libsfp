#ifndef SFP_SYSTEM_ERROR_HPP
#define SFP_SYSTEM_ERROR_HPP

#include <sfp/status.hpp>

#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <string>

namespace sfp {

class ErrorCategory : public boost::system::error_category {
public:
    virtual const char* name () const BOOST_NOEXCEPT override;
    virtual std::string message (int ev) const BOOST_NOEXCEPT override;
};

const boost::system::error_category& errorCategory ();
boost::system::error_code make_error_code (Status status);
boost::system::error_condition make_error_condition (Status status);

} // namespace sfp

namespace boost {
namespace system {

template <>
struct is_error_code_enum< ::sfp::Status> : public std::true_type { };

} // namespace system
} // namespace boost

#endif
