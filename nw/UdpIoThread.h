//==============================================================================
//
//  UdpIoThread.h
//
//  Copyright (C) 2017  Greg Utas
//
//  This file is part of the Robust Services Core (RSC).
//
//  RSC is free software: you can redistribute it and/or modify it under the
//  terms of the GNU General Public License as published by the Free Software
//  Foundation, either version 3 of the License, or (at your option) any later
//  version.
//
//  RSC is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
//  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
//  details.
//
//  You should have received a copy of the GNU General Public License along
//  with RSC.  If not, see <http://www.gnu.org/licenses/>.
//
#ifndef UDPIOTHREAD_H_INCLUDED
#define UDPIOTHREAD_H_INCLUDED

#include "IoThread.h"
#include "NwTypes.h"
#include "SysTypes.h"

namespace NetworkBase
{
   class UdpIpService;
}

//------------------------------------------------------------------------------

namespace NetworkBase
{
//  I/O thread for UDP-based protocols.
//
class UdpIoThread : public IoThread
{
public:
   //  Creates an I/O thread that will receive UDP messages on
   //  behalf of SERVICE.
   //
   UdpIoThread(const UdpIpService* service, ipport_t port);

   //  Overridden for patching.
   //
   void Patch(sel_t selector, void* arguments) override;
protected:
   //  Protected to restrict deletion.
   //
   virtual ~UdpIoThread();

   //  Overridden to release resources in order to unblock.
   //
   void Unblock() override;

   //  Overridden to release resources during error recovery.
   //
   void Cleanup() override;
private:
   //  Overridden to return a name for the thread.
   //
   const char* AbbrName() const override;

   //  Overridden to receive UDP messages on PORT.
   //
   void Enter() override;

   //  Generates a log when an error forces the thread to exit.
   //
   void OutputLog(NodeBase::debug32_t errval) const;

   //  Releases resources when exiting or cleaning up the thread.
   //
   void ReleaseResources();
};
}
#endif
