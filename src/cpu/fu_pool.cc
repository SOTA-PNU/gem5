/*
 * Copyright (c) 2012-2013, 2025 Arm Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/fu_pool.hh"

#include <iostream>
#include <sstream>

#include "cpu/func_unit.hh"
#include "enums/OpClass.hh"

namespace gem5
{

namespace o3
{

////////////////////////////////////////////////////////////////////////////
//
//  A pool of function units
//

inline void
FUPool::FUIdxQueue::addFU(int fu_idx)
{
    funcUnitsIdx.push_back(fu_idx);
    ++size;
}

inline int
FUPool::FUIdxQueue::getFU()
{
    int retval = funcUnitsIdx[idx++];

    if (idx == size)
        idx = 0;

    return retval;
}

FUPool::~FUPool()
{
    for (FuncUnit* fu : funcUnits) {
        delete fu;
    }
}


// Constructor
FUPool::FUPool(const Params &p)
    : SimObject(p), statsRegistered(false)
{
    numFU = 0;

    funcUnits.clear();

    maxOpLatencies.fill(Cycles(0));
    pipelined.fill(true);

    //
    //  Iterate through the list of FUDescData structures
    //
    for (FUDesc *i : p.FUList) {
        //
        //  Don't bother with this if we're not going to create any FU's
        //
        if (i->number) {
            //
            //  Create the FuncUnit object from this structure
            //   - add the capabilities listed in the FU's operation
            //     description
            //
            //  We create the first unit, then duplicate it as needed
            //
            FuncUnit *fu = new FuncUnit;

            for (OpDesc *j : i->opDescList) {
                // indicate that this pool has this capability
                capabilityList.set(j->opClass);

                // Add each of the FU's that will have this capability to the
                // appropriate queue.
                for (int k = 0; k < i->number; ++k)
                    fuPerCapList[j->opClass].addFU(numFU + k);

                // indicate that this FU has the capability
                fu->addCapability(j->opClass, j->opLat, j->pipelined);

                if (j->opLat > maxOpLatencies[j->opClass])
                    maxOpLatencies[j->opClass] = j->opLat;

                if (!j->pipelined)
                    pipelined[j->opClass] = false;
            }

            numFU++;

            //  Add the appropriate number of copies of this FU to the list
            fu->name = i->name() + "(0)";
            funcUnits.push_back(fu);

            for (int c = 1; c < i->number; ++c) {
                std::ostringstream s;
                numFU++;
                FuncUnit *fu2 = new FuncUnit(*fu);

                s << i->name() << "(" << c << ")";
                fu2->name = s.str();
                funcUnits.push_back(fu2);
            }
        }
    }

    unitBusy.resize(numFU);

    for (int i = 0; i < numFU; i++) {
        unitBusy[i] = false;
    }

    // === Initialize Port Utilization Statistics ===
    fuUsageCount.resize(numFU, 0);
    fuBusyCycles.resize(numFU, 0);
    fuPendingBusyCycles.resize(numFU, 0);
    fuOpClassUsage.resize(numFU);
    fuOpClassCycles.resize(numFU);
    
    for (int i = 0; i < numFU; i++) {
        fuOpClassUsage[i].fill(0);
        fuOpClassCycles[i].fill(0);
    }
}

void
FUPool::syncStatsForPort(int fu_idx)
{
    if (!statsRegistered) {
        return;
    }

    portBusyCycles[fu_idx] = fuBusyCycles[fu_idx];
    portUsageCount[fu_idx] = fuUsageCount[fu_idx];
    for (int op = 0; op < Num_OpClasses; ++op) {
        portOpClassUsage[fu_idx][op] = fuOpClassUsage[fu_idx][op];
        portOpClassCycles[fu_idx][op] = fuOpClassCycles[fu_idx][op];
    }
}

void
FUPool::syncTotalBusyCycles()
{
    if (!statsRegistered) {
        return;
    }

    uint64_t total = 0;
    for (int i = 0; i < numFU; ++i) {
        total += fuBusyCycles[i];
    }
    totalPortBusyCycles = total;
}

void
FUPool::regStats()
{
    SimObject::regStats();

    portBusyCycles
        .init(numFU)
        .name(name() + ".portBusyCycles")
        .desc("Busy cycles per FU port");
    portUsageCount
        .init(numFU)
        .name(name() + ".portUsageCount")
        .desc("Usage count per FU port");
    portOpClassUsage
        .init(numFU, Num_OpClasses)
        .name(name() + ".portOpClassUsage")
        .desc("Per-port, per-opclass usage count");
    portOpClassCycles
        .init(numFU, Num_OpClasses)
        .name(name() + ".portOpClassCycles")
        .desc("Per-port, per-opclass busy cycles");
    totalPortBusyCycles
        .name(name() + ".totalPortBusyCycles")
        .desc("Sum of all port busy cycles");

    statsRegistered = true;
    for (int i = 0; i < numFU; ++i) {
        const std::string portLabel = "Port" + std::to_string(i);
        portBusyCycles.subname(i, portLabel);
        portUsageCount.subname(i, portLabel);
        portOpClassUsage.subname(i, portLabel);
        portOpClassCycles.subname(i, portLabel);
        syncStatsForPort(i);
    }
    portOpClassUsage.ysubnames(enums::OpClassStrings);
    portOpClassCycles.ysubnames(enums::OpClassStrings);

    syncTotalBusyCycles();
}

void
FUPool::preDumpStats()
{
    SimObject::preDumpStats();

    if (!statsRegistered) {
        return;
    }

    for (int i = 0; i < numFU; ++i) {
        syncStatsForPort(i);
    }
    syncTotalBusyCycles();
}

bool
FUPool::isCapable(OpClass capability)
{
    //  If this pool doesn't have the specified capability,
    //  return this information to the caller
    return capabilityList[capability];
}

int
FUPool::getUnit(OpClass capability)
{
    //  If this pool doesn't have the specified capability,
    //  return this information to the caller
    if (!capabilityList[capability])
        return NoCapableFU;

    int fu_idx = fuPerCapList[capability].getFU();
    int start_idx = fu_idx;

    // Iterate through the circular queue if needed, stopping if we've reached
    // the first element again.
    while (unitBusy[fu_idx]) {
        fu_idx = fuPerCapList[capability].getFU();
        if (fu_idx == start_idx) {
            // No FU available
            return NoFreeFU;
        }
    }

    assert(fu_idx < numFU);

    unitBusy[fu_idx] = true;
    
    // Record port usage statistics
    recordPortUsage(fu_idx, capability);

    return fu_idx;
}

void
FUPool::freeUnitNextCycle(int fu_idx)
{
    assert(unitBusy[fu_idx]);
    unitsToBeFreed.push_back(fu_idx);
}

void
FUPool::processFreeUnits()
{
    while (!unitsToBeFreed.empty()) {
        int fu_idx = unitsToBeFreed.back();
        unitsToBeFreed.pop_back();

        assert(unitBusy[fu_idx]);

        unitBusy[fu_idx] = false;
        
        // Record busy cycles
        recordPortFreed(fu_idx);
    }
}

void
FUPool::dump()
{
    std::cout << "Function Unit Pool (" << name() << ")\n";
    std::cout << "======================================\n";
    std::cout << "Free List:\n";

    for (int i = 0; i < numFU; ++i) {
        if (unitBusy[i]) {
            continue;
        }

        std::cout << "  [" << i << "] : ";

        std::cout << funcUnits[i]->name << " ";

        std::cout << "\n";
    }

    std::cout << "======================================\n";
    std::cout << "Busy List:\n";
    for (int i = 0; i < numFU; ++i) {
        if (!unitBusy[i]) {
            continue;
        }

        std::cout << "  [" << i << "] : ";

        std::cout << funcUnits[i]->name << " ";

        std::cout << "\n";
    }
}

void
FUPool::recordPortUsage(int fu_idx, OpClass capability)
{
    assert(fu_idx >= 0 && fu_idx < numFU);

    fuUsageCount[fu_idx]++;
    fuOpClassUsage[fu_idx][capability]++;

    uint64_t busyCycles = 0;
    if (pipelined[capability]) {
        busyCycles = 1;
    } else {
        busyCycles = static_cast<uint64_t>(maxOpLatencies[capability]);
        if (busyCycles == 0) {
            busyCycles = 1;
        }
    }

    fuPendingBusyCycles[fu_idx] += busyCycles;
    fuOpClassCycles[fu_idx][capability] += busyCycles;
}

void
FUPool::recordPortFreed(int fu_idx)
{
    assert(fu_idx >= 0 && fu_idx < numFU);
    fuBusyCycles[fu_idx] += fuPendingBusyCycles[fu_idx];
    fuPendingBusyCycles[fu_idx] = 0;
}

bool
FUPool::isDrained() const
{
    bool is_drained = true;
    for (int i = 0; i < numFU; i++)
        is_drained = is_drained && !unitBusy[i];

    return is_drained;
}

} // namespace o3
} // namespace gem5
