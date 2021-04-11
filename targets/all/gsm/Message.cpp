/*
 * Copyright (c) 2021 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/Message.cpp
 */

#include "Modem.h"

namespace gsm
{

void Message::Release()
{
    owner->ReleaseMessage(this);
}

async(Message::WaitUntilProcessed, Timeout timeout)
async_def_once()
{
    async_return(await_mask_timeout(flags, MessageFlags::ModemWillSend, 0, timeout));
}
async_end

}
