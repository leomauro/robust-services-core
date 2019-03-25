//==============================================================================
//
//  Trigger.h
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
#ifndef TRIGGER_H_INCLUDED
#define TRIGGER_H_INCLUDED

#include "Protected.h"
#include <cstdint>
#include "Q1Way.h"
#include "SbTypes.h"
#include "SysTypes.h"

namespace NodeBase
{
   template< typename T > class Registry;
}

//------------------------------------------------------------------------------

namespace SessionBase
{
//  A Trigger allows an Initiator to observe a service's behavior and create a
//  ServiceSM associated with the Initiator to modify that service's behavior.
//  It is subclassed by a modifiable service.  Subclasses must be singletons.
//
//  A modifiable service may define a TriggerId without defining a concrete
//  Trigger subclass which registers against that identifier.  This may, in
//  fact, be a frequent occurrence.  Its purpose is to provide a TriggerId
//  to modifiers that have already been triggered, through their ProcessSap
//  and ProcessSnp functions.  These functions receive an event that supports
//  a TriggerId.  If a TriggerId is not provided, a modifier must analyze
//  the current state and event (for an SAP) and also the next state (for an
//  SNP) to determine what is occurring in its parent's state machine.  Such
//  analysis is eliminated by providing a TriggerId.  A Trigger only needs
//  to be registered against this TriggerId, however, if an Initiator needs
//  to *create* a modifier when the SAP or SNP occurs.
//
class Trigger : public NodeBase::Protected
{
   friend class Initiator;
   friend class NodeBase::Registry< Trigger >;
   friend class ServiceSM;
public:
   //  Allows "Id" to refer to a trigger identifier in this class hierarchy.
   //
   typedef TriggerId Id;

   //> Highest valid trigger identifier.
   //
   static const Id MaxId = INT8_MAX;

   //  Returns true if TID is a valid trigger identifier.
   //
   static bool IsValidId(Id tid)
   {
      return ((tid != NodeBase::NIL_ID) && (tid <= MaxId));
   }

   //  Returns the trigger's identifier.
   //
   Id Tid() const { return tid_; }

   //  Overridden to display member variables.
   //
   void Display(std::ostream& stream,
      const std::string& prefix, const NodeBase::Flags& options) const override;

   //  Overridden for patching.
   //
   void Patch(sel_t selector, void* arguments) override;
protected:
   //  Sets tid_.  Protected because this class is virtual.
   //
   explicit Trigger(Id tid);

   //  Deletes all initiators.  Protected because subclasses should
   //  be singletons.
   //
   virtual ~Trigger();
private:
   //  Adds INIT to the trigger's queue of initiators.  Invoked by
   //  Initiator's base class constructor.  Private to restrict usage.
   //
   bool BindInitiator(Initiator& init);

   //  Removes INIT from the trigger's queue of initiators.  Invoked
   //  by Initiator's base class destructor.  Private to restrict usage.
   //
   void UnbindInitiator(Initiator& init);

   //  Deleted to prohibit copying.
   //
   Trigger(const Trigger& that) = delete;
   Trigger& operator=(const Trigger& that) = delete;

   //  The identifier for this trigger.
   //
   Id tid_;

   //  The queue of initiators registered with this trigger.
   //
   NodeBase::Q1Way< Initiator > initq_;
};
}
#endif
