/// \file
/// Header for wrapping the logging system.

#ifndef __WRAP_LOG_H__
#define __WRAP_LOG_H__

#include "util/log.h"

namespace ila {
namespace pyapi {

void EnableLog(const std::string& tag);

void DisableLog(const std::string& tag);

void ClearLogs();

} // namespace pyapi
} // namespace

#endif // __WRAP_LOG_H__

