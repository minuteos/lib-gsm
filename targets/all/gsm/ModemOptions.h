/*
 * Copyright (c) 2020 triaxis s.r.o.
 * Licensed under the MIT license. See LICENSE.txt file in the repository root
 * for full license information.
 *
 * gsm/ModemOptions.h
 */

#pragma once

#include <base/base.h>

namespace gsm
{

class ModemOptions
{
public:
    virtual Span GetApn() { return Span(); }
    virtual Span GetApnUser() { return Span(); }
    virtual Span GetApnPassword() { return Span(); }
    virtual Span GetPin() { return Span(); }
    virtual void OnPinUsed() { }
    virtual void OnPowerOn() { }
    virtual void OnPowerOff() { }
    virtual bool RemovePin() { return true; }
    virtual bool UseFlowControl() { return true; }

    enum struct CallbackType
    {
        CommandSend,
        CommandReceive,
        CommandError,
        PowerSend,
        PowerReceive,
    };

    enum struct Parity
    {
        Off, Odd, Even, Mark, Space
    };

    // use even parity as default, to catch low spikes on the line
    // which get treated as valid 0xFF bytes without parity
    virtual Parity UseParity() { return Parity::Even; }

    virtual RES_PAIR_DECL(GetDiagnosticBuffer, CallbackType type) { return Buffer(); }
    ALWAYS_INLINE void DiagnosticCallback(CallbackType type, Buffer data) { DiagnosticCallback(type, data.Pointer(), data.Length()); }
    virtual void DiagnosticCallback(CallbackType type, char* data, size_t length) { }
};

}
