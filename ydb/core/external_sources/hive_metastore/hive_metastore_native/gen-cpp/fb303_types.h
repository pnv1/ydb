/**
 * Autogenerated by Thrift Compiler (0.20.0)
 *
 * DO NOT EDIT UNLESS YOU ARE SURE THAT YOU KNOW WHAT YOU ARE DOING
 *  @generated
 */
#ifndef fb303_TYPES_H
#define fb303_TYPES_H

#include <iosfwd>

#include <contrib/restricted/thrift/thrift/Thrift.h>
#include <contrib/restricted/thrift/thrift/TApplicationException.h>
#include <contrib/restricted/thrift/thrift/TBase.h>
#include <contrib/restricted/thrift/thrift/protocol/TProtocol.h>
#include <contrib/restricted/thrift/thrift/transport/TTransport.h>

#include <functional>
#include <memory>


namespace facebook { namespace fb303 {

/**
 * Common status reporting mechanism across all services
 */
struct fb_status {
  enum type {
    DEAD = 0,
    STARTING = 1,
    ALIVE = 2,
    STOPPING = 3,
    STOPPED = 4,
    WARNING = 5
  };
};

extern const std::map<int, const char*> _fb_status_VALUES_TO_NAMES;

std::ostream& operator<<(std::ostream& out, const fb_status::type& val);

std::string to_string(const fb_status::type& val);

}} // namespace

#endif
