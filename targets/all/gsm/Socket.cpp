/*
 * Copyright (c) 2020 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/Socket.cpp
 */

#include "Socket.h"
#include "Modem.h"

namespace gsm
{

async(Socket::Connect, Timeout timeout)
async_def()
{
    await_mask_not_timeout(flags, SocketFlags::ModemConnected, 0, timeout);
    async_return(IsConnected());
}
async_end

async(Socket::Disconnect, Timeout timeout)
async_def()
{
    Output().Close();
    flags |= SocketFlags::AppClose;
    owner->RequestProcessing();
    async_return(await_mask_not_timeout(flags, SocketFlags::ModemClosed, 0, timeout));
}
async_end

void Socket::Release()
{
    owner->ReleaseSocket(this);
}

}
