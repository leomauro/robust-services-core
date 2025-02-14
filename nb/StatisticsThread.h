//==============================================================================
//
//  StatisticsThread.h
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
#ifndef STATISTICSTHREAD_H_INCLUDED
#define STATISTICSTHREAD_H_INCLUDED

#include "Thread.h"
#include <cstddef>
#include "Duration.h"
#include "NbTypes.h"
#include "TimePoint.h"

//------------------------------------------------------------------------------

namespace NodeBase
{
//  Periodically generates a statistics report and performs rollovers on
//  statistics registers.
//
class StatisticsThread : public Thread
{
   friend class Singleton< StatisticsThread >;
public:
   //  Overridden to display member variables.
   //
   void Display(std::ostream& stream,
      const std::string& prefix, const Flags& options) const override;

   //  Overridden for patching.
   //
   void Patch(sel_t selector, void* arguments) override;
private:
   //> The number of seconds between statistics reports and the rollover
   //  of statistics (default = 15 minutes).
   //
   static secs_t LongIntervalSecs;

   //> The number of seconds between the rollover of the short interval
   //  for thread statistics (default = 5 seconds).
   //
   static secs_t ShortIntervalSecs;

   //  The number of wakeups between statistics reports, which is equal
   //  to SecondsInStatsInterval / SecondsInThreadInterval.  The thread
   //  wakes up frequently to roll over thread statistics, but does so
   //  for other statistics every SecondsInStatsInterval.
   //
   static size_t WakeupsBetweenReports;

   //> The interval between the times when the thread starts to run.
   //
   static Duration SleepInterval;

   //  Private because this is a singleton.
   //
   StatisticsThread();

   //  Private because this is a singleton.
   //
   ~StatisticsThread();

   //  Calculates how long the thread will sleep when it is initially
   //  entered, and also initializes countdown_;
   //
   Duration CalcFirstDelay();

   //  Overridden to return a name for the thread.
   //
   c_string AbbrName() const override;

   //  Overridden to delete the singleton.
   //
   void Destroy() override;

   //  Overridden to enter a loop that generates statistics reports and
   //  performs rollovers on statistics registers.
   //
   void Enter() override;

   //  Overridden to generate a statistics report during cold restarts.
   //
   bool ExitOnRestart(RestartLevel level) const override;

   //  The next time at which the thread wants to run.
   //
   TimePoint wakeupTime_;

   //  A counter that causes a report to be generated when it reaches zero.
   //
   size_t countdown_;

   //  Set when a report could not be generated and will be reattempted.
   //
   bool delayed_;
};
}
#endif
