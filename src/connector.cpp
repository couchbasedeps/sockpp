// stream_connector.cpp
//
// --------------------------------------------------------------------------
// This file is part of the "sockpp" C++ socket library.
//
// Copyright (c) 2014-2017 Frank Pagliughi
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// --------------------------------------------------------------------------

#include "sockpp/connector.h"
#include "sockpp/exception.h"
#include <cerrno>
#ifndef WIN32
#include <sys/poll.h>
#endif
#ifdef __APPLE__
#include <net/if.h>
#endif

namespace sockpp {

#ifdef _WIN32
    // Winsock calls return non-POSIX error codes
    #define ERR_IN_PROGRESS WSAEINPROGRESS
    #define ERR_TIMED_OUT   WSAETIMEDOUT
	#define ERR_WOULD_BLOCK WSAEWOULDBLOCK
#else
    #define ERR_IN_PROGRESS EINPROGRESS
    #define ERR_TIMED_OUT   ETIMEDOUT
	#define ERR_WOULD_BLOCK EWOULDBLOCK
#endif

/////////////////////////////////////////////////////////////////////////////

bool connector::recreate(const sock_address& addr)
{
    sa_family_t domain = addr.family();
    socket_t h = create_handle(domain);

    if (!check_socket_bool(h))
        return false;

    // This will close the old connection, if any.
    reset(h);
    return true;
}


/////////////////////////////////////////////////////////////////////////////

bool connector::connect(const sock_address& addr, std::optional<Interface> inf)
{
	if (!recreate(addr))
		return false;
    
    if (inf && !set_network_interface(*inf))
        return false;

	if (!check_ret_bool(::connect(handle(), addr.sockaddr_ptr(), addr.size())))
		return close_on_err();

	return true;
}

/////////////////////////////////////////////////////////////////////////////

bool connector::connect(const sock_address& addr, std::chrono::microseconds timeout, std::optional<Interface> inf)
{
    if (timeout.count() <= 0)
        return connect(addr, inf);

    if (!recreate(addr))
        return false;
    
    if (inf && !set_network_interface(*inf))
        return false;

    set_non_blocking(true);
    if (!check_ret_bool(::connect(handle(), addr.sockaddr_ptr(), addr.size()))) {
        if (last_error() == ERR_IN_PROGRESS || last_error() == ERR_WOULD_BLOCK) {
#ifdef WIN32
            // Non-blocking connect -- call `select` to wait until the timeout:
        	// Note:  Windows returns errors in exceptset so check it too, the
        	// logic afterwords doesn't change
            fd_set readset;
            FD_ZERO(&readset);
            FD_SET(handle(), &readset);
            fd_set writeset = readset;
        	fd_set exceptset = readset;
            timeval tv = to_timeval(timeout);
            int n = check_ret(::select(handle()+1, &readset, &writeset, &exceptset, &tv));
#else
            pollfd handle_ = { handle(), POLLIN|POLLOUT, 0 };
            auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
            int n = check_ret(::poll(&handle_, 1, (int)timeoutMs.count()));
#endif

            if (n > 0) {
                // Got a socket event, but it might be an error, so check:
                int err;
                if (get_option(SOL_SOCKET, SO_ERROR, &err))
                    clear(err);
            } else if (n == 0) {
                clear(ERR_TIMED_OUT);
            }
        }

        if (last_error() != 0) {
            close();
            return false;
        }
    }

    set_non_blocking(false);
	return true;
}

bool connector::set_network_interface(const Interface& inf)
{
    auto addrFamily = family();
    
    // For AF_UNSPEC, assume IPv4:
    if (addrFamily == AF_UNSPEC)
        addrFamily = AF_INET;
    
    if ((addrFamily != AF_INET && addrFamily != AF_INET6) || addrFamily != inf.family())
        throw sys_error(EAFNOSUPPORT);
    
#if defined(__APPLE__)
    auto index = if_nametoindex(inf.name().c_str());
    if (index == 0) {
        set_last_error();
        return false;
    }
    if (addrFamily == AF_INET)
        return set_option(IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index));
    else
        return set_option(IPPROTO_IPV6, IPV6_BOUND_IF, &index, sizeof(index));
#elif defined(_WIN32)
    if (addrFamily == AF_INET)
        return set_option(IPPROTO_IP, IP_UNICAST_IF, &(inf.addr4()), sizeof(inf.addr4()));
    else
        return set_option(IPPROTO_IPV6, IPV6_UNICAST_IF, &(inf.addr6()), sizeof(inf.addr6()));
#elif defined(__linux__)
    return set_option(SOL_SOCKET, SO_BINDTODEVICE, inf.name().c_str(), inf.name().size());
#else
    throw sys_error(ENOTSUP);
#endif
}

/////////////////////////////////////////////////////////////////////////////
// end namespace sockpp
}

