/*
 * Copyright (c) 2021 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/Message.h
 */

#pragma once

#include <kernel/kernel.h>

#include <collections/SelfLinkedList.h>

namespace gsm
{


enum struct MessageFlags : uint8_t
{
    //! Message has a reference from the application
    AppReference = 0x01,

    //! Message is yet to be sent by modem
    ModemWillSend = 0x10,
    //! Message is being sent by the modem
    ModemSending = 0x20,
    //! Message sending has failed
    ModemSendFailed = 0x80,
};

DEFINE_FLAG_ENUM(MessageFlags);

class Modem;

class Message
{
public:
    Span Recipient() const { return Span(this + 1, lenRcpt); }
    Span Text() const { return Span((const char*)(this + 1) + lenRcpt, lenTxt); }

    bool Sent() const { return !!(flags & (MessageFlags::ModemWillSend | MessageFlags::ModemSendFailed)); }
    int MessageReference() const { return mr; }

    async(WaitUntilProcessed, Timeout timeout);

    void Release();

private:
    Message(Modem* owner) : owner(owner) {}

    Message* next;
    Modem* owner;
    const char* data;
    MessageFlags flags;
    uint8_t lenRcpt;
    uint16_t lenTxt;
    int mr = -1;

    bool ShouldSend() const
    {
        return !!(flags & MessageFlags::ModemWillSend);
    }

    bool IsSending() const
    {
        return !!(flags & MessageFlags::ModemSending);
    }

    bool CanDelete() const
    {
        return !(flags & (MessageFlags::AppReference | MessageFlags::ModemWillSend));
    }

    void Sending()
    {
        flags += MessageFlags::ModemSending;
    }

    void SendingComplete(int mr)
    {
        this->mr = mr;
        flags = flags - MessageFlags::ModemWillSend - MessageFlags::ModemSending;
    }

    void SendingFailed()
    {
        flags = flags - MessageFlags::ModemWillSend - MessageFlags::ModemSending + MessageFlags::ModemSendFailed;
    }

    friend class SelfLinkedList<Message>;
    friend class Modem;
    friend class SimComModem;
};

}
