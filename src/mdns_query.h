#pragma once
extern "C" {
#include "mdnssd.h"
bool mdnssd_query_cancelable(mdnssd_handle_s* handle, const char* query, bool unicast,
    int runtime, mdns_callback_t* callback, void* cookie,
    bool (*cancelled)(const void*), const void* cancelCookie);
}
