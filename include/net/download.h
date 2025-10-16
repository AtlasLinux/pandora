#ifndef NET_DOWNLOAD_H
#define NET_DOWNLOAD_H

#include "util/err.h"

error_t fetch_package(const char* name, const char* version);

#endif