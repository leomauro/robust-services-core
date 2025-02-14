//==============================================================================
//
//  NwTypes.cpp
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
#include "NwTypes.h"
#include <ostream>
#include "SysTypes.h"

using std::ostream;
using namespace NodeBase;

//------------------------------------------------------------------------------

namespace NetworkBase
{
fixed_string ProtocolStrings[IpProtocol_N + 1] =
{
   "Any",
   "UDP",
   "TCP",
   ERROR_STR
};

ostream& operator<<(ostream& stream, IpProtocol proto)
{
   if((proto >= 0) && (proto < IpProtocol_N))
      stream << ProtocolStrings[proto];
   else
      stream << ProtocolStrings[IpProtocol_N];
   return stream;
}
}
