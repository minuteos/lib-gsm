/*
 * Copyright (c) 2020 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/Socket.h
 */

#pragma once

#include <kernel/kernel.h>

#include <collections/SelfLinkedList.h>
#include <io/Pipe.h>
#include <io/PipeReader.h>
#include <io/PipeWriter.h>

namespace gsm
{

enum struct SocketFlags : uint16_t
{
    //! TLS requested for socket
    AppSecure = 0x01,
    //! The application has requested the socket to be closed
    AppClose = 0x02,
    //! Socket has a reference from the application
    AppReference = 0x04,

    //! Check if data is incoming
    CheckIncoming = 0x10,

    //! The socket has a modem channel allocated
    ModemAllocated = 0x100,
    //! The request to connect the socket has been sent to the modem
    ModemConnecting = 0x200,
    //! The allocated socket channel is bound to a channel in the modem itself
    ModemReference = 0x400,
    //! Socket has been connected (not cleared after disconnect)
    ModemConnected = 0x800,
    //! Modem is currently sending a packet for this socket
    ModemSending = 0x1000,
    //! Modem has incoming data for this socket
    ModemIncoming = 0x2000,
    //! The request to close the socket has been sent to the modem
    ModemClosing = 0x4000,
    //! The socket processing has been finished in the modem
    ModemClosed = 0x8000,
};

DEFINE_FLAG_ENUM(SocketFlags);

class Socket
{
public:
    Socket(class Modem* owner, bool* txSignal)
        : owner(owner)
    {
        tx.BindSignal(txSignal);
    }

    async(Connect, Timeout timeout = Timeout::Infinite);
    async(Disconnect, Timeout timeout = Timeout::Infinite);
    void Release();

    bool IsConnected() const { return (flags & (SocketFlags::ModemConnected | SocketFlags::ModemClosed)) == SocketFlags::ModemConnected; }
    bool IsSecure() const { return !!(flags & SocketFlags::AppSecure); }
    bool IsClosed() const { return !!(flags & SocketFlags::ModemClosed); }

    io::PipeReader Input() { return rx; }
    io::PipeWriter Output() { return tx; }

private:
    Socket(const Socket& other) = delete; // prevent accidental copying

    Socket* next;
    class Modem* owner;
    io::Pipe rx, tx;
    SocketFlags flags;
    uint16_t port;
    const char* host;

    io::PipeReader OutputReader() { return tx; }
    io::PipeWriter InputWriter() { return rx; }

    bool IsNew() const
    {
        return (flags & ~SocketFlags::AppSecure)
            == SocketFlags::AppReference;
    }

    bool NeedsClose() const
    {
        return (flags & (SocketFlags::AppClose | SocketFlags::ModemReference | SocketFlags::ModemClosing))
            == (SocketFlags::AppClose | SocketFlags::ModemReference);
    }

    bool NeedsConnect() const
    {
        return (flags & (SocketFlags::AppClose | SocketFlags::AppReference | SocketFlags::ModemAllocated | SocketFlags::ModemReference | SocketFlags::ModemConnecting | SocketFlags::ModemClosing | SocketFlags::ModemClosed))
            == (SocketFlags::ModemAllocated | SocketFlags::AppReference);
    }

    bool DataToSend()
    {
        return IsConnected() && CanSend() && OutputReader().Available();
    }

    bool DataToReceive()
    {
        return !!(flags & SocketFlags::ModemIncoming);
    }

    bool DataToCheck()
    {
        return !!(flags & SocketFlags::CheckIncoming);
    }

    bool CanDelete() const
    {
        return !(flags & (SocketFlags::AppReference | SocketFlags::ModemReference));
    }

    bool CanSend() const
    {
        return (flags & (SocketFlags::ModemConnected | SocketFlags::ModemSending | SocketFlags::ModemClosing | SocketFlags::ModemClosed))
            == SocketFlags::ModemConnected;
    }

    bool IsSending() const
    {
        return !!(flags & SocketFlags::ModemSending);
    }

    bool CanReceive()
    {
        return (flags & (SocketFlags::ModemConnected | SocketFlags::ModemIncoming | SocketFlags::ModemClosing | SocketFlags::ModemClosed)) == (SocketFlags::ModemConnected | SocketFlags::ModemIncoming)
            && InputWriter().CanAllocate();
    }

    bool IsAllocated() const
    {
        return !!(flags & SocketFlags::ModemAllocated);
    }

    void Allocate()
    {
        ASSERT(!IsAllocated());
        flags |= SocketFlags::ModemAllocated;
    }

    void Bound()
    {
        ASSERT(IsAllocated());
        flags |= SocketFlags::ModemReference;
    }

    void Connected()
    {
        ASSERT(IsAllocated());
        flags = (flags & ~SocketFlags::ModemConnecting) | SocketFlags::ModemConnected;
    }

    void Incoming()
    {
        ASSERT(IsConnected());
        flags |= SocketFlags::ModemIncoming;
    }

    void MaybeIncoming()
    {
        ASSERT(IsConnected());
        flags |= SocketFlags::CheckIncoming;
    }

    void IncomingRequested()
    {
        ASSERT(IsConnected());
        flags &= ~(SocketFlags::ModemIncoming | SocketFlags::CheckIncoming);
    }

    void Sending()
    {
        ASSERT(IsConnected() && CanSend());
        flags |= SocketFlags::ModemSending;
    }

    void SendingFinished()
    {
        ASSERT(!CanSend() && IsSending());
        flags &= ~SocketFlags::ModemSending;
    }

    void Disconnected()
    {
        ASSERT(IsAllocated());
        Finished();
    }

    void Finished()
    {
        Output().Close();
        InputWriter().Close();
        flags = (flags & ~(SocketFlags::ModemConnecting | SocketFlags::ModemReference)) | SocketFlags::ModemConnected | SocketFlags::ModemClosed;
    }

    friend class Modem;
    friend class SimComModem;
    friend class SelfLinkedList<Socket>;
};

}
