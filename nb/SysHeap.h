//==============================================================================
//
//  SysHeap.h
//
//  Copyright (C) 2013-2021  Greg Utas
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
#ifndef SYSHEAP_H_INCLUDED
#define SYSHEAP_H_INCLUDED

#include "Heap.h"
#include <cstddef>
#include <iosfwd>
#include <set>
#include "SysDecls.h"
#include "SysTypes.h"

//------------------------------------------------------------------------------

namespace NodeBase
{
//  Operating system abstraction layer: heap.
//
class SysHeap : public Heap
{
public:
   //  Virtual to allow subclassing.
   //
   virtual ~SysHeap();

   //  Inserts, in HEAPS, the address of each heap allocated by this process.
   //  Updates EXPL with an explanation if a problem occurs.
   //
   static void ListHeaps(std::set< void* >& heaps, std::ostringstream& expl);

   //  Overridden to return the heap's address.
   //
   void* Addr() const override;

   //  Allocates SIZE bytes.
   //
   void* Alloc(size_t size) override;

   //  Returns the size of the block at ADDR.
   //
   size_t BlockToSize(const void* addr) const override;

   //  Returns true if the heap supports write-protection.
   //
   bool CanBeProtected() const override;

   //  Overridden to display member variables.
   //
   void Display(std::ostream& stream,
      const std::string& prefix, const Flags& options) const override;

   //  Frees the memory segment at ADDR.
   //
   void Free(void* addr) override;

   //  Overridden for patching.
   //
   void Patch(sel_t selector, void* arguments) override;

   //  Returns false on Windows, where VirtualProtect can fail if
   //  used on a heap.  Use NbHeap for a heap that requires write
   //  protection.
   //
   int SetPermissions(MemoryProtection attrs) override;

   //  Overridden to return the heap's size.
   //
   size_t Size() const override { return size_; }

   //  Overridden to return the type of memory that the heap manages.
   //
   MemoryType Type() const override { return type_; }

   //  Validates the heap.
   //
   bool Validate(const void* addr) const override;
protected:
   //  Wrapper for the default C++ heap.
   //
   SysHeap();

   //  Creates a heap for memory of TYPE.  If SIZE is 0, the heap's
   //  size can expand, else it is limited to SIZE bytes.  TYPE must
   //  not be MemPermanent.  Protected because this class is virtual.
   //
   SysHeap(MemoryType type, size_t size);
private:
   //  The native handle to the underlying heap.
   //
   SysHeap_t heap_;

   //  The heap's size.
   //
   const size_t size_;

   //  The type of memory that the heap manages.
   //
   const MemoryType type_;
};
}
#endif
