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
    virtual bool RemovePin() { return true; }
};

}
