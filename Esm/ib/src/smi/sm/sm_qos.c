/* BEGIN_ICS_COPYRIGHT7 ****************************************

Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

** END_ICS_COPYRIGHT7   ****************************************/

/* [ICS VERSION STRING: unknown] */

//======================================================================//
//                                                                      //
// FILE NAME                                                            //
//    sm_qos.c                                                          //
//                                                                      //
// DESCRIPTION                                                          //
//    This file contains SM QoS routines for setting up SL2VL mapping   //  
//    and VL Arbitration tables.                                        //
//                                                                      //
// FUNCTIONS                                                            //
//    sm_setup_SL2VL         Setup the SL2VL mapping tables             //
//    sm_initSwitch_SLVLMap   Program switch SL2VL mapping tables       //
//    sm_initialize_SLVLMap   Program SLVL mapping tables               //
//    sm_initialize_VLArbitration   Program VLArb tables                //
//                                                                      //
//======================================================================//

#include <stdlib.h>				/* for qsort */
#include "os_g.h"
#include "ib_status.h"
#include "sm_l.h"


/**
  Copy back the results from an aggregate response into the topology, performing byte swapping as necessary.
	@c aggr and @c end must denote a contiguous range of memory <tt>[aggr, end)<\tt>.

	@parma smaportp the SMA port (port 0 on a switch, port 1 on an HFI, typically).
	@return VSTATUS_BAD on unhandleable runtime errors, VSTATUS_OK otherwise, even if there's a segment with the error bit set.
*/
static Status_t
sm_node_handleGetRespAggr(Node_t * nodep, Port_t * smaportp, STL_AGGREGATE * aggr, STL_AGGREGATE * end);

/**
	Update SCVLt/SCVLnt for a node (usu. switch) from @c aggr.  Assumes aggr->Data is in network order.
*/
static Status_t
sm_aggregateToScvl(Node_t * nodep, STL_AGGREGATE * aggr);

/**
	Update curArb.vlarb* fields from @c agrr.  Assumes aggr->Data is in network order.
*/ 
static Status_t
sm_aggregateToVlarb(Node_t * nodep, STL_AGGREGATE * aggr);

/**
	Do LR Get(Aggregate) of attributes on @c nodep with values from SMA at @c smaportp.

	See @fn sm_node_updateFields() for interface details.

	@return VSTATUS_OK only if all operations succeed, non-OK otherwise.  Will return VSTATUS_BAD if an individual aggregate-segment operation failed.
*/
static Status_t
sm_node_updateFromSma_aggregate(IBhandle_t fd, uint16_t slid, Node_t * nodep, Port_t * smaportp);

/**
	Fallback code for SMAs that don't support aggregate operations.

	See @fn sm_node_updateFields() for interface details.
*/
static Status_t
sm_node_updateFromSma_solo(IBhandle_t fd, uint16_t slid, Node_t * nodep, Port_t * smaportp);

/**
	Update @c nodep SLSC, SCSL, SCVLt, and SCVLnt values from @c nodep found in @c srcTopop (usually topology from prior sweep).

	@return VSTATUS_OK on success (even if not updated), something else if things go really wrong.
*/
static Status_t
sm_node_updateFromTopo(Node_t * nodep, Topology_t * oldTopop, Topology_t * curTopop);

#define DO_INLINE_SET 0

/**
	Send all changes pending for all nodes in @c topop.  Continues sending even if updating one or more SMAs fails.  Releases changes for a node if update was sucessful.

	@param firstError [out,optional] If not NULL then on non-OK return val (except Node_t::index out-of range errors), @c *firstError will be the node that triggered the error.
*/
static Status_t
sm_syncSmaChanges(Topology_t * topop, Node_t ** firstError);

static Status_t
sm_node_syncSma_aggregate(Topology_t * topop, Node_t * nodep, Port_t * smaportp);

static Status_t
sm_node_syncSma_solo(Topology_t * topop, Node_t * nodep, Port_t * smaportp);

static void
setWeightMultiplier(Qos_t * qos);

// The following is for uniform qos
Qos_t sm_Qos[STL_MAX_VLS];

int sm_check_node_cache_valid(Node_t *);

static Status_t
sm_setSmaChanged(Topology_t * topop, Node_t * nodep, boolean changed)
{
	if (!topop->smaChanges) {
		if (topop->num_nodes == 0)
			return VSTATUS_BAD;
		if (vs_pool_alloc(&sm_pool, sizeof(bitset_t), (void**)&topop->smaChanges) != VSTATUS_OK)
			return VSTATUS_BAD;

		bitset_init(&sm_pool, topop->smaChanges, topop->num_nodes + 1);
	}

	if (changed)
		bitset_set(topop->smaChanges, nodep->index);
	else
		bitset_clear(topop->smaChanges, nodep->index);
	return VSTATUS_OK;
}

static Status_t
sm_markSmaChanged(Topology_t * topop, Node_t * nodep)
{
	return sm_setSmaChanged(topop, nodep, 1);
}

static Status_t
 sm_clearSmaChanged(Topology_t * topop, Node_t * nodep)
{
	return sm_setSmaChanged(topop, nodep, 0);
}

static void
sm_roundVLBandwidths(Qos_t * qos)
{
    int vl;
    // The following rounds the bandwidth to steps of 5
    for (vl= 0; vl < qos->numVLs; vl++) {
        if (qos->vlBandwidth[vl]) {
            if (qos->vlBandwidth[vl] % 5) {
                if (qos->vlBandwidth[vl] % 5 > 2) {
                    qos->vlBandwidth[vl] += (5 - (qos->vlBandwidth[vl] % 5));
                } else {
                    qos->vlBandwidth[vl] -= (qos->vlBandwidth[vl] % 5);
                    if (!qos->vlBandwidth[vl])
                        qos->vlBandwidth[vl] = 5;
                }
            }
        }
    }
	setWeightMultiplier(qos);
}


static void
sm_DbgPrintQOS(Qos_t * qos)
{
    if (sm_config.sm_debug_vf)
    {
        bitset_info_log(&qos->lowPriorityVLs, "lowPriorityVLs");
        bitset_info_log(&qos->highPriorityVLs, "highPriorityVLs");
        IB_LOG_INFINI_INFO_FMT( __func__,
                                "SCVL%2d = 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x", qos->numVLs,
                               qos->scvl.SCVLMap[0].VL, qos->scvl.SCVLMap[1].VL, qos->scvl.SCVLMap[2].VL,
                               qos->scvl.SCVLMap[3].VL, qos->scvl.SCVLMap[4].VL, qos->scvl.SCVLMap[5].VL,
                               qos->scvl.SCVLMap[6].VL, qos->scvl.SCVLMap[7].VL);

        IB_LOG_INFINI_INFO_FMT( __func__,
                                "         0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
                               qos->scvl.SCVLMap[8].VL, qos->scvl.SCVLMap[9].VL, qos->scvl.SCVLMap[10].VL,
                               qos->scvl.SCVLMap[11].VL, qos->scvl.SCVLMap[12].VL, qos->scvl.SCVLMap[13].VL,
                               qos->scvl.SCVLMap[14].VL, qos->scvl.SCVLMap[15].VL);

        IB_LOG_INFINI_INFO_FMT( __func__,
                                "         0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
                               qos->scvl.SCVLMap[16].VL, qos->scvl.SCVLMap[17].VL, qos->scvl.SCVLMap[18].VL,
                               qos->scvl.SCVLMap[19].VL, qos->scvl.SCVLMap[20].VL, qos->scvl.SCVLMap[21].VL,
                               qos->scvl.SCVLMap[22].VL, qos->scvl.SCVLMap[23].VL);

        IB_LOG_INFINI_INFO_FMT( __func__,
                                "         0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
                               qos->scvl.SCVLMap[24].VL, qos->scvl.SCVLMap[25].VL, qos->scvl.SCVLMap[26].VL,
                               qos->scvl.SCVLMap[27].VL, qos->scvl.SCVLMap[28].VL, qos->scvl.SCVLMap[29].VL,
                               qos->scvl.SCVLMap[30].VL, qos->scvl.SCVLMap[31].VL);

        int vl;
        int totalBw = 0;
        for (vl = bitset_find_first_one(&qos->lowPriorityVLs); vl > -1;
             vl = bitset_find_next_one(&qos->lowPriorityVLs, vl+ 1)) {
            IB_LOG_INFINI_INFO_FMT( __func__, "lowPriority VL%d has bandwidth %d", vl,
                                   qos->vlBandwidth[vl]);
            totalBw += qos->vlBandwidth[vl];
        }
        IB_LOG_INFINI_INFO_FMT( __func__, "lowPriority has total bandwidth %d", totalBw);
        totalBw = 0;
        for (vl = bitset_find_first_one(&qos->highPriorityVLs); vl > -1;
             vl = bitset_find_next_one(&qos->highPriorityVLs, vl+ 1)) {
            IB_LOG_INFINI_INFO_FMT( __func__, "highPriority VL%d has bandwidth %d", vl,
                                   qos->vlBandwidth[vl]);
            totalBw += qos->vlBandwidth[vl];
        }
        IB_LOG_INFINI_INFO_FMT( __func__, "highPriority has total bandwidth %d", totalBw);
    }
}


static void
scvlAssign(int startVL, int endVL, bitset_t * SCs, Qos_t * qos, int high, VirtualFabrics_t *VirtualFabrics)
{
	int		vl, sc;
	int		numVls = endVL-startVL;
    int		numSCs = SCs->nset_m;
    bitset_t * prioVL = (high==1) ? &qos->highPriorityVLs : &qos->lowPriorityVLs;

    if ( (numVls >= numSCs) || (numVls==1) ) {
        // 1::1 assignment, including 1 VL to 1 SC. 
        // OR all SCs globbed together on 1 VL (regardless of any other criteria)
        // Note: Revisit for DOR (all SCs assigned to 1 VL is bad for DOR)
        vl=startVL;
        for (sc = bitset_find_first_one(SCs); sc > -1; sc = bitset_find_next_one(SCs, sc + 1)) {
            qos->scvl.SCVLMap[sc].VL = vl;
            qos->vlBandwidth[vl] += sm_SlBandwidthAllocated[sm_SCtoSL[sc]]; 
            bitset_set(prioVL, vl);
            if(numVls!=1) vl++;
        }
    } else {
        // Number of SCs > VLs, and numVls > 1, need to oversubscribe.
        // We can group SCs (SLs/VFs) to VLs with "similar" preemption ranks.
        //   - any SC with zero rank (non-preemptible) can be grouped together
        //   - any SC with "adjacent", non-zero rank can be grouped together
        // Note: Revisit for DOR as this fn does not consider DOR routing pairs
        //       and will possibly contract DOR pairs to the same VL

        // To determine adjecency, sort the SLs by preemption rank.
        // Count the number of zero-rank SLs in the same loop.
        uint8_t pos[STL_MAX_SCS]; 
        memset(pos, 0xff, sizeof(pos));
        int numZeroSCs = 0, vf, i;
        for (i = 0; i < STL_MAX_SCS; i++) {
            int rank = 0xff;
            for (sc = bitset_find_first_one(SCs); sc > -1; sc = bitset_find_next_one(SCs, sc + 1)) {
                // Attempting to find the VF associated with this SC.
                // Generally, each VF will be assigned its own unique SL/SC.
                // HOWEVER, if QOS is disabled, then it is possible that multiple VFs
                //   can be assiged to the SAME SL/SC.
                // BUT - this is ok as the VF characteristics are the same
                //   (or similar enough) for the considerations here.
                vf=smGetSLVF(sm_SCtoSL[sc], VirtualFabrics);// sm_SCtoSL[sc]; 
                if ((vf!=-1) && (rank > VirtualFabrics->v_fabric[vf].preempt_rank)) {
                    rank = VirtualFabrics->v_fabric[vf].preempt_rank;
                    pos[i]=sc;
                    if (rank==0) numZeroSCs++;
                }
            }
            bitset_clear(SCs, pos[i]); // Remove lowest rank SC.
            if (SCs->nset_m==0) break; // no more SCs.
        }

        // Now that we know the number of rank-zero SCs, 
        //  determine the number of rank-zero VLs that we should use. 
        int numZeroVls = (numZeroSCs * numVls) / numSCs; 
        if ((numZeroVls==0) && (numZeroSCs>0)) numZeroVls++; 

        // Update the number of non-zero SCs / VLs 
        numSCs -= numZeroSCs;
        numVls -= numZeroVls;

        i=0; vl = startVL; sc = pos[0];

        // Allocate the zero rank SCs to VLs
        int SCsForThisVL;
        int SCsperVl = (numZeroVls ? numZeroSCs/numZeroVls : 0);
        int extraSCs = (numZeroVls ? numZeroSCs%numZeroVls : 0);
        while ((numZeroVls--) && (sc!=0xff)) {
			if (sc >= STL_MAX_SCS) {
				IB_LOG_ERROR_FMT(__func__, "Allocate zero rank: SC >= STL_MAX_SCS: sc:%d, i:%d",sc,i);
				break;
			}
			if (vl >= STL_MAX_VLS) {
				IB_LOG_ERROR_FMT(__func__, "Allocate zero rank: VL >= STL_MAX_VLS: vl:%d",vl);
				break;
			}
            bitset_set(prioVL, vl);
            SCsForThisVL = SCsperVl + ((extraSCs-- > 0) ? 1 : 0);
            while ((SCsForThisVL--) && (sc!=0xff)) {
                qos->scvl.SCVLMap[sc].VL = vl;
                qos->vlBandwidth[vl] += sm_SlBandwidthAllocated[sm_SCtoSL[sc]];
				i++;
				if (i >= STL_MAX_SCS) {
					IB_LOG_ERROR_FMT(__func__, "Allocate zero rank: index >= STL_MAX_SCS: i:%d",i);
					break;
				}
                sc=pos[i];
				if (sc >= STL_MAX_SCS) {
					IB_LOG_ERROR_FMT(__func__, "Allocate zero rank: SC >= STL_MAX_SCS: sc:%d, i:%d",sc,i);
					break;
				}
            }
            vl++;
        }

        // Allocate the non-zero rank SCs to VLs
        SCsperVl = (numVls ? numSCs/numVls : 0);
        extraSCs = (numVls ? numSCs%numVls : 0);
        while ((numVls--) && (sc!=0xff)) {
			if (sc >= STL_MAX_SCS) {
				IB_LOG_ERROR_FMT(__func__, "Allocat non-zero rank: SC >= STL_MAX_SCS: sc:%d, i:%d",sc,i);
				break;
			}
			if (vl >= STL_MAX_VLS) {
				IB_LOG_ERROR_FMT(__func__, "Allocate zero rank: VL >= STL_MAX_VLS: vl:%d",vl);
				break;
			}
            bitset_set(prioVL, vl);
            SCsForThisVL = SCsperVl + ((extraSCs-- > 0) ? 1 : 0);
            while ((SCsForThisVL--) && (sc!=0xff)){
                qos->scvl.SCVLMap[sc].VL = vl;
                qos->vlBandwidth[vl] += sm_SlBandwidthAllocated[sm_SCtoSL[sc]];
				i++;
				if (i >= STL_MAX_SCS) {
					IB_LOG_ERROR_FMT(__func__, "Allocate non-zero rank: index >= STL_MAX_SCS: i:%d",i);
					break;
				}
                sc=pos[i];
				if (sc >= STL_MAX_SCS) {
					IB_LOG_ERROR_FMT(__func__, "Allocate non-zero rank: SC >= STL_MAX_SCS: sc:%d, i:%d",sc,i);
					break;
				}
            }
            vl++;
        }
    }
}


static void
slscFixedAssign(int startVL, int endVL, bitset_t * SLs, Qos_t * qos, int high, VirtualFabrics_t *VirtualFabrics)
{
    int     vl, sl;
    int     numVls = endVL-startVL;
    int     numSLs = SLs->nset_m;
    int     scOffset;

    bitset_t * prioVL = (high==1) ? &qos->highPriorityVLs : &qos->lowPriorityVLs;

    if ( (numVls >= numSLs) || (numVls==1) ) {
        // 1::1 assignment, including 1 VL to 1 SL.
        // OR all SLs globbed together on 1 VL (regardless of any other criteria)
        vl=startVL;
        scOffset=0;
        for (sl = bitset_find_first_one(SLs); sl > -1; sl = bitset_find_next_one(SLs, sl + 1)) {
            if ((vl>=STL_MAX_VLS) || (sl>=STL_MAX_VLS) || (vl+scOffset>=STL_MAX_SCS)){
                IB_LOG_ERROR_FMT(__func__,
                    "1:1 Indices out of range: vl:%d, sl:%d, sc:%d", vl, sl, vl+scOffset);
                break;
            }

            sm_SLtoSC[sl] = vl+scOffset; // SC::VL is 1::1 in fixed maps!
            sm_SCtoSL[vl+scOffset] = sl;
			//For standby VFs, sm_main::sm_update_bw will set sm_SlBandwidthAllocated[sl] to 0
			//therefore, qos->vlBandwidth[vl] will only represent active VF bandwidths
			qos->vlBandwidth[vl] += sm_SlBandwidthAllocated[sl];

            bitset_set(prioVL, vl);

            if(numVls==1) scOffset += qos->numVLs;
            else vl++;

            if ((vl+scOffset) == 15) scOffset += qos->numVLs;
        }
    } else {
        // Number of SLs > VLs, and numVls > 1, need to oversubscribe.
        // We can group SLs (SLs/VFs) to VLs with "similar" preemption ranks.
        //   - any SL with zero rank (non-preemptible) can be grouped together
        //   - any SL with "adjacent", non-zero rank can be grouped together

        // To determine adjecency, sort the SLs by preemption rank.
        // And Count the number of zero-rank SLs in the same loop.
        uint8_t pos[STL_MAX_SLS];
        memset(pos, 0xff, sizeof(pos));
        int numZeroSLs = 0, vf, i;
        for (i = 0; i < STL_MAX_SLS; i++) {
            int rank = 0xff;
            for (sl = bitset_find_first_one(SLs); sl > -1; sl = bitset_find_next_one(SLs, sl + 1)) {
                // Find the VF associated with this SL.
                // Generally, each VF will be assigned its own unique SL.
                // HOWEVER, if QOS is disabled, then it is possible that multiple VFs
                //   can be assiged to the SAME SL.
                // BUT - this is ok as the VF characteristics are the same
                //   (or similar enough) for the considerations here.
                vf=smGetSLVF(sl, VirtualFabrics);
                if ((vf!=-1) && (rank > VirtualFabrics->v_fabric[vf].preempt_rank)) {
                    rank = VirtualFabrics->v_fabric[vf].preempt_rank;
                    pos[i]=sl;
                    if (rank==0) numZeroSLs++;
                }
            }
            bitset_clear(SLs, pos[i]); // Remove lowest rank SL.
            if (SLs->nset_m==0) break; // no more SLs.
        }

        // Now that we know the number of rank-zero SLs,
        //  determine the number of rank-zero VLs that we should use.
        int numZeroVls = (numZeroSLs * numVls) / numSLs;
        if ((numZeroVls==0) && (numZeroSLs>0)) numZeroVls++;

        // Update the number of non-zero SLs / VLs
        numSLs -= numZeroSLs;
        numVls -= numZeroVls;

        i=0; vl = startVL; sl = pos[0];

        // Allocate the zero rank SCs to VLs
        int SLsForThisVL;
        int SLsperVl = (numZeroVls ? numZeroSLs/numZeroVls : 0);
        int extraSLs = (numZeroVls ? numZeroSLs%numZeroVls : 0);
        while ((numZeroVls--) && (sl!=0xff)) {
            bitset_set(prioVL, vl);
            SLsForThisVL = SLsperVl + ((extraSLs-- > 0) ? 1 : 0);
            scOffset = 0;
            while ((SLsForThisVL--) && (sl!=0xff)) {
                if ((vl+scOffset) == 15) scOffset += qos->numVLs;
                if ((vl>=STL_MAX_VLS) || (sl>=STL_MAX_VLS) || (i>=STL_MAX_SLS) || (vl+scOffset>=STL_MAX_SCS)){
                    IB_LOG_ERROR_FMT(__func__,
                        "No preempt indices out of range: vl:%d, sl:%d, rank:%d, sc:%d", vl, sl, i, vl+scOffset);
                    break;
                }
                qos->vlBandwidth[vl] += sm_SlBandwidthAllocated[sl]; 
                sm_SLtoSC[sl] = vl+scOffset; // SC::VL is 1::1 in fixed maps!
                sm_SCtoSL[vl+scOffset] = sl;
				i++;
                if (i>=STL_MAX_SLS) {
                    IB_LOG_ERROR_FMT(__func__,
                        "No preempt indices out of range: vl:%d, sl:%d, rank:%d, sc:%d", vl, sl, i, vl+scOffset);
                    break;
                }
                sl = pos[i];
                scOffset += qos->numVLs;
            }
            vl++;
        }

        // Allocate the non-zero rank SLs to VLs
        SLsperVl = (numVls ? numSLs/numVls : 0);
        extraSLs = (numVls ? numSLs%numVls : 0);
        while ((numVls--) && (sl!=0xff)) {
            bitset_set(prioVL, vl);
            SLsForThisVL = SLsperVl + ((extraSLs-- > 0) ? 1 : 0);
            scOffset = 0;
            while ((SLsForThisVL--) && (sl!=0xff)){
                if ((vl+scOffset) == 15) scOffset += qos->numVLs;
                if ((vl>=STL_MAX_VLS) || (sl>=STL_MAX_VLS) || (i>=STL_MAX_SLS) || (vl+scOffset>=STL_MAX_SCS)){
                    IB_LOG_ERROR_FMT(__func__,
                        "Preempt indices out of range: vl:%d, sl:%d, rank:%d, sc:%d", vl, sl, i, vl+scOffset);
                    break;
                }
                qos->vlBandwidth[vl] += sm_SlBandwidthAllocated[sl];
                sm_SLtoSC[sl] = vl+scOffset; // SC::VL is 1::1 in fixed maps!
                sm_SCtoSL[vl+scOffset] = sl;

				i++;
                if (i>=STL_MAX_SLS) {
                    IB_LOG_ERROR_FMT(__func__,
                        "Preempt indices out of range: vl:%d, sl:%d, rank:%d, sc:%d", vl, sl, i, vl+scOffset);
                    break;
                }
                sl=pos[i];
                scOffset += qos->numVLs;
            }
            vl++;
        }
    }
}


static void
AllocateVL(uint8_t numVLs, Qos_t * qos, VirtualFabrics_t *VirtualFabrics) 
{
	//Dynamic reconfiguration won't work if this non fixed mapping is used; 
	//need to reassess changes that are necessary

    int sc, vf, i;
	int startVL=0;
    int minRqVl;

    bitset_t	scsInUse;
    bitset_t	highPrioritySCs;
    bitset_t	lowPrioritySCs;

	if (!bitset_init(&sm_pool, &qos->highPriorityVLs, STL_MAX_VLS) ||
		!bitset_init(&sm_pool, &qos->lowPriorityVLs, STL_MAX_VLS)  ||
	    !bitset_init(&sm_pool, &scsInUse, STL_MAX_SCS)             ||
        !bitset_init(&sm_pool, &highPrioritySCs, STL_MAX_SCS)      ||
		!bitset_init(&sm_pool, &lowPrioritySCs, STL_MAX_SCS)) {
		IB_FATAL_ERROR("AllocateVL: No memory for SCs setup, exiting.");
	}

	qos->numVLs = numVLs;

    for (vf=0; vf < VirtualFabrics->number_of_vfs; vf++) {
        sc=VirtualFabrics->v_fabric[vf].base_sc;
        for (i=0; i<VirtualFabrics->v_fabric[vf].routing_scs; i++, sc++) {
            if (sc==15) sc++; // skip the invalid SC
            bitset_set(&scsInUse, sc);
            if (VirtualFabrics->v_fabric[vf].priority==1) {
                bitset_set(&highPrioritySCs, sc);
            } else {
                bitset_set(&lowPrioritySCs, sc);
            }
        }
    }

	if (sm_config.sm_debug_vf) {
        bitset_info_log(&scsInUse, "scsInUse");
        bitset_info_log(&highPrioritySCs, "highPrioritySCs");
        bitset_info_log(&lowPrioritySCs, "lowPrioritySCs");
    }

    // Setup minimum required VL
    minRqVl=1;
    
    // If have both high and low, min required is doubled.
    if ((highPrioritySCs.nset_m!=0) &&
        (lowPrioritySCs.nset_m!=0)) {
        minRqVl*=2;
    }

    // With less than required VLs, cannot assign high and low prio VFs
    // to different VLs. Assign all to low.
	if (numVLs < minRqVl) {
        scvlAssign(startVL, numVLs, &scsInUse, qos, 0, VirtualFabrics);
        goto done;
	}

    // If we have low and no high, assign all VLs to low.
    if ((highPrioritySCs.nset_m==0) &&
        (lowPrioritySCs.nset_m!=0)) {
        scvlAssign(startVL, numVLs, &lowPrioritySCs, qos, 0, VirtualFabrics);
        goto done;
    }

    // If we have no low and high, assign all VLs to high.
    if ((highPrioritySCs.nset_m==0) &&
        (lowPrioritySCs.nset_m!=0)) {
        scvlAssign(startVL, numVLs, &highPrioritySCs, qos, 1, VirtualFabrics);
        goto done;
    }

    // We have a mix of high and low.  Assign Low reserving some VLs for high.
    scvlAssign(startVL, numVLs-(minRqVl/2), &lowPrioritySCs, qos, 0, VirtualFabrics);

    startVL = bitset_find_next_zero(&qos->lowPriorityVLs, startVL+1);

    scvlAssign(startVL, numVLs, &highPrioritySCs, qos, 1, VirtualFabrics);

done: 

	bitset_free(&scsInUse);        
	bitset_free(&highPrioritySCs); 
	bitset_free(&lowPrioritySCs);  

	sm_roundVLBandwidths(qos);
	sm_DbgPrintQOS(qos);
}


static void
AllocateSCsForFixedMap(uint8_t numVLs, Qos_t * qos, VirtualFabrics_t *VirtualFabrics)
{
    // Assign SCs to SLs for Fixed mapping.
    int sl, vl, vf;
    bitset_t usedSLs;
    bitset_t highPrioritySLs;
    bitset_t lowPrioritySLs;

    if (numVLs == 0) {
        // Logic error - cannot call with number of VLs = 0
        IB_FATAL_ERROR("AllocateSCsForFixedMap: Num VLs cannot be 0.");
    }

    if (!bitset_init(&sm_pool, &usedSLs, STL_MAX_SLS) ||
        !bitset_init(&sm_pool, &highPrioritySLs, STL_MAX_SLS) ||
        !bitset_init(&sm_pool, &lowPrioritySLs, STL_MAX_SLS)  ||
        !bitset_init(&sm_pool, &qos->highPriorityVLs, STL_MAX_VLS) ||
        !bitset_init(&sm_pool, &qos->lowPriorityVLs, STL_MAX_VLS)) {
        IB_FATAL_ERROR("AllocateSCsForFixedMap: No memory for QoS setup, exiting.");
    }

    qos->numVLs = numVLs;

    // Using the "FIXED MAP IMPLEMENTATION"
    // This means - FIXED SC::VL, FIXED number of VLs per port, FIXED for the cluster
    // No link that has less VLs than the "FIXED" number is active.

    // Assign SCs to SLs- Consider High / LOW priority, and preemption.
    for (vf=0; vf < VirtualFabrics->number_of_vfs_all; vf++) {

        // This algorithm does not support DOR
        if (VirtualFabrics->v_fabric_all[vf].routing_scs != 1) {
            IB_FATAL_ERROR("AllocateSCsForFixedMap: Routing SCs must be 1. exiting.");
        }

        // Create lists of SL assignments [all, high, low]
        sl = VirtualFabrics->v_fabric_all[vf].base_sl;
        bitset_set(&usedSLs, sl);
        if (VirtualFabrics->v_fabric_all[vf].priority==1) {
            bitset_set(&highPrioritySLs, sl);
        } else {
            bitset_set(&lowPrioritySLs, sl);
        }
    }

    if (sm_config.sm_debug_vf) {
        bitset_info_log(&usedSLs, "usedSLs");
        bitset_info_log(&highPrioritySLs, "highPrioritySLs");
        bitset_info_log(&lowPrioritySLs, "lowPrioritySLs");
    }

    // Check for oversubscription...
    // With fixed SC::VL map, a VL can only be oversubscribed 
    // by an SC so many times... Need to check.
    if ((highPrioritySLs.nset_m!=0) && (lowPrioritySLs.nset_m!=0) && (numVLs!=1)) {
        int numLPVLs = (lowPrioritySLs.nset_m < numVLs ? lowPrioritySLs.nset_m : numVLs -1);
        int numHPVLs = numVLs - numLPVLs;
        if ((highPrioritySLs.nset_m/numHPVLs) > ((STL_MAX_SCS-1)/numVLs)){
            IB_LOG_ERROR_FMT(__func__,
                "Oversubscribed VLs, too many HP/LP VFs. Reduce HP or LP VFs. HPVF:%d, LPVF:%d, VLs:%d",
                highPrioritySLs.nset_m, lowPrioritySLs.nset_m, numVLs);
            goto done;
        }
    }

    // If we have enough VLs to support all the SLs we need, its a simple 1::1 map.
    if (numVLs == 1) {
        slscFixedAssign(0, numVLs, &usedSLs, qos, 0, VirtualFabrics);
        goto done;
    }

    // If we have low and no high, assign all VLs to low.
    if ((highPrioritySLs.nset_m==0) && (lowPrioritySLs.nset_m!=0)) {
        slscFixedAssign(0, numVLs, &lowPrioritySLs, qos, 0, VirtualFabrics);
        goto done;
    }

    // If we have no low and high, assign all VLs to high.
    if ((highPrioritySLs.nset_m==0) && (lowPrioritySLs.nset_m!=0)) {
        slscFixedAssign(0, numVLs, &highPrioritySLs, qos, 1, VirtualFabrics);
        goto done;
    }

    // We have a mix of high and low.  Assign Low reserving at least 1 for high.
    slscFixedAssign(0, numVLs-1, &lowPrioritySLs, qos, 0, VirtualFabrics);
    vl = bitset_find_next_zero(&qos->lowPriorityVLs, 1);
    slscFixedAssign(vl, numVLs, &highPrioritySLs, qos, 1, VirtualFabrics);

done:

    bitset_free(&usedSLs);
    bitset_free(&highPrioritySLs);
    bitset_free(&lowPrioritySLs);

    sm_roundVLBandwidths(qos);
    sm_DbgPrintQOS(qos);
}

static void
AllocateSCsForFixedMapFor1VL(Qos_t * qos, VirtualFabrics_t *VirtualFabrics)
{
    // Assign SCs to SLs for Fixed mapping.
    int sl, vf;
    bitset_t usedSLs;

    if (!bitset_init(&sm_pool, &usedSLs, STL_MAX_SLS) ||
        !bitset_init(&sm_pool, &qos->highPriorityVLs, STL_MAX_VLS) ||
        !bitset_init(&sm_pool, &qos->lowPriorityVLs, STL_MAX_VLS)) {
        IB_FATAL_ERROR("AllocateSCsForFixedMap: No memory for QoS setup, exiting.");
    }

    qos->numVLs = 1;
    bitset_set(&qos->lowPriorityVLs, 0);

    for (vf=0; vf < VirtualFabrics->number_of_vfs_all; vf++) {
        sl = VirtualFabrics->v_fabric_all[vf].base_sl;
        if (bitset_test(&usedSLs, sl)==0) {
            bitset_set(&usedSLs, sl); 

			//Only update qos->vlBandwidth if this VF is active
			if (!VirtualFabrics->v_fabric_all[vf].standby)
				qos->vlBandwidth[0] += sm_SlBandwidthAllocated[sl];
        }
    }

    bitset_free(&usedSLs);

    sm_roundVLBandwidths(qos);
    sm_DbgPrintQOS(qos);
}

static void
setup_vlvfmap(Qos_t * qos, VirtualFabrics_t *VirtualFabrics)
{
    uint8_t vf, sc, vl, i, j;

    // Default to no VF per VL
    memset(&qos->vlvf, -1, sizeof(qos->vlvf));

    if (VirtualFabrics->number_of_vfs_all >= MAX_VFABRICS) {
        IB_LOG_ERROR("Unexpected number of VFs", VirtualFabrics->number_of_vfs_all);
        return;
    }

    // Looping through the VFs, looking up SCs to VLs
    for (vf=0; vf < VirtualFabrics->number_of_vfs_all; vf++) {
        sc = VirtualFabrics->v_fabric_all[vf].base_sc;
        for (j=0; j < VirtualFabrics->v_fabric_all[vf].routing_scs; j++, sc++) {
            if (sc==15) sc++; // skip the invalid SC
            vl = qos->scvl.SCVLMap[sc].VL;
            if ((vl==15) || (vl >= STL_MAX_VLS)) {
                IB_LOG_WARN("Unexpected SC:VL Mapping: VL=", vl);
                IB_LOG_WARN("Unexpected SC:VL Mapping: SC=", sc);
                continue;
            }

            // Assign next available vl-vf
            for (i=0; i<MAX_VFABRICS; i++) {
                if (qos->vlvf.vf[vl][i] == vf) break; // No dup vfs
                if (qos->vlvf.vf[vl][i] == -1) {
                    qos->vlvf.vf[vl][i] = vf;
                    break;
                }
            }
        }
    }

    //// Debug code... 
    //printf("Num VLs: %d\n",qos->numVLs);
    //for (vl=0; vl<STL_MAX_VLS;vl++) {
    //    if (qos->vlvf.vf[vl][0]!=-1) {
    //        printf("VL %d is assigned to VF:", vl); 
    //        for (i=0; i<MAX_VFABRICS; i++) {
    //            if (qos->vlvf.vf[vl][i] == -1) break;
    //            printf("%d, ", qos->vlvf.vf[vl][i]);
    //        }
    //        printf("\n");
    //    }
    //}
    //printf("\n\n");
}

void
sm_setup_SC2VL(VirtualFabrics_t *VirtualFabrics)
{
	//Dynamic reconfiguration won't work if this non fixed mapping is used; 
	//need to reassess changes that are necessary

    int i,j;
    for (i=1; i<STL_MAX_VLS; i++) {
        memset(&sm_Qos[i], 0, sizeof(sm_Qos[i]));
        for (j=0; j< STL_MAX_SCS; j++) {
            sm_Qos[i].scvl.SCVLMap[j].VL=15; // Invalid VL
        }
        AllocateVL(i, &sm_Qos[i], VirtualFabrics);
        setup_vlvfmap(&sm_Qos[i], VirtualFabrics);
    }
}


void
sm_setup_SC2VLFixedMap(int numMandatoryVLs, VirtualFabrics_t *VirtualFabrics)
{
    int i, j;
    for (i=1; i<STL_MAX_VLS; i++) {
        memset(&sm_Qos[i], 0, sizeof(sm_Qos[i]));
        for (j=0; j< STL_MAX_SCS; j++) {
            sm_Qos[i].scvl.SCVLMap[j].VL=15; // Invalid VL
        }
    }

    // Setup 1::1 standard SC::VL map for min required VLs
    for (i=0, j=0; i < STL_MAX_SCS; i++,j++) {
        if (j==numMandatoryVLs) j=0;
        if (i==15) continue;
        sm_Qos[numMandatoryVLs].scvl.SCVLMap[i].VL = j; 
    }

    // Setup the single supported QOS for the required number of VLs
    AllocateSCsForFixedMap(numMandatoryVLs, &sm_Qos[numMandatoryVLs], VirtualFabrics);

    // Setup the base SCs in VF.
    // This needs to be done before setting the VLVF maps.
    for (i=0; i < VirtualFabrics->number_of_vfs_all; i++) {
        // Note - assumes routing SCs = 1.
        VirtualFabrics->v_fabric_all[i].base_sc =
            sm_SLtoSC[VirtualFabrics->v_fabric_all[i].base_sl];

		//Update base_sc if this VF is active
		uint32_t idxToUpdate;
		if (!VirtualFabrics->v_fabric_all[i].standby) {
			idxToUpdate = findVfIdxInActiveList(&VirtualFabrics->v_fabric_all[i], VirtualFabrics);
			if (idxToUpdate != -1) {
				VirtualFabrics->v_fabric[idxToUpdate].base_sc = VirtualFabrics->v_fabric_all[i].base_sc;
			}
		}
    }

    setup_vlvfmap(&sm_Qos[numMandatoryVLs], VirtualFabrics);

    // Because of ESP0 - we must also setup QOS struct for 1 VL
    // Setup VL=1 QOS map.
    for (j=0; j < STL_MAX_SCS; j++) {
        if (j==15) continue;
        sm_Qos[1].scvl.SCVLMap[j].VL = 0; 
    }
    AllocateSCsForFixedMapFor1VL(&sm_Qos[1], VirtualFabrics);
    setup_vlvfmap(&sm_Qos[1], VirtualFabrics);

}


Qos_t*
GetQos(uint8_t vl) {
#ifdef USE_FIXED_SCVL_MAPS

    // ESP0 still has only 1 VL. Must support this.
    if (vl==1) {
        return &sm_Qos[1];
    }
    if (vl!=sm_config.min_supported_vls) {
        IB_LOG_ERROR("Unexpected number of VLs:", vl);
    }
	return &sm_Qos[sm_config.min_supported_vls];
#else
    // Range of data VLs is 1 - 31
    if ((vl>=1) && (vl<STL_MAX_VLS)) {
        return &sm_Qos[vl];
    }
	return &sm_Qos[1];
#endif 
}

static Status_t
sm_initialize_Switch_SLSCMap(Topology_t * topop, Node_t * switchp,
    STL_SLSCMAP * slscmapp)
{
    Status_t status = VSTATUS_OK; 
    Port_t *swportp; 

    IB_ENTER(__func__, topop, switchp, 0, 0); 

    // Let all switch ports default to the power on default for a switch port,
    // which is a default SCtoSC table which is
    // 1:1 for all combinations of ingress and egress switch ports.
    // 
    // The SL2SC map will only be set for switch port zero, as defined by the
    // following requirement in section 20.2.2.6.10 SLtoSCMappingTable:
    // For switches, it is only used to traffic sent by switch port 0
    swportp = sm_get_port(switchp, 0); 
    if (!sm_valid_port(swportp)) {
        IB_LOG_WARN_FMT(__func__, 
                        "Failed to get Port 0 of Switch " FMT_U64, 
                        switchp->nodeInfo.NodeGUID); 
        return VSTATUS_BAD;
    }

    if (!swportp->portData->current.slsc) {
        IB_LOG_WARN_FMT(__func__,
            "SLSC for node %s nodeGuid "FMT_U64" port %d is not current",
            sm_nodeDescString(switchp), switchp->nodeInfo.NodeGUID, swportp->index);
    }

    STL_SLSCMAP * curSlsc = &swportp->portData->slscMap;

    status = topop->routingModule->funcs.select_slsc_map(topop, switchp, swportp, swportp, slscmapp); 
    if (status != VSTATUS_OK) {
        IB_LOG_WARNRC("Failed to get SLSC "
                      "map from routing algorithm; using default; rc:", 
                      status);
    }
    
    // Compare the port's current SLSC map against what the topology says it
    // should be. If they're different, send the new one.
    if (!swportp->portData->current.slsc ||
        memcmp((void *)curSlsc, (void *)slscmapp, sizeof(*slscmapp)) != 0 ||
        sm_config.forceAttributeRewrite) {
#if DO_INLINE_SET
        status = SM_Set_SLSCMap_LR(fd_topology, amod, sm_lid, swportp->portData->lid, slscmapp, sm_config.mkey); 
        
        if (status != VSTATUS_OK) {
            IB_LOG_WARN_FMT(__func__, 
                            "Failed to set SLSC Map for switch node %s nodeGuid " FMT_U64, 
                            sm_nodeDescString(switchp), switchp->nodeInfo.NodeGUID); 
            switchp->slscChange = 1;
        }
        swportp->portData->current.slsc = (status == VSTATUS_OK);
#else
        swportp->portData->dirty.slsc = 1;
        sm_markSmaChanged(topop, switchp);
#endif
    }

#if DO_INLINE_SET
    // Set SLSC Map for the switch port 0
    swportp->portData->slscMap = *slscmapp; 
#endif


    IB_EXIT(__func__, 0); 
    return (status);
}

static Status_t
sm_initialize_Switch_SCSLMap(Topology_t * topop, Node_t * switchp,
    STL_SCSLMAP * scslmapp)
{
    Status_t status = VSTATUS_OK;
    Port_t *swportp; 
    
    IB_ENTER(__func__, topop, switchp, 0, 0); 
    
    //
    // For switches this table is only used for traffic from switch port 0    
    swportp = sm_get_port(switchp, 0); 
    if (!sm_valid_port(swportp)) {
        IB_LOG_WARN_FMT(__func__, 
                        "Failed to get Port 0 of Switch " FMT_U64, 
                        switchp->nodeInfo.NodeGUID); 
        IB_EXIT(__func__, 1); 
        return VSTATUS_BAD;
    }

    if (!swportp->portData->current.scsl) {
        IB_LOG_WARN_FMT(__func__,
          "SCSL for node %s nodeGuid "FMT_U64" port %d is not current",
          sm_nodeDescString(switchp), switchp->nodeInfo.NodeGUID, swportp->index);
    }
    STL_SCSLMAP * curScsl = &swportp->portData->scslMap;

    status = topop->routingModule->funcs.select_scsl_map(topop, switchp, swportp, swportp, scslmapp); 
    if (status != VSTATUS_OK) {
        IB_LOG_WARNRC("Failed to get SCSL "
                      "map from routing algorithm; using default; rc:", 
                      status);
    }
    
    // compare the port's current SCSL map against what the topology says it
    // should be. If they're different, send the new one.
    if (!swportp->portData->current.scsl ||
        memcmp((void *)curScsl, (void *)scslmapp, sizeof(*scslmapp)) != 0 || sm_config.forceAttributeRewrite) {

#if DO_INLINE_SET
        status = SM_Set_SCSLMap_LR(fd_topology, amod, sm_lid, 
                                   swportp->portData->lid, scslmapp, sm_config.mkey); 
        
        if (status != VSTATUS_OK) {
            IB_LOG_WARN_FMT(__func__, 
                            "Failed to set SCSL Map for switch node %s nodeGuid " FMT_U64, 
                            sm_nodeDescString(switchp), switchp->nodeInfo.NodeGUID); 
            switchp->slscChange = 1;
        }
        swportp->portData->current.scsl = (status == VSTATUS_OK);
#else
        swportp->portData->dirty.scsl = 1;
        sm_markSmaChanged(topop, switchp);
#endif
    }

#if DO_INLINE_SET
    swportp->portData->scslMap = *scslmapp; 
#endif

    IB_EXIT(__func__, 0); 
    return (status);
}

static Status_t
sm_initialize_Switch_SCVLMaps(Topology_t * topop, Node_t * switchp)
{
    uint32_t amod = 0; 
    uint8_t synchModeGen1 = 1;
    Status_t status = VSTATUS_OK; 
    Node_t *neighborNodep; 
    Port_t * out_portp,*neighborPortp, *swportp, *neighborSwPortp; 
    STL_SCVLMAP scvlmap; 

    IB_ENTER(__func__, topop, switchp, 0, 0);

    swportp = sm_get_port(switchp, 0);
    if (!sm_valid_port(swportp)) {
        IB_LOG_WARN_FMT(__func__, 
            "Failed to get Port 0 of Switch " FMT_U64,
            switchp->nodeInfo.NodeGUID);
        status = VSTATUS_BAD;
        goto fail;
    }

    // 
    // for every node/port combo, we need to setup the SC->VL_t and SC_->VL_nt
    // mapping.  According to Volume 1, Section 20.2.2.6.14 SCtoVLxMappingTable.
    
    for_all_ports(switchp, out_portp) {
        if (!sm_valid_port(out_portp) || out_portp->state <= IB_PORT_DOWN) 
            continue; 

        if ((neighborNodep = sm_find_node(topop, out_portp->nodeno)) == NULL || 
            (neighborPortp = sm_find_node_port(topop, neighborNodep, out_portp->portno)) == NULL || 
            !sm_valid_port(neighborPortp)) {
            IB_LOG_WARN_FMT(__func__, 
                            "Unable to get neighbor to node %s nodeGuid " FMT_U64
                            " output port %d", sm_nodeDescString(switchp), 
                            switchp->nodeInfo.NodeGUID, out_portp->index); 
            status = VSTATUS_BAD;
            goto fail;
        }

        if (neighborNodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
            neighborSwPortp = sm_get_port(neighborNodep, 0);
            if (!sm_valid_port(neighborSwPortp)) {
                IB_LOG_WARN_FMT(__func__, 
                    "Failed to get Port 0 of Switch " FMT_U64,
                    neighborNodep->nodeInfo.NodeGUID);
                status = VSTATUS_BAD;
                goto fail;
            }
        }

        if (!out_portp->portData->current.scvlt) {
            IB_LOG_WARN_FMT(__func__,
                "SCVLt for node %s nodeGuid "FMT_U64" port %d is not current",
                sm_nodeDescString(switchp), switchp->nodeInfo.NodeGUID, out_portp->index);
        }

        if (!neighborPortp->portData->current.scvlnt) {
            IB_LOG_WARN_FMT(__func__,
                "SCVLnt for node %s nodeGuid "FMT_U64" port %d is not current",
                sm_nodeDescString(neighborNodep), neighborNodep->nodeInfo.NodeGUID, neighborPortp->index);
        }

        // the SCtoVL_nt table must be configured consistently with the SCtoVL_t table at its neighbor. When the
        // link state is Init the SM shall have the responsibility of updating both the SCtoVL_t table and the
        // neighbor's SCtoVL_nt table ("synchronous" update). When the link state is Armed or Active, the SM
        // shall update the SCtoVL_t table; and the SMAs of both ports shall have the responsibility of updating
        // the neighbor's SCtoVL_nt table ("asynchronous" update of SCtoVL_t only).        
        //
        // section 9.7.14.3 of the STL spec, "Optional Mechanism for Changes While in LinkArmed or LinkActive"
        // The mechanisms in this section are only available when the ports on both side of a link report
        // IsAsyncSC2VLSupported. If either port reports it does not have this capability, the FM shall not
        // attempt to perform the changes outlined in this section.
        // It is anticipated that STL Gen1 will not support this capability.
        synchModeGen1 = 1;
        if (out_portp->state > IB_PORT_INIT || neighborPortp->state > IB_PORT_INIT) {
            // when asynchronous mode is supported in Gen2 additional checks should
            // be done against the IsAsyncSC2VLSupported field. 
            synchModeGen1 = 0;
        }

        //
        // initialize the SC2VL_t map of the switch port
        amod = (out_portp->state == IB_PORT_INIT) ? 1 << 24 : (1 << 24) | 1 << 12;   // 1 block, synch/asynch respectively
        amod |= (uint32_t)out_portp->index;

        STL_SCVLMAP * curScvl = &out_portp->portData->scvltMap;

        status = topop->routingModule->funcs.select_scvl_map(topop, switchp, out_portp, neighborPortp, &scvlmap);
        if (status != VSTATUS_OK) {
            IB_LOG_WARNRC("Failed to get SCVL "
                "map from routing algorithm; using default; rc:",
                status); 
            continue;
        }

        // compare the port's current SCVL map against what the topology says it
        // should be. If they're different, send the new one.
        if (!out_portp->portData->current.scvlt ||
            memcmp((void *)curScvl, (void *)&scvlmap, sizeof(scvlmap)) != 0 || sm_config.forceAttributeRewrite) {
            if (synchModeGen1) {
                status = SM_Set_SCVLtMap_LR(fd_topology, amod, sm_lid, swportp->portData->lid, &scvlmap, sm_config.mkey); 

                if (status != VSTATUS_OK) {
                    IB_LOG_WARN_FMT(__func__, 
                                    "Failed to set SCVL_t Map for node %s nodeGuid " FMT_U64
                                    " output port %d", sm_nodeDescString(switchp), 
                                    switchp->nodeInfo.NodeGUID, out_portp->index);
                }
                out_portp->portData->current.scvlt = (status == VSTATUS_OK);
            } else {
                IB_LOG_WARN_FMT(__func__, 
                                "Mismatch/Unable to set SCVL_t Map for node %s nodeGuid " FMT_U64
                                " output port %d", sm_nodeDescString(switchp), 
                                switchp->nodeInfo.NodeGUID, out_portp->index);
            }
        }
        
        // set SCVL_t Map for the port
        out_portp->portData->scvltMap = scvlmap; 

        //
        // initialize the SCVL_nt map of the neighbor port.  When the link state is Armed or Active, the
        // SMAs of both ports shall have the responsibility of updating the neighbor's SCtoVL_nt table
        // ("asynchronous" update of SCtoVL_t only).
        amod = (1 << 24) | neighborPortp->index;   // 1 block, sych update

        STL_SCVLMAP * curScvlnt = &neighborPortp->portData->scvlntMap;
        if (!neighborPortp->portData->current.scvlnt ||
            memcmp((void *)curScvlnt, (void *)&scvlmap, sizeof(scvlmap)) != 0 || sm_config.forceAttributeRewrite) {
            if (synchModeGen1) {
                status = SM_Set_SCVLntMap_LR(fd_topology,
                                             amod,
                                             sm_lid, 
                                             (neighborNodep->nodeInfo.NodeType == NI_TYPE_SWITCH) ? neighborSwPortp->portData->lid : neighborPortp->portData->lid,
                                             &scvlmap,
                                             sm_config.mkey); 
                
                if (status != VSTATUS_OK) {
                    IB_LOG_WARN_FMT(__func__, 
                                    "Failed to set SCVL_nt Map for node %s nodeGuid " FMT_U64
                                    " output port %d", sm_nodeDescString(neighborNodep), 
                                    neighborNodep->nodeInfo.NodeGUID, neighborPortp->index);
                }
                neighborPortp->portData->current.scvlnt = (status == VSTATUS_OK);
            } else {
                IB_LOG_WARN_FMT(__func__, 
                                "Mismatch/Unable to set SCVL_nt Map for node %s nodeGuid " FMT_U64
                                " output port %d", sm_nodeDescString(neighborNodep), 
                                neighborNodep->nodeInfo.NodeGUID, neighborPortp->index);
            }
        }
        
        // set SCVL_nt Map for the neighbor port
        neighborPortp->portData->scvlntMap = scvlmap;
    }

fail:
    IB_EXIT(__func__, 0); 
    return (status);
}

static Status_t
sm_initialize_Node_Port_SLSCMap(Topology_t * topop, Node_t * nodep, Port_t * out_portp, STL_SLSCMAP * slscmapp)
{ 
//  uint8_t *path;
    Status_t status = VSTATUS_OK; 
    
    IB_ENTER(__func__, topop, nodep, out_portp, 0); 
    
//  path = PathToPort(nodep, out_portp);
    
    // 
    // for every node/port combo, we need to setup the SL->SC mapping.
    // According to Volume 1, Section 20.2.2.6.10 SLtoSCMappingTable.
    
    // 
    // if this port is DOWN, then we do nothing.
    // 
    if (out_portp->state <= IB_PORT_DOWN) {
        IB_EXIT(__func__, 1); 
        return (VSTATUS_OK);
    }
    
    // note: if node was previously non-responding. Don't bother going any further.
    if (nodep->nonRespCount) {
        return (VSTATUS_OK);
    }

    if (!out_portp->portData->current.slsc) {
        IB_LOG_WARN_FMT(__func__,
            "SCSL for node %s nodeGuid "FMT_U64" port %d is not current",
            sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, out_portp->index);
    }
    STL_SLSCMAP * curSlsc = &out_portp->portData->slscMap;

    status = topop->routingModule->funcs.select_slsc_map(topop, nodep, out_portp, out_portp, slscmapp); 
    if (status != VSTATUS_OK) {
        IB_LOG_WARNRC("Failed to get SLSC "
                      "map from routing algorithm; using default; rc:", 
                      status);
    }
    
    // 
    // Compare the port's current SLSC map against what the topology says it
    // should be. If they're different, send the new one.
    // 
    if (!out_portp->portData->current.slsc ||
        memcmp((void *)curSlsc, (void *)slscmapp, sizeof(*slscmapp)) != 0 || sm_config.forceAttributeRewrite) {
#if DO_INLINE_SET
        status = SM_Set_SLSCMap_LR(fd_topology, amod, sm_lid, out_portp->portData->lid, slscmapp, sm_config.mkey); 
        
        if (status != VSTATUS_OK) {
            IB_LOG_WARN_FMT(__func__, 
                            "Failed to set SLSC Map for node %s nodeGuid " FMT_U64
                            " output port %d", sm_nodeDescString(nodep), 
                            nodep->nodeInfo.NodeGUID, out_portp->index); 
            nodep->slscChange = 1;
        }
        out_portp->portData->current.slsc = (status == VSTATUS_OK);
#else
        out_portp->portData->dirty.slsc = 1;
        sm_markSmaChanged(topop, nodep);
#endif
    }

#if DO_INLINE_SET
    // Set SLSC Map for the port
    out_portp->portData->slscMap = *slscmapp; 
#endif
    
    IB_EXIT(__func__, 0); 
    return (status);
}

static Status_t
sm_initialize_Node_Port_SCSLMap(Topology_t * topop, Node_t * nodep, Port_t * in_portp, STL_SCSLMAP * scslmapp)
{ 
//  uint8_t *path;
    Status_t status = VSTATUS_OK; 
    
    IB_ENTER(__func__, topop, nodep, in_portp, 0); 
    
//  path = PathToPort(nodep, in_portp);
    
    // 
    // for every node/port combo, we need to setup the SC->SL mapping.
    // According to Volume 1, Section 20.2.2.6.13 SCtoSLMappingTable.
    
    if (in_portp->state <= IB_PORT_DOWN) {
        IB_EXIT(__func__, 1); 
        return (VSTATUS_OK);
    } else if (nodep->nodeInfo.NodeType != NI_TYPE_CA) {
        
        IB_EXIT(__func__, 2); 
        return (VSTATUS_OK);
    }
    
    // 
    // set up the SC->SL mapping just for this port.
    
    // note: if node was previously non-responding. Don't bother going any further.
    if (nodep->nonRespCount) {
        return (VSTATUS_OK);
    }

    if (!in_portp->portData->current.scsl) {
        IB_LOG_WARN_FMT(__func__,
          "SCSL for node %s nodeGuid "FMT_U64" port %d is not current",
          sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, in_portp->index);
    }
    STL_SCSLMAP * curScsl = &in_portp->portData->scslMap;

    status = topop->routingModule->funcs.select_scsl_map(topop, nodep, in_portp, in_portp, scslmapp); 
    if (status != VSTATUS_OK) {
        IB_LOG_WARNRC
           ("sm_initialize_Node_Port_SCSLMap: Failed to get SCSL "
            "map from routing algorithm; using default; rc:", 
            status);
    }

    // 
    // compare the port's current SCSL map against what the topology says it
    // should be. If they're different, send the new one.
    if (!in_portp->portData->current.scsl ||
        memcmp((void *)curScsl, (void *)scslmapp, sizeof(*scslmapp)) != 0 || sm_config.forceAttributeRewrite) {
#if DO_INLINE_SET
        status = SM_Set_SCSLMap_LR(fd_topology, amod, sm_lid, in_portp->portData->lid, scslmapp, sm_config.mkey); 
            
        if (status != VSTATUS_OK) {
            IB_LOG_WARN_FMT(__func__, 
                            "Failed to set SCSL Map for node %s nodeGuid " FMT_U64
                            " output port %d", sm_nodeDescString(nodep), 
                            nodep->nodeInfo.NodeGUID, in_portp->index);
        }
        in_portp->portData->current.scsl = (status == VSTATUS_OK);
#else
        in_portp->portData->dirty.scsl = 1;
        sm_markSmaChanged(topop, nodep);
#endif
    }

#if DO_INLINE_SET
    // set SCSL Map for the port
    in_portp->portData->scslMap = *scslmapp; 
#endif
    
    IB_EXIT(__func__, 0); 
    return (status);
}

static Status_t
sm_initialize_Node_Port_SCVLMaps(Topology_t * topop, Node_t * nodep, Port_t * in_portp)
{ 
//  uint8_t * path,*neighborPath;
    uint32_t amod = 0; 
    uint8_t synchModeGen1 = 1;
    Status_t status = VSTATUS_OK; 
    Node_t *neighborNodep; 
    Port_t * neighborPortp,*swportp; 
    STL_SCVLMAP scvlmap;
    STL_SCVLMAP * curScvlt, * curScvlnt;

    IB_ENTER(__func__, topop, nodep, in_portp, 0); 

//  path = PathToPort(nodep, in_portp);

    // 
    // for every node/port combo, we need to setup the SC->VL_t and SC_->VL_nt
    // mapping.  According to Volume 1, Section 20.2.2.6.14 SCtoVLxMappingTable.
    
    if (in_portp->state <= IB_PORT_DOWN) {
        IB_EXIT(__func__, 1); 
        return (VSTATUS_OK);
    } else if (nodep->nodeInfo.NodeType != NI_TYPE_CA) {
        IB_EXIT(__func__, 2); 
        return (VSTATUS_OK);
    }
    
    // 
    // set up the SC->VL mappings just for this port and neighbor port.
    
    // note: if node was previously non-responding. Don't bother going any further.
    if (nodep->nonRespCount) 
        return (VSTATUS_OK); 
    
    if ((neighborNodep = sm_find_node(topop, in_portp->nodeno)) == NULL || 
        (neighborPortp = sm_find_node_port(topop, neighborNodep, in_portp->portno)) == NULL || 
        !sm_valid_port(neighborPortp)) {
        IB_LOG_WARN_FMT(__func__, 
                        "Unable to get neighbor to node %s nodeGuid " FMT_U64
                        " output port %d", sm_nodeDescString(nodep), 
                        nodep->nodeInfo.NodeGUID, in_portp->index); 
        return (VSTATUS_BAD);
    }
    
    if (neighborNodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
        swportp = sm_get_port(neighborNodep, 0); 
        if (!sm_valid_port(swportp)) {
            IB_LOG_WARN_FMT(__func__, 
                            "Failed to get Port 0 of Switch " FMT_U64, 
                            neighborNodep->nodeInfo.NodeGUID); 
            return VSTATUS_BAD;
        }
    }

    if (!in_portp->portData->current.scvlt) {
        IB_LOG_WARN_FMT(__func__,
            "SCVLt for node %s nodeGuid "FMT_U64" port %d is not current",
            sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, in_portp->index);
    }

    if (!neighborPortp->portData->current.scvlnt) {
        IB_LOG_WARN_FMT(__func__,
            "SCVLnt for node %s nodeGuid "FMT_U64" port %d is not current",
            sm_nodeDescString(neighborNodep), neighborNodep->nodeInfo.NodeGUID, neighborPortp->index);
    }

    curScvlt = &in_portp->portData->scvltMap;
    curScvlnt = &neighborPortp->portData->scvlntMap;

//  neighborPath = PathToPort(neighborNodep, neighborPortp);
    //    
    // the SCtoVL_nt table must be configured consistently with the SCtoVL_t table at its neighbor. When the
    // link state is Init the SM shall have the responsibility of updating both the SCtoVL_t table and the
    // neighbor's SCtoVL_nt table ("synchronous" update). When the link state is Armed or Active, the SM
    // shall update the SCtoVL_t table; and the SMAs of both ports shall have the responsibility of updating
    // the neighbor's SCtoVL_nt table ("asynchronous" update of SCtoVL_t only).
    //
    // section 9.7.14.3 of the STL spec, "Optional Mechanism for Changes While in LinkArmed or LinkActive"
    // The mechanisms in this section are only available when the ports on both side of a link report
    // IsAsyncSC2VLSupported. If either port reports it does not have this capability, the FM shall not
    // attempt to perform the changes outlined in this section.
    // It is anticipated that STL Gen1 will not support this capability.
    if (in_portp->state > IB_PORT_INIT || neighborPortp->state > IB_PORT_INIT) {
        // when asynchronous mode is supported in Gen2 additional checks should
        // be done against the IsAsyncSC2VLSupported field. 
        synchModeGen1 = 0;
    }

    //
    // get SCVL_t map of the port
    amod = (in_portp->state == IB_PORT_INIT) ? 1 << 24 : (1 << 24) | 1 << 12;   // 1 block, synch/asynch respectively
    amod |= (uint32_t)in_portp->index;

    status = topop->routingModule->funcs.select_scvl_map(topop, nodep, in_portp, in_portp, &scvlmap); 
    if (status != VSTATUS_OK) {
        IB_LOG_WARNRC("Failed to get SCVL "
                      "map from routing algorithm; using default; rc:", 
                      status);
    }

    // 
    // compare the port's current SCVL map against the computed SCVLt map.
    // If they're different, send the new one.
    if (!in_portp->portData->current.scvlt ||
        memcmp((void *)curScvlt, (void *)&scvlmap, sizeof(scvlmap)) != 0 || sm_config.forceAttributeRewrite) {
        if (synchModeGen1) {
            status = SM_Set_SCVLtMap_LR(fd_topology, amod, sm_lid, in_portp->portData->lid, &scvlmap, sm_config.mkey); 

            if (status != VSTATUS_OK) {
                IB_LOG_WARN_FMT(__func__, 
                                "Failed to set SCVL_t Map for node %s nodeGuid " FMT_U64
                                " output port %d", sm_nodeDescString(nodep), 
                                nodep->nodeInfo.NodeGUID, in_portp->index);
            }
            in_portp->portData->current.scvlt = (status == VSTATUS_OK);
        } else {
            IB_LOG_WARN_FMT(__func__, 
                            "Mismatch/Unable to set SCVL_t Map for node %s nodeGuid " FMT_U64
                            " output port %d", sm_nodeDescString(nodep), 
                            nodep->nodeInfo.NodeGUID, in_portp->index);
        }
    }

    // set SCVL_t Map for the port
    in_portp->portData->scvltMap = scvlmap;
    //
    // set SCVL_nt map of the neighbor port
    amod = (1 << 24) | neighborPortp->index; // 1 block, port

    if (!neighborPortp->portData->current.scvlnt ||
        memcmp((void *)curScvlnt, (void *)&scvlmap, sizeof(scvlmap)) != 0 || sm_config.forceAttributeRewrite) {
        if (synchModeGen1) {
            status = SM_Set_SCVLntMap_LR(fd_topology, amod,
                                         sm_lid, 
                                         (neighborNodep->nodeInfo.NodeType == NI_TYPE_SWITCH) ? swportp->portData->lid : neighborPortp->portData->lid, 
                                         &scvlmap, sm_config.mkey); 

            if (status != VSTATUS_OK) {
                IB_LOG_WARN_FMT(__func__, 
                                "Failed to set SCVL_nt Map for neighbor node %s nodeGuid " FMT_U64
                                " output port %d", sm_nodeDescString(neighborNodep), 
                                neighborNodep->nodeInfo.NodeGUID, neighborPortp->index);
            }
            neighborPortp->portData->current.scvlnt = (status == VSTATUS_OK);
        } else {
            IB_LOG_WARN_FMT(__func__, 
                            "Mismatch/Unable to set SCVL_nt Map for neighbor node %s nodeGuid " FMT_U64
                            " output port %d", sm_nodeDescString(neighborNodep), 
                            neighborNodep->nodeInfo.NodeGUID, neighborPortp->index);
        }
    }

    // set SCVL_nt Map for the neighbor port
    neighborPortp->portData->scvlntMap = scvlmap;
    
    IB_EXIT(__func__, 0); 
    return (status);
}

Status_t
sm_initialize_Node_SLMaps(Topology_t * topop, Node_t * nodep, Port_t * out_portp)
{ 
    Status_t status; 

    STL_SCSLMAP scslmap;
    STL_SLSCMAP slscmap;

    status = sm_port_init_changes(out_portp);
    if (status != VSTATUS_OK)
      return status;

    out_portp->portData->changes.slsc = &slscmap;
    out_portp->portData->changes.scsl = &scslmap;

    // initialize the SL2SC mapping table for the egress port
    status = sm_initialize_Node_Port_SLSCMap(sm_topop, nodep, out_portp, out_portp->portData->changes.slsc); 
    
    // initialize the SC2SL mapping table for the egress port
    if (status == VSTATUS_OK) 
        status = sm_initialize_Node_Port_SCSLMap(sm_topop, nodep, out_portp, out_portp->portData->changes.scsl); 

    // initialize the SC2VL* mapping tables for the egress port
    if (status == VSTATUS_OK)
        status = sm_initialize_Node_Port_SCVLMaps(sm_topop, nodep, out_portp); 

    Node_t * lastNode = NULL;
    status = sm_syncSmaChanges(topop, &lastNode);

    if (status != VSTATUS_OK) {
        IB_LOG_ERROR_FMT(__func__,
            "Error on synching SMA updates; status : 0x%02x; last-node processed NodeGUID : "FMT_U64,
            status, ( lastNode ? lastNode->nodeInfo.NodeGUID : 0));
    }

    return status;
}

Status_t
sm_initialize_Switch_SLMaps(Topology_t * topop, Node_t * nodep)
{
    IB_ENTER(__func__, topop, nodep, 0, 0);
    Status_t status;
    Port_t * swportp = sm_get_port(nodep, 0);

    STL_SCSLMAP scslmap;
    STL_SLSCMAP slscmap;

    if (!sm_valid_port(swportp)) {
        return VSTATUS_BAD;
    }

    status = sm_port_init_changes(swportp);
    if (status != VSTATUS_OK)
        return status;

    swportp->portData->changes.slsc = &slscmap;
    swportp->portData->changes.scsl = &scslmap;

    // initialize the SL2SC mapping table for all ports.
    status = sm_initialize_Switch_SLSCMap(sm_topop, nodep, swportp->portData->changes.slsc);

    // initialize the SC2SL mapping table for all ports.
    if (status == VSTATUS_OK) 
        status = sm_initialize_Switch_SCSLMap(sm_topop, nodep, swportp->portData->changes.scsl);

    // initialize the SC2VL* mapping tables for all ports.
    if (status == VSTATUS_OK)
        status = sm_initialize_Switch_SCVLMaps(sm_topop, nodep);

    Node_t * lastNode = NULL;
    status = sm_syncSmaChanges(topop, &lastNode);

    if (status != VSTATUS_OK) {
        IB_LOG_ERROR_FMT(__func__,
            "Error on synching SMA updates; status : 0x%02x; last-node processed NodeGUID : "FMT_U64,
            status, ( lastNode ? lastNode->nodeInfo.NodeGUID : 0));
    }

    IB_EXIT(__func__, status);
    return status;
}

/**
	Update the SMA of @c nodep with all changes stored on @c nodep.
*/
static Status_t
sm_node_syncSmaChanges(Topology_t * topop, Node_t * nodep, Port_t * smaportp)
{
	Status_t s;

	if (nodep->aggregateEnable) {
		s = sm_node_syncSma_aggregate(topop, nodep, smaportp);
	}

	if (!nodep->aggregateEnable || s != VSTATUS_OK) {
		s = sm_node_syncSma_solo(topop, nodep, smaportp);

		if (nodep->aggregateEnable && s == VSTATUS_OK) {
			IB_LOG_WARN_FMT(__func__,
				"Disabling aggregate support on node %s nodeGUID "FMT_U64,
				sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID);
				nodep->aggregateEnable = 0;
		}
	}

	return s;
}

static Status_t
sm_node_syncSma_aggregate(Topology_t * topop, Node_t * nodep, Port_t * smaportp)
{
	Status_t s = VSTATUS_OK;
	STL_AGGREGATE * aggrBuffer = NULL;

	STL_LID_32 destLid;

	if (!sm_valid_port(smaportp))
		return VSTATUS_BAD;

	destLid = smaportp->portData->portInfo.LID;

	const size_t reqMem =
		smaportp->portData->dirty.scsl * (sizeof(STL_AGGREGATE) + sizeof(STL_SCSLMAP)) +
		smaportp->portData->dirty.slsc * (sizeof(STL_AGGREGATE) + sizeof(STL_SLSCMAP));

	if (reqMem == 0)
		return VSTATUS_OK;

  vs_pool_alloc(&sm_pool, reqMem, (void*)&aggrBuffer);
  if (!aggrBuffer)
    return VSTATUS_BAD;

	STL_AGGREGATE * aggrHdr = aggrBuffer;

	if (smaportp->portData->dirty.scsl) {
		aggrHdr->AttributeID = STL_MCLASS_ATTRIB_ID_SC_SL_MAPPING_TABLE;
		aggrHdr->Result.s.Error = 0;
		aggrHdr->Result.s.RequestLength = (sizeof(STL_SLSCMAP) + 7)/8;
		aggrHdr->AttributeModifier = 0;

		memcpy(aggrHdr->Data, smaportp->portData->changes.scsl, sizeof(STL_SCSLMAP));
		BSWAP_STL_SCSLMAP((STL_SCSLMAP *)aggrHdr->Data);

		aggrHdr = STL_AGGREGATE_NEXT(aggrHdr);
	}

	if (smaportp->portData->dirty.slsc) {
		aggrHdr->AttributeID = STL_MCLASS_ATTRIB_ID_SL_SC_MAPPING_TABLE;
		aggrHdr->Result.s.Error = 0;
		aggrHdr->Result.s.RequestLength = (sizeof(STL_SLSCMAP) + 7)/8;

		aggrHdr->AttributeModifier = 0;

		memcpy(aggrHdr->Data, smaportp->portData->changes.slsc, sizeof(STL_SLSCMAP));
		BSWAP_STL_SLSCMAP((STL_SLSCMAP *)aggrHdr->Data);
		aggrHdr = STL_AGGREGATE_NEXT(aggrHdr);
	}

	uint32_t madStatus = 0;
	STL_AGGREGATE * lastSeg = NULL;

	s = SM_Set_Aggregate_LR(fd_topology, aggrBuffer, aggrHdr,
		sm_lid, destLid, sm_config.mkey, &lastSeg, &madStatus);

	if (!lastSeg && s == VSTATUS_OK) {
		s = VSTATUS_BAD;
		goto fail; 
	}

	if (lastSeg) {
		// Can still process partial aggregate response on MAD error
		Status_t tmpStatus = sm_node_handleGetRespAggr(nodep, smaportp, aggrBuffer, lastSeg);
		s = (s != VSTATUS_OK? s : tmpStatus);
	}

	if (s != VSTATUS_OK || madStatus != MAD_STATUS_SUCCESS) {
		IB_LOG_ERROR_FMT(__func__,
			"Error on Set(Aggregate): NodeGUID : "FMT_U64"; NodeDesc : \"%s\"; status : %d; madStatus : 0x%02x",
			nodep->nodeInfo.NodeGUID, sm_nodeDescString(nodep), s, madStatus);
		if (madStatus != MAD_STATUS_SUCCESS && lastSeg != NULL) {
			IB_LOG_ERROR_FMT(__func__,
				"First error: AttributeID : 0x%02x; AttributeModifier : 0x%08x",
				lastSeg->AttributeID, lastSeg->AttributeModifier);
		}

		//@todo: do dirty.slsc & dirty.scsl make slscChange redundant?
		nodep->slscChange = smaportp->portData->dirty.slsc || smaportp->portData->dirty.scsl;
		goto fail;
	}

	boolean clean = !(smaportp->portData->dirty.slsc || smaportp->portData->dirty.scsl);

	if (clean) {
		sm_clearSmaChanged(topop, nodep);
	}

fail:
	vs_pool_free(&sm_pool, aggrBuffer);

	return s;
}

static Status_t
sm_node_syncSma_solo(Topology_t * topop, Node_t * nodep, Port_t * smaportp)
{
	Status_t status = VSTATUS_OK;

	STL_LID_32 destLid;

	if (!sm_valid_port(smaportp))
		return VSTATUS_BAD;

	destLid = smaportp->portData->lid;

	if (smaportp->portData->dirty.slsc) {
		uint32_t amod = 0;
		STL_SLSCMAP slsc = *smaportp->portData->changes.slsc;

		status = SM_Set_SLSCMap_LR(fd_topology, amod, sm_lid, destLid, &slsc, sm_config.mkey); 
		
		if (status != VSTATUS_OK) {
			nodep->slscChange = 1;
			goto fail;
		}

		smaportp->portData->slscMap = slsc;
		smaportp->portData->dirty.slsc = 0;
	}

	if (smaportp->portData->dirty.scsl) {
		uint32_t amod = 0;
		STL_SCSLMAP scsl = *smaportp->portData->changes.scsl;
		status = SM_Set_SCSLMap_LR(fd_topology, amod, sm_lid, destLid, &scsl, sm_config.mkey);

		if (status != VSTATUS_OK) {
			nodep->slscChange = 1;
			goto fail;
		}

		smaportp->portData->scslMap = scsl;
		smaportp->portData->dirty.scsl = 0;
	}

	boolean clean = !(smaportp->portData->dirty.slsc || smaportp->portData->dirty.scsl);

	if (clean) {
		sm_clearSmaChanged(topop, nodep);
	}

fail:
	return status;
}

static Status_t
sm_syncSmaChanges(Topology_t * topop, Node_t ** firstError)
{
	Status_t s = VSTATUS_OK;
	Status_t retStat = VSTATUS_OK;
	int nodeIdx;

	if (!topop->smaChanges)
		return VSTATUS_OK;

	nodeIdx = bitset_find_first_one(topop->smaChanges);
	while (nodeIdx >= 0) {
		if (nodeIdx > topop->num_nodes) {
			IB_LOG_ERROR_FMT(__func__,
				"nodeIdx > num_nodes; nodeIdx : %d; num_nodes : %d",
				nodeIdx, topop->num_nodes);
				return VSTATUS_BAD;
		}

		Node_t * nodep = sm_find_node(topop, nodeIdx);

		if (nodep) {
			Port_t * smaportp = NULL;

			for_all_sma_ports(nodep, smaportp) {
				if (!sm_valid_port(smaportp) || smaportp->state < IB_PORT_INIT)
					continue;

				s = sm_node_syncSmaChanges(topop, nodep, smaportp);

				if (s != VSTATUS_OK) {
					if (retStat == VSTATUS_OK) {
						if (firstError)
							*firstError = nodep;
						retStat = s;
					}

					IB_LOG_WARN_FMT(__func__,
							"Failed to sync changes for node %s, node GUID "FMT_U64", port %d",
							sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, smaportp->index);
					}
				}

				// dirty and change values are not currently copied sweep-to sweep, so free to
				// release them whether or not Set() was successful
				sm_node_release_changes(nodep);
		}

		nodeIdx = bitset_find_next_one(topop->smaChanges, nodeIdx + 1);
	}

	return retStat;
}

static Status_t
sm_node_handleGetRespAggr(Node_t * nodep, Port_t * smaportp, STL_AGGREGATE * aggr, STL_AGGREGATE * end)
{
	Status_t s = VSTATUS_OK;
	for ( ; aggr < end; aggr = STL_AGGREGATE_NEXT(aggr)) {
		switch (aggr->AttributeID) {
			//@todo: any way to combine this code with the code in SM_Set_<T>*()?
			case STL_MCLASS_ATTRIB_ID_SL_SC_MAPPING_TABLE:
				smaportp->portData->dirty.slsc &= aggr->Result.s.Error;
				if (!aggr->Result.s.Error) {
					if ((aggr->Result.s.RequestLength * 8) < sizeof(STL_SLSCMAP)) {
						s = VSTATUS_BAD;
						break;
					}

					memcpy(&smaportp->portData->slscMap, aggr->Data, sizeof(STL_SLSCMAP));
					ZERO_RSVD_STL_SLSCMAP(&smaportp->portData->slscMap);
					smaportp->portData->current.slsc = 1;
				}
				break;
			case STL_MCLASS_ATTRIB_ID_SC_SL_MAPPING_TABLE:
				smaportp->portData->dirty.scsl &= aggr->Result.s.Error;
				if (!aggr->Result.s.Error) {
					if ((aggr->Result.s.RequestLength * 8) < sizeof(STL_SCSLMAP)) {
						s = VSTATUS_BAD;
						break;
					}

					memcpy(&smaportp->portData->scslMap, aggr->Data, sizeof(STL_SCSLMAP));
					BSWAP_STL_SCSLMAP(&smaportp->portData->scslMap);
					smaportp->portData->current.scsl = 1;
				}
				break;
			case STL_MCLASS_ATTRIB_ID_SC_VLT_MAPPING_TABLE:
			case STL_MCLASS_ATTRIB_ID_SC_VLNT_MAPPING_TABLE:
				s = sm_aggregateToScvl(nodep, aggr);
				break;
			case STL_MCLASS_ATTRIB_ID_VL_ARBITRATION:
				s = sm_aggregateToVlarb(nodep, aggr);
				break;
			default:
				return VSTATUS_BAD;
		}

		if (s != VSTATUS_OK)
			break;
	}

	return s;
}

static Status_t
sm_aggregateToScvl(Node_t * nodep, STL_AGGREGATE * aggr)
{
	// Note: a GetResp() can be caused by either a Get() or a Set()
	//
	//
	//@todo: any way to combine this with code from sm_Get_SCVL*
	if (!aggr->Result.s.Error) {
		uint8 blkCount, startPort;
		uint16 endPort; // [startPort, endPort)
		boolean allPorts = (aggr->AttributeModifier >> 8) & 0x1;

		// The block count of the response is constrained by the size of the segment
		blkCount = (uint8)(aggr->AttributeModifier >> 24);

		startPort = (uint8) aggr->AttributeModifier;
		if (allPorts)
			endPort = nodep->nodeInfo.NumPorts + 1;
		else {
			if ((uint16)startPort + blkCount > nodep->nodeInfo.NumPorts + 1) {
				IB_LOG_ERROR_FMT(__func__,
					"Computed end port exceeds number of ports: computed end port : %d; number of ports : %d",
					(uint16)startPort + blkCount, nodep->nodeInfo.NumPorts);
				return VSTATUS_BAD;
			}
			endPort = startPort + blkCount;
		}

		if (blkCount == 0) {
			IB_LOG_ERROR_FMT(__func__, "Amod block count cannot be zero");
			return VSTATUS_BAD;
		}

		size_t actBlkCap = (aggr->Result.s.RequestLength * 8)/sizeof(STL_SCVLMAP);
		if (blkCount > actBlkCap) {
			IB_LOG_ERROR_FMT(__func__,
				"Amod block count exceeds payload block capacity.  Amod block count : %d; payload block capacity : %ld",
				blkCount, actBlkCap);
			return VSTATUS_BAD;
		}

		// swap & zero data in advance
		uint8 i, j;
		for (i = 0; i < blkCount; ++i) {
			BSWAP_STL_SCVLMAP(&((STL_SCVLMAP*)aggr->Data)[i]);
		}

		/*
			Following code requires that all ports in a multiblock range were valid when the request was composed,
			otherwise the requester shouldn't compose a multiblock request that includes ports that weren't valid.
		*/
		Port_t * portp = NULL;
		for (i = startPort, j = 0; i < endPort; ++i, j = (j + 1) % blkCount) {
			portp = sm_get_port(nodep, i);

			if (!sm_valid_port(portp) || (portp->state <=IB_PORT_DOWN))
				continue;

			switch (aggr->AttributeID) {
				case STL_MCLASS_ATTRIB_ID_SC_VLT_MAPPING_TABLE:
					memcpy(&portp->portData->scvltMap, &((STL_SCVLMAP*)aggr->Data)[j], sizeof(STL_SCVLMAP));
					portp->portData->current.scvlt = 1;

					break;
				case STL_MCLASS_ATTRIB_ID_SC_VLNT_MAPPING_TABLE:
					memcpy(&portp->portData->scvlntMap, &((STL_SCVLMAP*)aggr->Data)[j], sizeof(STL_SCVLMAP));
					portp->portData->current.scvlnt = 1;

					break;
			}
		}
	}

	return VSTATUS_OK;
}

static Status_t
sm_aggregateToVlarb(Node_t * nodep, STL_AGGREGATE * aggr)
{
	// It's ok to have an error, just means that nothing will get updated
	if (aggr->Result.s.Error)
		return VSTATUS_OK;

	uint32_t amod = aggr->AttributeModifier;
	uint8_t blkCount, section, startPort;
	blkCount = (uint8_t)(amod >> 24);
	section = (uint8_t)(amod >> 16);
	startPort = (uint8_t)amod;

	size_t respSize = aggr->Result.s.RequestLength * 8;

	if ((blkCount * sizeof(STL_VLARB_TABLE)) > respSize)
		return VSTATUS_BAD;

	STL_VLARB_TABLE * resp = (STL_VLARB_TABLE*)aggr->Data;

	uint8_t i;
	for (i = 0; i < blkCount; ++i) {
		Port_t * portp = sm_get_port(nodep, i + startPort);

		if (!sm_valid_port(portp) || (portp->state <= IB_PORT_DOWN))
			continue;

		struct _PortDataVLArb * arbp = &portp->portData->curArb;
		uint8_t * dest = NULL;
		size_t cpySize = 0;
		switch (section) {
			case STL_VLARB_LOW_ELEMENTS:
				dest = (uint8_t*)arbp->vlarbLow;
				cpySize = sizeof(arbp->vlarbLow);
				break;
			case STL_VLARB_HIGH_ELEMENTS:
				dest = (uint8_t*)arbp->vlarbHigh;
				cpySize = sizeof(arbp->vlarbHigh);
				break;
			case STL_VLARB_PREEMPT_ELEMENTS:
				dest = (uint8_t*)arbp->vlarbPre;
				cpySize = sizeof(arbp->vlarbPre);
				break;
			case STL_VLARB_PREEMPT_MATRIX:
				dest = (uint8_t*)arbp->vlarbMatrix;
				cpySize = sizeof(arbp->vlarbMatrix);
				break;
			default:
				return VSTATUS_BAD;
		}

		BSWAP_STL_VLARB_TABLE(&resp[i], section);

		memcpy(dest, (uint8_t*)(&resp[i]), cpySize);

		switch (section) {
			case STL_VLARB_LOW_ELEMENTS:
				portp->portData->current.vlarbLow = 1;
				break;
			case STL_VLARB_HIGH_ELEMENTS:
				portp->portData->current.vlarbHigh = 1;
				break;
			case STL_VLARB_PREEMPT_ELEMENTS:
				portp->portData->current.vlarbPre = 1;
				break;
			case STL_VLARB_PREEMPT_MATRIX:
				portp->portData->current.vlarbMatrix = 1;
				break;
		}
	}

	return VSTATUS_OK;
}

Status_t
sm_initialize_VLArbitration(Topology_t * topop, Node_t * nodep, Port_t * portp)
{
//  uint8_t *path;
	uint32_t amod;
	Status_t status = VSTATUS_OK;
	uint16_t dlid, numPorts = 1;
	uint32_t dataSize = 0;

	IB_ENTER(__func__, topop, nodep, portp, 0);

	/* Note: If node was previously non-responding. Don't bother going any further. */
	if (nodep->nonRespCount) {
		IB_EXIT(__func__, 0);
		return (VSTATUS_OK);
	}

	if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {	// Port is down
		IB_EXIT(__func__, 1);
		return (VSTATUS_OK);
	}

	if ((portp->index == 0) &&	// Not a physical port
		!nodep->switchInfo.u2.s.EnhancedPort0) {
		IB_EXIT(__func__, 2);
		return (VSTATUS_OK);
	}

	if (portp->portData->vl0 == 1) {	// Only 1 VL supported
		IB_EXIT(__func__, 3);
		return (VSTATUS_OK);
	}
                                    
//  path = PathToPort(nodep, portp);

	if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
		Port_t *swportp;
		swportp = sm_get_port(nodep, 0);
		if (!sm_valid_port(swportp)) {
			IB_LOG_WARN_FMT(__func__,
							"Failed to get Port 0 of Switch " FMT_U64,
							nodep->nodeInfo.NodeGUID);
			return VSTATUS_BAD;
		}
		dlid = swportp->portData->lid;
	} else {
		dlid = portp->portData->lid;
	}

	struct _PortDataVLArb * arbp = sm_port_getNewArb(portp);

	status = topop->routingModule->funcs.fill_stl_vlarb_table(topop, nodep, portp, arbp);

	if (status != VSTATUS_OK) {
		IB_LOG_WARNRC("Failed to get VL Arbitration "
			"data from routing algorithm; using default; rc:", status);

		sm_FillVlarbTableDefault(portp, arbp);
	}

	// 
	// High priority table.
	// 
	if (!portp->portData->current.vlarbHigh || !portp->portData->current.vlarbLow || !portp->portData->current.vlarbMatrix) {
		IB_LOG_WARN_FMT(__func__,
			"VLArb information is not current on node %s nodeGUID "FMT_U64" port %d",
			sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, portp->index);
	}

	amod = (numPorts << 24) | (STL_VLARB_HIGH_ELEMENTS << 16) | portp->index;

	dataSize = MIN(portp->portData->portInfo.VL.ArbitrationHighCap * sizeof(STL_VLARB_TABLE_ELEMENT), sizeof(portp->portData->curArb.vlarbHigh));
	if (!portp->portData->current.vlarbHigh ||
		memcmp(portp->portData->curArb.vlarbHigh, arbp->vlarbHigh,
			dataSize) != 0 || sm_config.forceAttributeRewrite) {
		status = SM_Set_VLArbitration_LR(fd_topology, amod, sm_lid, dlid, (STL_VLARB_TABLE *)arbp->vlarbHigh, sizeof(arbp->vlarbHigh), sm_config.mkey);
	
		if (status != VSTATUS_OK) {
			IB_LOG_ERROR_FMT(__func__,
							 "SET of VL Arbitration high priority table has failed for node %s guid "
							 FMT_U64 " status=%d", sm_nodeDescString(nodep),
							 nodep->nodeInfo.NodeGUID, status);
		}
		portp->portData->current.vlarbHigh = (status == VSTATUS_OK);
	}

	memcpy(portp->portData->curArb.vlarbHigh, arbp->vlarbHigh, dataSize);

	/* 
	 *  Low priority table.
	 */
	amod = (numPorts << 24) | (STL_VLARB_LOW_ELEMENTS << 16) | portp->index;

	dataSize = MIN(portp->portData->portInfo.VL.ArbitrationLowCap * sizeof(STL_VLARB_TABLE_ELEMENT), sizeof(portp->portData->curArb.vlarbLow));
	if (!portp->portData->current.vlarbLow ||
		memcmp(portp->portData->curArb.vlarbLow, arbp->vlarbLow,
			dataSize) != 0 || sm_config.forceAttributeRewrite) {
			status = SM_Set_VLArbitration_LR(fd_topology, amod, sm_lid, dlid, (STL_VLARB_TABLE*) arbp->vlarbLow, sizeof(arbp->vlarbLow), sm_config.mkey);

		if (status != VSTATUS_OK) {
			IB_LOG_ERROR_FMT(__func__,
							 "SET of VL Arbitration low priority table has failed for node %s guid "
							 FMT_U64 " status=%d", sm_nodeDescString(nodep),
							 nodep->nodeInfo.NodeGUID, status);
		}
		portp->portData->current.vlarbLow = (status == VSTATUS_OK);
	}

	memcpy(portp->portData->curArb.vlarbLow, arbp->vlarbLow, dataSize);

	/* 
	 *  Preemption table - we never set this, but we should retain what the device has stored
	 *  for SA queries.
	 */
	memcpy(portp->portData->curArb.vlarbPre, arbp->vlarbPre, sizeof(arbp->vlarbPre));

	/* 
	 *  Preemption Matrix.
	 */
	if (portp->portData->portInfo.FlitControl.Interleave.s.MaxNestLevelTxEnabled != 0) {
		amod = (numPorts << 24) | (STL_VLARB_PREEMPT_MATRIX << 16) | portp->index;

		if (!portp->portData->current.vlarbMatrix ||
			memcmp(portp->portData->curArb.vlarbMatrix, arbp->vlarbMatrix,
				sizeof(portp->portData->curArb.vlarbMatrix)) != 0 || sm_config.forceAttributeRewrite) {
			status = SM_Set_VLArbitration_LR(fd_topology, amod, sm_lid, dlid, (STL_VLARB_TABLE*) arbp->vlarbMatrix, sizeof(arbp->vlarbMatrix), sm_config.mkey);
			
			if (status != VSTATUS_OK) {
				IB_LOG_ERROR_FMT(__func__,
								 "SET of VL Arbitration Matrix has failed for node %s guid "
								 FMT_U64 " status=%d", sm_nodeDescString(nodep),
								 nodep->nodeInfo.NodeGUID, status);
			}
			portp->portData->current.vlarbMatrix = (status == VSTATUS_OK);
		}

		memcpy(portp->portData->curArb.vlarbMatrix, arbp->vlarbMatrix, sizeof(arbp->vlarbMatrix));
	}

	// Whether things succeeded or not, have no use for newArb anymore
	sm_port_releaseNewArb(portp);

	IB_EXIT(__func__, status);
	return (status);
}

// We need to set the VLARB tables with equal cost slices.
// If minimum BW is 10% or a multiple of 10%, fits nicely
// into table.  
// If minimum BW is 5% and we had 20 entry arb table, this
// would return 5.  STL1 has 16 entries, not enough room 
// in vlarb table for 5% increments so adjustments are made.
static void
setWeightMultiplier(Qos_t * qos)
{
	int currentVl;
	int currentVlBw;
	uint8_t minBandwidth = 0xff;
	uint8_t maxBandwidth = 0;
	int tbase = 1;
	bitset_t* vlsInUse = &qos->lowPriorityVLs;

	// set default
	qos->weightMultiplier = 100;

	if (vlsInUse->nset_m <= 1) {
		return;
	}

	for (currentVl = bitset_find_first_one(vlsInUse); currentVl > -1;
		 currentVl = bitset_find_next_one(vlsInUse, currentVl + 1)) {
		if (qos->vlBandwidth[currentVl] > 0) {
			currentVlBw = qos->vlBandwidth[currentVl];
			if (minBandwidth > currentVlBw) {
				minBandwidth = currentVlBw;
			}
			if (maxBandwidth < currentVlBw) {
				maxBandwidth = currentVlBw;
			}
			if ((currentVlBw % 10) > 2)
				// not base 10 (may be rounded down)
				tbase = 0;
		}
	}

	if (tbase) {
		// base 10 fits in table without adjustments
		if ((minBandwidth != 100) &&
			(minBandwidth != 50)) {
			for (currentVl = bitset_find_first_one(vlsInUse); currentVl > -1;
			 	currentVl = bitset_find_next_one(vlsInUse, currentVl + 1)) {
				if (qos->vlBandwidth[currentVl] > 0) {
					currentVlBw = qos->vlBandwidth[currentVl];
					if (currentVlBw % minBandwidth) {
						minBandwidth = 10;
						break;
					}
				}
			}
		}
		qos->weightMultiplier = minBandwidth;
		return;
	}

	if (minBandwidth > 5) {
		// 5% doesn't fit into table as neatly as 10% increments
		// Make sure we have lowest bw instead of returning 5
		for (currentVl = bitset_find_first_one(vlsInUse); currentVl > -1;
			 currentVl = bitset_find_next_one(vlsInUse, currentVl + 1)) {
			if (qos->vlBandwidth[currentVl] > 0) {
				currentVlBw = qos->vlBandwidth[currentVl];
				if (currentVlBw % minBandwidth) {
					// not neatly devisable, start at 5%
					minBandwidth = 5;
					break;
				}
			}
		}
	} else
		minBandwidth = 5;

	qos->weightMultiplier = minBandwidth;
}
// This method sets a VLARB entry.
//
// twoSlicesPerSlot:
// 	For 5% BW slices, 16 entry table will not hold all entries.
// 	In this case, where total slices exceeds table, the 10% slices
// 	get weight*2.  Any 5% increment will get weight.  While this is
// 	not an equal slice, it is a best effort.
// bwFitsInTable:
//  This table is based on 16 entry STL HW. If the number of slices 
//  required does not fit into STL table, will give equal cost slices.
static void
SetVlarbEntry(STL_VLARB_TABLE_ELEMENT * vlblockp, uint8_t vl, int* entry,
	int weight, int bwFitsInTable, int twoSlicesPerSlot, 
	uint8_t* vlSlices, uint8_t* vlSlots, uint16_t* totalSlots)
{

	vlblockp[*entry].s.VL = vl;

	if (!twoSlicesPerSlot) {
		vlblockp[*entry].Weight = weight;
		vlSlices[vl]--;
		vlSlots[vl]--;
		*totalSlots = *totalSlots-1;

	} else if (bwFitsInTable) {
		// bw fits in table if we double up the slices.
		// If one slot left, give it weight, otherwise double.
		if (vlSlices[vl] == 1) {
			// 5% remainder gets weight
			vlblockp[*entry].Weight = weight;
			vlSlices[vl]--;
			vlSlots[vl]--;
			*totalSlots = *totalSlots-1;
		} else {
			// 10% slot, decrement by two since slices are 5%
			if (weight*2 > 255) {
				vlblockp[*entry].Weight = 255;
			} else {
				vlblockp[*entry].Weight = weight*2;
			}
			vlSlices[vl]-=2;
			vlSlots[vl]--;
			*totalSlots = *totalSlots-1;
		}
	} else {
		// Just give equal weight to all VLs.
		vlblockp[*entry].Weight = weight;
		vlSlices[vl] = 0;
		*totalSlots -= vlSlots[vl];
		vlSlots[vl] = 0;
	}
	*entry = *entry + 1;
}

// It is best for arbitration to use multiple equal size weight slices in a round-robin
// assignment than a single large slice per VL.
// This code is optimized for VL arb table with 16 entries or greater.
static void
FillLowRR(Node_t * nodep, Port_t * portp, STL_VLARB_TABLE_ELEMENT * vlblockp, Qos_t * qos, int weight)
{
	int currentEntry;
	uint8_t vlSlices[STL_MAX_VLS] = {0};
	uint8_t vlSlots[STL_MAX_VLS] = {0};
	int i, interleave = 0;
	int currentVl;
	int highbw = -1;
	uint16_t totalSlices = 0;
	uint16_t totalSlots = 0;
	int bwFitsInTable = 1;
	bitset_t vlsInUse;
	bitset_t highbwVLs;

	if (!bitset_init(&sm_pool, &highbwVLs, STL_MAX_VLS)) {
		IB_FATAL_ERROR("QosFillVlarbTable: No memory for QoS setup, exiting.");
	}

	if (!bitset_init(&sm_pool, &vlsInUse, STL_MAX_VLS)) {
		IB_FATAL_ERROR("QosFillVlarbTable: No memory for QoS setup, exiting.");
	}

	bitset_copy(&vlsInUse, &qos->lowPriorityVLs);

	for (currentVl = bitset_find_first_one(&vlsInUse); currentVl >= 0;
		 currentVl = bitset_find_next_one(&vlsInUse, currentVl + 1)) {

		vlSlices[currentVl] = qos->vlBandwidth[currentVl] / qos->weightMultiplier;
		if ((qos->vlBandwidth[currentVl] - (vlSlices[currentVl] * qos->weightMultiplier)) >=
			(qos->weightMultiplier / 2)) {
			vlSlices[currentVl] += 1;
		}
		if (vlSlices[currentVl] <= 0)
			vlSlices[currentVl] = 1;

		// interleave large bw group so not duplicated at end of table.  
		if (qos->vlBandwidth[currentVl] > highbw) {
			if (highbw >= 0) {
				bitset_clear_all(&highbwVLs);
			}
			highbw = qos->vlBandwidth[currentVl];
			bitset_set(&highbwVLs, currentVl);
		} else if (qos->vlBandwidth[currentVl] == highbw) {
			bitset_set(&highbwVLs, currentVl);
		}

		totalSlices += vlSlices[currentVl];

		if (qos->weightMultiplier == 5) {
			vlSlots[currentVl] = vlSlices[currentVl]/2;
			if (vlSlices[currentVl] % 2)
				vlSlots[currentVl]++;
		} else {
			vlSlots[currentVl] += vlSlices[currentVl];
		}
		totalSlots += vlSlots[currentVl];
	}

	if (totalSlices > portp->portData->portInfo.VL.ArbitrationHighCap) {
		if ((totalSlices / 2) > portp->portData->portInfo.VL.ArbitrationHighCap) {
			// Unexpected device (less than 16 vlarb entries) and BW configured with
			// too small of an increment.  Disable BW and log a warning.  Will allow
			// config to run, but QoS BW disbled on this device port.
            IB_LOG_WARN_FMT(__func__,
				"Disabling QoS BW allocation for node %s guid "
				FMT_U64", VLARB table is too small for this configuration",
				sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID);
			bwFitsInTable = 0;
		}
	}

	for (i = bitset_find_first_one(&highbwVLs); i>=0; 
		 i = bitset_find_next_one(&highbwVLs, i+1)) {
		bitset_clear(&vlsInUse, i);
	}

	currentVl = bitset_find_first_one(&vlsInUse);
	currentEntry = 0;

	while (currentEntry < portp->portData->portInfo.VL.ArbitrationHighCap) {
		interleave = 0;
		for (i = bitset_find_first_one(&highbwVLs); i>=0; 
		 	 i = bitset_find_next_one(&highbwVLs, i+1)) {

			if (!interleave) {
				interleave = (totalSlots - (bitset_nset(&highbwVLs)*vlSlots[i])) / vlSlots[i];
			}

			if (vlSlices[i] == 0)
				break;

			if (currentEntry >= portp->portData->portInfo.VL.ArbitrationHighCap)
				break;

			SetVlarbEntry(vlblockp, i, &currentEntry, weight, bwFitsInTable,
						  (totalSlices > portp->portData->portInfo.VL.ArbitrationHighCap),
						  vlSlices, vlSlots, &totalSlots);

			if (!vlSlices[i]) {
				bitset_clear(&highbwVLs, i);
			}
		}

		for (i=0; i<interleave; i++) {
			if (currentVl < 0 && bitset_nset(&highbwVLs)== 0)
				break;

			if (currentVl < 0)
				continue;

			if (currentEntry >= portp->portData->portInfo.VL.ArbitrationHighCap)
				break;

			SetVlarbEntry(vlblockp, currentVl, &currentEntry, weight, bwFitsInTable,
						  (totalSlices > portp->portData->portInfo.VL.ArbitrationHighCap),
						  vlSlices, vlSlots, &totalSlots);

			if (!vlSlices[currentVl]) {
				bitset_clear(&vlsInUse, currentVl);
			}

			currentVl = bitset_find_next_one(&vlsInUse, currentVl + 1);
			if (currentVl < 0) {
				currentVl = bitset_find_first_one(&vlsInUse);
			}
		}
		if (bitset_nset(&highbwVLs) == 0) {
			if (totalSlots) {
				interleave = totalSlots;
			} else {
				 break;
			}
		}
	}

	bitset_free(&vlsInUse);
	bitset_free(&highbwVLs);
}

Status_t
QosFillVlarbTable(Topology_t * topop, Node_t * nodep, Port_t * portp, Qos_t * qos, struct _PortDataVLArb * arbp)
{

	int currentVl;
	STL_VLARB_TABLE_ELEMENT *vlblockp;
	uint8_t currentEntry;
	int weight = 0;

	memset(arbp->vlarbLow, 0, sizeof(arbp->vlarbLow));
	memset(arbp->vlarbHigh, 0, sizeof(arbp->vlarbHigh));
	memset(arbp->vlarbPre, 0, sizeof(arbp->vlarbPre));

	weight = Decode_MTU_To_Int(portp->portData->mtuActive) / 64;

	// Setup high priority table.
	// No bandwidth associated with high priority.
	if (qos->highPriorityVLs.nset_m) {
		vlblockp = arbp->vlarbHigh;
		currentVl = bitset_find_first_one(&qos->highPriorityVLs);

		for (currentEntry = 0; currentEntry < portp->portData->portInfo.VL.ArbitrationHighCap;
			 currentEntry++) {
			if (currentVl < 0)
				break;

			vlblockp[currentEntry].s.VL = currentVl;
			vlblockp[currentEntry].Weight = weight;

			currentVl = bitset_find_next_one(&qos->highPriorityVLs, currentVl + 1);
		}
	}

	// Setup low priority BW table
	if (qos->lowPriorityVLs.nset_m) {
		vlblockp = arbp->vlarbLow;
		FillLowRR(nodep, portp, vlblockp, qos, weight);
	}

	return VSTATUS_OK;
}

Status_t
QosFillStlVlarbTable(Topology_t * topop, Node_t * nodep, Port_t * portp, Qos_t * qos, struct _PortDataVLArb * arbp)
{
	// Use the previous IB init logic for now
	return QosFillVlarbTable(topop, nodep, portp, qos, arbp);
}

#if 0
// TEST
void dumpit(int32_t memSize, int16_t * pbw, uint8_t* mtu, int32_t wd, int32_t au, STL_BUFFER_CONTROL_TABLE * pBfrCtrl)
{
    int j, total= 0;
    //printf("Port %d: AU: %d\n",0, au);
    printf("  RxMem(AU/kB):      %6d/%6d\n", memSize, ((memSize*au)/1024));
    printf("  WireDepth(AU/B):   %6d/%6d\n", wd, (wd*au));
    printf("  Tot Shared(AU/kB): %6d/%6d\n", pBfrCtrl->TxOverallSharedLimit, ((pBfrCtrl->TxOverallSharedLimit*au)/1024));

    printf("\t");         for (j=0; j < 16; j++) { printf("VL%-4d", j);}
    printf("\n\t");       for (j=16;j < 32; j++) { printf("VL%-4d", j);}
    printf("\n  QOS\t");  for (j=0; j < 16; j++) { printf("%-6d", pbw[j]);}
    printf("\n\t");       for (j=16;j < 32; j++) { printf("%-6d", pbw[j]);}
    printf("\n  MTU\t");  for (j=0; j < 16; j++) { printf("%-6d", mtu[j]);}
    printf("\n\t");       for (j=16;j < 32; j++) { printf("%-6d", mtu[j]);}
    printf("\n  DED\t");  for (j=0; j < 16; j++) { printf("%-6d", pBfrCtrl->VL[j].TxDedicatedLimit); total+=pBfrCtrl->VL[j].TxDedicatedLimit; }
    printf("\n\t");       for (j=16;j < 32; j++) { printf("%-6d", pBfrCtrl->VL[j].TxDedicatedLimit); total+=pBfrCtrl->VL[j].TxDedicatedLimit; }
    printf("\n  SHRD\t"); for (j=0; j < 16; j++) { printf("%-6d", pBfrCtrl->VL[j].TxSharedLimit);}
    printf("\n\t");       for (j=16;j < 32; j++) { printf("%-6d", pBfrCtrl->VL[j].TxSharedLimit);}

    printf("\n");

    // Checks 
    if (pBfrCtrl->TxOverallSharedLimit+total != memSize) {
        printf("!!! ERROR: Sizes / Totals  do not match\n");
        printf("   TxOverallSharedLimit:  %d\n",pBfrCtrl->TxOverallSharedLimit);
        printf("   Dedicated Totals:      %d\n",total);
        printf("   Rx Memory Size :       %d\n",memSize);
    }
    printf("-------------------------\n");
}
#endif 


#define PROTOCOL_HEADER_SIZE 128

static int bwCompare(const void * a, const void * b) { return (*(int16_t*)a - *(int16_t*)b);}

static Status_t 
setupBufferControl(int32_t memSize, int16_t * pbw, uint8_t* pmtu, int32_t wd, int32_t au,
                   bool_t shmem, STL_BUFFER_CONTROL_TABLE * pBfrCtrl) 
{
    int       i, j;
    int32_t   intervalSize;
    int32_t   remainingSize;
    int32_t   packetSize[STL_MAX_VLS];
    int16_t   rankBw[STL_MAX_VLS];
    int32_t   header = PROTOCOL_HEADER_SIZE;
    int32_t   maxNoDed;
    uint8_t   mult = (uint8_t)sm_config.dedicatedVLMemMulti;  
    int32_t   minShared; 

    enum RULES {
        RULE_A = 1,
        RULE_B = 2, 
        RULE_C = 3
    } applied;

	// If this port does not support shared memory,
	// Ensure use of dedicated memory 
    if ((shmem==FALSE) && (mult==0)) {
		mult = 1;
    }

    // Ensure consistency of requests.
    minShared = (sm_config.minSharedVLMem * memSize)/100;
    if ((minShared>0) && (shmem==FALSE)) {
        minShared=0;
    }

    for (i=0; i<STL_MAX_VLS; i++) {
        // Initialize VLs buffer space to 0
        pBfrCtrl->VL[i].TxDedicatedLimit=0;
        pBfrCtrl->VL[i].TxSharedLimit=0;

        // Initialize the packet sizes.
		// Do everything in terms of au's
        // Always ensure dedicated buffer space for VL15
		if (i == 15) {
            packetSize[i] = (2048+header)*2;
            packetSize[i] = (packetSize[i] + au - 1)/au; /* round-up */

            // check for unexpected limitations
            //  which prevent even VL15 from having dedicated memory
            if (memSize-minShared-packetSize[i]-wd<0) {
                if (minShared > (packetSize[i] + wd)) {
					minShared -= (packetSize[i] + wd); 
                } else {
					minShared = 0;
				}
            }
		} else if (pbw[i]>0) {
            packetSize[i] = (GetBytesFromMtu(pmtu[i])+header)*mult; 
            packetSize[i] = (packetSize[i] + au - 1)/au; /* round-up */
        } else {
            packetSize[i] = 0;
        }
    }


    // Attempt Rule A: allocate a packet-size plus wire depth 
    remainingSize = memSize-minShared;
    intervalSize = 1;
    applied = RULE_A;
    for (i=0; i<STL_MAX_VLS; i++) {
        if (packetSize[i]>0) {
            remainingSize -= (packetSize[i]+wd);
            if (remainingSize < 0) break;

            intervalSize  += packetSize[i];
            pBfrCtrl->VL[i].TxDedicatedLimit = packetSize[i]+wd; 
        }
    }

    // If no shared memory, apply RULE_A iteratively until all memory consumed.
    intervalSize = remainingSize/intervalSize;
    if ((shmem==FALSE) && (intervalSize >0)) {
        for (i=0; i<STL_MAX_VLS; i++) {
            if (packetSize[i]>0) {
                pBfrCtrl->VL[i].TxDedicatedLimit+=packetSize[i]*intervalSize; 
            }
        }
    }

    // Rank the QOS for subsequent rules
    if (remainingSize < 0) {
        memcpy(rankBw, pbw, sizeof(rankBw));
        qsort(rankBw, STL_MAX_VLS, sizeof(int16_t), bwCompare); 
    }

    // Attempt Rule B
	// (only applicable if Wire Depth is non-zero, otherwise result is rule A)
    if ((remainingSize < 0) && (wd > 0)){
        applied = RULE_B;

        for (i=0; i<STL_MAX_VLS;i++) {
            // optimize for duplicate rank values.
            if ((i<STL_MAX_VLS-1) && (rankBw[i]==rankBw[i+1])) continue;

            remainingSize = memSize-minShared;

            for (j=0; j<STL_MAX_VLS;j++) {
                if (pbw[j]>rankBw[i] || j == 15) {
                    remainingSize -= (packetSize[j]+wd);
                    pBfrCtrl->VL[j].TxDedicatedLimit = packetSize[j]+wd; 
                } else {
                    remainingSize -= packetSize[j];
                    pBfrCtrl->VL[j].TxDedicatedLimit = packetSize[j]; 
                }
                if (remainingSize<0) break; 
            }

            if (remainingSize>=0) break; 
        }
    }

    // Attempt Rule C (must work)
    if (remainingSize < 0) {
        applied = RULE_C;

        for (i=0; i<STL_MAX_VLS;i++) {
            // optimize for duplicate rank values.
            if ((i<STL_MAX_VLS-1) && (rankBw[i]==rankBw[i+1])) continue;

            maxNoDed = 0;
            remainingSize = memSize-minShared;

            for (j=0; j<STL_MAX_VLS;j++) {
                if (pbw[j]>rankBw[i] || j == 15) {
                    remainingSize -= (packetSize[j]+wd);
                    if (remainingSize<0) break; 

                    pBfrCtrl->VL[j].TxDedicatedLimit = packetSize[j]+wd; 
                } else {
                    maxNoDed = MAX(maxNoDed, (packetSize[j]+wd));
                    pBfrCtrl->VL[j].TxDedicatedLimit = 0;
                }
            }

            if (remainingSize>=0) {
                if (shmem==FALSE)
                    break;
                else if (remainingSize+minShared >= maxNoDed)
                     break; 
            }
        }
    }

    if (shmem==TRUE) {
        remainingSize+=minShared;
    } else {
        remainingSize=0;
    }
    pBfrCtrl->TxOverallSharedLimit = remainingSize;

    for (i = 0; i < STL_MAX_VLS; i++) {
        if (i == 15 || pbw[i]>0) {
            if (applied == RULE_C) {
                // In worst case scenarios, only allow VL to shared if dedicated is 0.
                if (pBfrCtrl->VL[i].TxDedicatedLimit==0) {
                    pBfrCtrl->VL[i].TxSharedLimit=remainingSize; 
                } 
            } else {
                pBfrCtrl->VL[i].TxSharedLimit=remainingSize; 
            }
        }
    }

    // Rule C works, but it might prevent forward progress.
    // We need to alert the user of this condition, but do not have
    //  meaningful information to put in the log / error messsage.
    // Return an error here so that caller can create appropriate log msg.
    if (applied == RULE_C) {
        return (VSTATUS_BAD);
    }

    return (VSTATUS_OK);
}

Status_t
sm_initialize_Port_BfrCtrl(Topology_t * topop, Node_t * nodep, Port_t * portp,
							STL_BUFFER_CONTROL_TABLE *bct)
{
    Port_t*     neighborPort = 0;
    Node_t*     neighborNode = 0;
    int32_t     rxMemSize;
    int32_t     wd;
    int32_t     au;
    bool_t      shmem;
	int16_t     bw[STL_MAX_VLS];
    uint8_t     mtu[STL_MAX_VLS];
    int         vf, vl, i;
	VlVfMap_t   vlvfmap;
	VirtualFabrics_t *VirtualFabrics = topop->vfs_ptr;
    
    // Find the neighbor port (Get neighbors recv buffer size and allocation units)
    neighborPort = sm_find_neighbor_node_and_port(topop, portp, &neighborNode);
    if (neighborPort==NULL) {
        IB_LOG_ERROR_FMT(__func__,
                         "Unable to find neighbor port for node %s guid "
                         FMT_U64 " Port number=%d", sm_nodeDescString(nodep),
                         nodep->nodeInfo.NodeGUID, portp->index);
        return (VSTATUS_BAD);
    }

    au=8*(1 << neighborPort->portData->portInfo.BufferUnits.s.BufferAlloc);
    shmem = neighborPort->portData->portInfo.CapabilityMask3.s.IsSharedSpaceSupported;

    rxMemSize = neighborPort->portData->portInfo.OverallBufferSpace;
    if (rxMemSize==0) {
        IB_LOG_ERROR_FMT(__func__,
                         "Overall Rx Buffer size is zero for node %s guid "
                         FMT_U64 " Port number=%d", sm_nodeDescString(neighborNode),
                         neighborNode->nodeInfo.NodeGUID, neighborPort->index);
        return (VSTATUS_BAD);
    }

	// Override wire depth / replay depths from configuration.
    if ((int32_t)(sm_config.wireDepthOverride) == -1) {

		if ((int32_t)(sm_config.replayDepthOverride) == -1) {
			wd = BYTES_PER_LTP * MIN(portp->portData->portInfo.ReplayDepth.WireDepth, 
									 portp->portData->portInfo.ReplayDepth.BufferDepth);
		} else if (sm_config.replayDepthOverride == 0) {
			wd = portp->portData->portInfo.ReplayDepth.WireDepth * BYTES_PER_LTP;
        }
		else {
			wd = MIN(portp->portData->portInfo.ReplayDepth.WireDepth * BYTES_PER_LTP, 
					 sm_config.replayDepthOverride);
		}

    } else if (sm_config.wireDepthOverride == 0) {

		if ((int32_t)(sm_config.replayDepthOverride) == -1) {
			wd = portp->portData->portInfo.ReplayDepth.BufferDepth * BYTES_PER_LTP;
		} else if (sm_config.replayDepthOverride == 0) {
			wd = 0;
        } else {
			wd = sm_config.replayDepthOverride;
		}

	} else {

		if ((int32_t)(sm_config.replayDepthOverride) == -1) {
			wd = MIN(sm_config.wireDepthOverride, 
					 portp->portData->portInfo.ReplayDepth.BufferDepth * BYTES_PER_LTP);
		} else if (sm_config.replayDepthOverride == 0) {
			wd = sm_config.wireDepthOverride;
        } else {
			wd = MIN(sm_config.wireDepthOverride, sm_config.replayDepthOverride);
		}

	}
	// Convert WD from bytes to AU's (rounding up)
    wd = (wd + au - 1) / au; 

    // Setup BW and MTU per VL based on this ports VL membership in VFs
	topop->routingModule->funcs.select_vlvf_map(topop, nodep, portp, &vlvfmap);

	// Evaluate MTU and QOS for this VL.
    for (vl=0;vl<STL_MAX_VLS;vl++) {
        mtu[vl]=0;
        bw[vl]=0;
        for(i=0;i<MAX_VFABRICS;i++) {
            vf=vlvfmap.vf[vl][i];
            if ((vf<0) || (vf>=MAX_VFABRICS)) break; // Done list.

			mtu[vl]=MAX(mtu[vl], VirtualFabrics->v_fabric[vf].max_mtu_int);
			bw[vl]+= sm_VfBandwidthAllocated[vf];
        }
		mtu[vl] = MIN(mtu[vl], portp->portData->mtuActive);
    }

    // Setup the buffer control map.
    if (setupBufferControl(rxMemSize, bw, mtu,  wd, au, shmem, bct)!=VSTATUS_OK) {
        IB_LOG_ERROR_FMT(__func__,
                         "Errors encountered for setup Buffer Control for node %s guid "
                         FMT_U64 " Port number=%d", sm_nodeDescString(nodep),
                         nodep->nodeInfo.NodeGUID, portp->index);
    }

    //printf("Node Description: %s, port:%d wd:%d TxD: %d\n", 
    //        sm_nodeDescString(nodep),
    //		portp->index,
    //		(portp->portData->portInfo.ReplayDepth.WireDepth * BYTES_PER_LTP),
    //	    (portp->portData->portInfo.ReplayDepth.BufferDepth *BYTES_PER_LTP));
    //dumpit(rxMemSize, bw, mtu, wd, au, bct);

    return VSTATUS_OK;
}

/**
  @return Number of aggregate segments required to send @c n blocks of size @c s per block
*/
static __inline__
size_t stl_ReqAggrSegCount(size_t n, size_t s)
{
  if (s == 0) return 0;
  size_t blksPerSeg = STL_MAX_PAYLOAD_AGGREGATE/s;
  return (n + (blksPerSeg - 1))/blksPerSeg;
}

/**
  @return Amount of memory required to do aggregate send @c n blocks of size @c s per block
*/
static __inline__
size_t stl_ReqAggrSegMem(size_t n, size_t s)
{
  size_t segsReq = stl_ReqAggrSegCount(n, s);
  return segsReq * sizeof(STL_AGGREGATE) + n * s;
}

Status_t
sm_node_updateFields(IBhandle_t fd, uint16_t slid, Node_t * nodep, Port_t * smaportp)
{
    Status_t s = sm_node_updateFromTopo(nodep, &old_topology, sm_topop);

    if (s != VSTATUS_OK)
        return s;

    if (!sm_valid_port(smaportp))
        return VSTATUS_BAD;

    if (nodep->aggregateEnable) {
        s = sm_node_updateFromSma_aggregate(fd, slid, nodep, smaportp);
    }

    // Note that for a multi-port HFI, if one aggregate operation fails and non-aggregate
    // succeeds, aggregates will be disabled for all ports on that HFI
    if (!nodep->aggregateEnable || s != VSTATUS_OK) {
        s = sm_node_updateFromSma_solo(fd, slid, nodep, smaportp);

        // Aggregate update failed but non-aggregate update succeeded, disable aggregates on this node
        if (nodep->aggregateEnable && s == VSTATUS_OK) {
            IB_LOG_WARN_FMT(__func__,
                "Disabling aggregate support on node %s nodeGUID "FMT_U64,
                sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID);
            nodep->aggregateEnable = 0;
        }
    }

    return s;
}

static Status_t
sm_node_updateFromSma_aggregate(IBhandle_t fd, uint16_t slid, Node_t * nodep, Port_t * smaportp)
{
    Status_t s = VSTATUS_BAD;

    uint8 startPort, numPorts;

    startPort = smaportp->index;

    switch (nodep->nodeInfo.NodeType) {
        case NI_TYPE_CA:
            numPorts = 1;
            break;
        case NI_TYPE_SWITCH:
            numPorts = nodep->nodeInfo.NumPorts + 1;
            break;
        default:
            return VSTATUS_BAD;
    }

    if (!sm_valid_port(smaportp)) {
        IB_LOG_ERROR_FMT(__func__,
            "Failed to get SMA port for node %s, node GUID "FMT_U64", port %d",
            sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, smaportp->index);
        return VSTATUS_BAD;
    }

    size_t reqMem =
        (sizeof(STL_AGGREGATE) + sizeof(STL_SLSCMAP)) +
        (sizeof(STL_AGGREGATE) + sizeof(STL_SCSLMAP)) +
        2 * stl_ReqAggrSegMem(numPorts, sizeof(STL_SCVLMAP)) + // 2 * -> SCVLt and SCVLnt

        // Not optimal; in the worst case, can do 2 VLArb blocks/segment
        3 * (numPorts * (sizeof(STL_AGGREGATE) + sizeof(STL_VLARB_TABLE))); // vlarbLow, vlarbHigh, and preempt matrix use the same wire-size structure even though they are not the same size

    STL_AGGREGATE * aggrBuffer;
    vs_pool_alloc(&sm_pool, reqMem, (void*)&aggrBuffer);

    if (!aggrBuffer)
        return VSTATUS_BAD;

    memset(aggrBuffer, 0, reqMem);
    STL_AGGREGATE * segHdr = aggrBuffer;

    if (!smaportp->portData->current.slsc) {
        segHdr->AttributeID = STL_MCLASS_ATTRIB_ID_SL_SC_MAPPING_TABLE;
        segHdr->Result.s.Error = 0;
        segHdr->Result.s.RequestLength = (sizeof(STL_SLSCMAP) + 7)/8;
        segHdr->AttributeModifier = 0;
        segHdr = STL_AGGREGATE_NEXT(segHdr);
    }

    if (!smaportp->portData->current.scsl) {
        segHdr->AttributeID = STL_MCLASS_ATTRIB_ID_SC_SL_MAPPING_TABLE;
        segHdr->Result.s.Error = 0;
        segHdr->Result.s.RequestLength = (sizeof(STL_SCSLMAP) + 7)/8;
        segHdr->AttributeModifier = 0;
        segHdr = STL_AGGREGATE_NEXT(segHdr);
    }

    Port_t * portp = NULL;
    boolean getScvlt = FALSE;
    boolean getScvlnt = FALSE;
    boolean getVlarbLow = FALSE;
    boolean getVlarbHigh = FALSE;
    boolean getVlarbMatrix = FALSE;

    {
        uint8 i;
        for (i = 0; i < numPorts; ++i) {
            portp = sm_get_port(nodep, startPort + i);
            if (!sm_valid_port(portp) || (portp->state <= IB_PORT_DOWN))
                continue;

            getScvlt |= !portp->portData->current.scvlt;
            getScvlnt |= !portp->portData->current.scvlnt;

            if (portp->portData->vl0 > 1) {
                getVlarbLow |= !portp->portData->current.vlarbLow;
                getVlarbHigh |= !portp->portData->current.vlarbHigh;
                getVlarbMatrix |= !portp->portData->current.vlarbMatrix;
            }

            if (getScvlt && getScvlnt && getVlarbLow && getVlarbHigh && getVlarbMatrix)
                break;
        }
    }

    if (getScvlt) {
        // Get(SCVLt)
        size_t i;
        size_t portsPerSeg = STL_MAX_PAYLOAD_AGGREGATE/sizeof(STL_SCVLMAP);
        size_t segsReq = stl_ReqAggrSegCount(numPorts, sizeof(STL_SCVLMAP));
        for (i = 0; i < segsReq; ++i) {
            uint8 port = (i * portsPerSeg) + startPort;
            uint8 blkCount = (segsReq > (i + 1)? portsPerSeg : numPorts % portsPerSeg);

            segHdr->AttributeID = STL_MCLASS_ATTRIB_ID_SC_VLT_MAPPING_TABLE;
            segHdr->Result.s.Error = 0;
            segHdr->Result.s.RequestLength = (blkCount * sizeof(STL_SCVLMAP) + 7)/8;
            segHdr->AttributeModifier = (blkCount << 24) | port;
            segHdr = STL_AGGREGATE_NEXT(segHdr);
        }
    }

    if (getScvlnt) {
        // Copypasta from above getScvlt code FTW
        // Get(SCVLnt)
        size_t i;
        size_t portsPerSeg = STL_MAX_PAYLOAD_AGGREGATE/sizeof(STL_SCVLMAP);
        size_t segsReq = stl_ReqAggrSegCount(numPorts, sizeof(STL_SCVLMAP));
        for (i = 0; i < segsReq; ++i) {
            uint8 port = (i * portsPerSeg) + startPort;
            uint8 blkCount = (segsReq > (i + 1)? portsPerSeg : numPorts % portsPerSeg);

            segHdr->AttributeID = STL_MCLASS_ATTRIB_ID_SC_VLNT_MAPPING_TABLE;
            segHdr->Result.s.Error = 0;
            segHdr->Result.s.RequestLength = (blkCount * sizeof(STL_SCVLMAP) + 7)/8;
            segHdr->AttributeModifier = (blkCount << 24) | port;
            segHdr = STL_AGGREGATE_NEXT(segHdr);
        }
    }

    const int SEC_COUNT = 3;
    boolean getVlarb[] = { getVlarbHigh, getVlarbLow, getVlarbMatrix };
    uint8_t vlarbSec[] = { STL_VLARB_HIGH_ELEMENTS, STL_VLARB_LOW_ELEMENTS, STL_VLARB_PREEMPT_MATRIX };
    int i;
    for (i = 0; i < SEC_COUNT; ++i) {
        if (!getVlarb[i])
            continue;

        size_t blkSize = sizeof(STL_VLARB_TABLE);
        size_t portsPerSeg = STL_MAX_PAYLOAD_AGGREGATE/blkSize;

        assert(portsPerSeg > 0);

        uint8 blkCount = 0;

        Port_t * portp, * END_PORT; // END_PORT is one past the end

        if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
            portp = sm_get_port(nodep, PORT_A0(nodep));
            END_PORT = sm_get_port(nodep, PORT_A1(nodep)) + 1;
        }
        else {
            portp = smaportp;
            END_PORT = smaportp + 1;
        }

        Port_t * endPortp = NULL;

        // SMA will return an error for a Get(VLArb) on a port with only one data VL
        // So have to build request selectively
        while (portp && portp != END_PORT) {
            // Find first port for which we can get vlarb data
            while (portp != END_PORT &&
                (!sm_valid_port(portp) || portp->portData->vl0 <= 1 || portp->state <= IB_PORT_DOWN)) {
                ++portp;
            }

            if (portp == END_PORT)
                break;

            // Find up to blkCount contiguous ports that support more than one data VL
            endPortp = portp;
            while (endPortp != END_PORT && blkCount < portsPerSeg &&
                sm_valid_port(endPortp) && endPortp->portData->vl0 > 1 && portp->state > IB_PORT_DOWN) {
                ++blkCount;
                ++endPortp;
            }

            segHdr->AttributeID = STL_MCLASS_ATTRIB_ID_VL_ARBITRATION;
            segHdr->Result.s.Error = 0;
            segHdr->Result.s.RequestLength = (blkCount * blkSize + 7)/8;
            segHdr->AttributeModifier = (blkCount << 24) | (vlarbSec[i] << 16) | portp->index;
            segHdr = STL_AGGREGATE_NEXT(segHdr);

            blkCount = 0;
            portp = endPortp;
        }
    }

    uint32_t madStatus;
    STL_AGGREGATE * lastSeg = NULL;

    uint16_t dlid = smaportp->portData->lid;
    s = SM_Get_Aggregate_LR(fd, aggrBuffer, segHdr, slid, dlid, &lastSeg, &madStatus);

    if (!lastSeg && s == VSTATUS_OK) {
        s = VSTATUS_BAD;
        goto bail; 
    }

    if (lastSeg) {
        // Can still process partial aggregate response on MAD error
        Status_t tmpStatus = sm_node_handleGetRespAggr(nodep, smaportp, aggrBuffer, lastSeg);
        s = (s != VSTATUS_OK? s : tmpStatus);
    }

    if (s != VSTATUS_OK || madStatus != MAD_STATUS_SUCCESS) {
        IB_LOG_ERROR_FMT(__func__,
            "Error on Get(Aggregate): NodeGUID : "FMT_U64"; NodeDesc : \"%s\"; port : %d; status : %d; madStatus : 0x%02x",
            nodep->nodeInfo.NodeGUID, sm_nodeDescString(nodep), smaportp->index, s, madStatus);
        if (lastSeg) {
            IB_LOG_ERROR_FMT(__func__,
                "First error: AttributeID : 0x%02x; AttributeModifier : 0x%08x",
                lastSeg->AttributeID, lastSeg->AttributeModifier);
        }

        // Set s to BAD if it was an SMA error on a particular segment
        if (s == VSTATUS_OK)
            s = VSTATUS_BAD;
        goto bail;
    }

bail:
    vs_pool_free(&sm_pool, aggrBuffer);

    return s;
}

static Status_t
sm_node_updateFromSma_solo(IBhandle_t fd, uint16_t slid, Node_t * nodep, Port_t * smaportp)
{
    uint32_t amod;
    Status_t s;
    uint8 buffer[STL_MAX_PAYLOAD_SMP_LR];
    uint8 startPort, numPorts;

    startPort = smaportp->index;

    switch (nodep->nodeInfo.NodeType) {
        case NI_TYPE_CA:
            numPorts = 1;
            break;
        case NI_TYPE_SWITCH:
            numPorts = nodep->nodeInfo.NumPorts + 1;
            break;
        default:
            return VSTATUS_BAD;
    }

    if (!sm_valid_port(smaportp)) {
        IB_LOG_ERROR_FMT(__func__,
            "Failed to get SMA port for node %s, node GUID "FMT_U64", port %d",
            sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, smaportp->index);
        return VSTATUS_BAD;
    }

    uint16_t dlid = smaportp->portData->lid;
    s = VSTATUS_OK;

    if (!smaportp->portData->current.slsc) {

        s = SM_Get_SLSCMap_LR(fd, 0, slid, dlid, (STL_SLSCMAP*)buffer);
        if (s != VSTATUS_OK)
            return s;

        smaportp->portData->slscMap = *((STL_SLSCMAP*)buffer);
        smaportp->portData->current.slsc = 1;
    }

    if (!smaportp->portData->current.scsl) {
        // Get(SCSL)
        s = SM_Get_SCSLMap_LR(fd, 0, slid, dlid, (STL_SCSLMAP*)buffer);
        if (s != VSTATUS_OK)
            return s;
        smaportp->portData->scslMap = *((STL_SCSLMAP*)buffer);
        smaportp->portData->current.scsl = 1;
    }

    if ((numPorts * sizeof(STL_SCVLMAP)) > STL_MAX_PAYLOAD_SMP_LR) {
        return VSTATUS_BAD;
    }

    boolean getScvlt = FALSE;
    boolean getScvlnt = FALSE;
    boolean getVlarbLow = FALSE;
    boolean getVlarbHigh = FALSE;
    boolean getVlarbMatrix = FALSE;

    Port_t * portp = NULL;

    {
        uint8 i;
        for (i = 0; i < numPorts; ++i) {
            portp = sm_get_port(nodep, startPort + i);
            if (!sm_valid_port(portp) || (portp->state <= IB_PORT_DOWN))
                continue;

            getScvlt |= !portp->portData->current.scvlt;
            getScvlnt |= !portp->portData->current.scvlnt;

            if (portp->portData->vl0 > 1) {
                getVlarbLow |= !portp->portData->current.vlarbLow;
                getVlarbHigh |= !portp->portData->current.vlarbHigh;
                getVlarbMatrix |= !portp->portData->current.vlarbMatrix;
            }

            if (getScvlt && getScvlnt && getVlarbLow && getVlarbHigh && getVlarbMatrix)
                break;
        }
    }

    if (getScvlt) {
        // Get(SCVLt) - fallback code so get ports individually
        uint8 i;
        for (i = 0; i < numPorts; ++i) {
            portp = sm_get_port(nodep, i + startPort);
            if (!sm_valid_port(portp) || (portp->state <= IB_PORT_DOWN))
                continue;

            amod = (1 << 24) | portp->index;

            s = SM_Get_SCVLtMap_LR(fd, amod, slid, dlid, (STL_SCVLMAP*)buffer);

            if (s != VSTATUS_OK) {
                IB_LOG_ERROR_FMT(__func__,
                    "Failed to get SCVLt for nodeGUID 0x"FMT_U64", port %d",
                    nodep->nodeInfo.NodeGUID, portp->index);
                //TODO: should continue but make sure non-zero status is returned
            }
            else {
                portp->portData->current.scvlt = 1;
                portp->portData->scvltMap = *((STL_SCVLMAP*)buffer);
            }
        }
    }

    if (getScvlnt) {
        // Get(SCVLnt) - fallback code so get ports individually
        uint8 i;
        for (i = 0; i < numPorts; ++i) {
            portp = sm_get_port(nodep, i + startPort);
            if (!sm_valid_port(portp) || (portp->state <= IB_PORT_DOWN))
                continue;

            amod = (1 << 24) | portp->index;

            s = SM_Get_SCVLntMap_LR(fd, amod, slid, dlid, (STL_SCVLMAP*)buffer);

            if (s != VSTATUS_OK) {
                IB_LOG_ERROR_FMT(__func__,
                    "Failed to get SCVLnt for nodeGUID 0x"FMT_U64", port %d",
                    nodep->nodeInfo.NodeGUID, portp->index);
                //TODO: should continue but make sure non-zero status is returned
            }
            else {
                portp->portData->current.scvlnt = 1;
                portp->portData->scvlntMap = *((STL_SCVLMAP*)buffer);
            }
        }
    }

    const int SEC_COUNT = 3;
    boolean getVlarb[] = { getVlarbHigh, getVlarbLow, getVlarbMatrix };
    uint8_t vlarbSec[] = { STL_VLARB_HIGH_ELEMENTS, STL_VLARB_LOW_ELEMENTS, STL_VLARB_PREEMPT_MATRIX };

    int i;
    for (i = 0; i < SEC_COUNT; ++i) {
        if (!getVlarb[i])
            continue;

        // Wire block size is the same for low, high, and preempt matrix even though
        // internal sizes may be different
        size_t blkSize = sizeof(STL_VLARB_TABLE);

        // SMA will return an error on a multiport request if any of the ports do not
        // support more than one data VL.  So hardcode blksPerMad to '1' for now to avoid this problem
        size_t blksPerMad = 1; // = STL_MAX_PAYLOAD_SMP_LR/blkSize

        uint8 j;
        for (j = 0; j < numPorts; ++j) {
            portp = sm_get_port(nodep, j + startPort);

            if (blksPerMad == 1) {
                if (!sm_valid_port(portp) || portp->portData->vl0 <= 1 || portp->state <= IB_PORT_DOWN)
                    continue;
            }

            // May have to do multiple blocks for switches to get VLArb values for all ports
            if (j % blksPerMad == 0) {
                uint8_t blkCount = MIN(numPorts - j, blksPerMad);
                amod = (blkCount << 24) | (vlarbSec[i] << 16) | ( j + startPort);

                s = SM_Get_VLArbitration_LR(fd, amod, slid, dlid, (STL_VLARB_TABLE*)buffer);
                if (s != VSTATUS_OK) {
                    //TODO: should continue but make sure non-OK status is returned
                    return s;
                }
            }

            if (!sm_valid_port(portp) || (portp->state <= IB_PORT_DOWN))
                continue;

            struct _PortDataVLArb * arbp = &portp->portData->curArb;
            uint8_t * dest = NULL;
            size_t cpySize = 0;
            switch (vlarbSec[i]) {
                case STL_VLARB_LOW_ELEMENTS:
                    dest = (uint8_t*)arbp->vlarbLow;
                    cpySize = sizeof(arbp->vlarbLow);
                    break;
                case STL_VLARB_HIGH_ELEMENTS:
                    dest = (uint8_t*)arbp->vlarbHigh;
                    cpySize = sizeof(arbp->vlarbHigh);
                    break;
                case STL_VLARB_PREEMPT_ELEMENTS:
                    dest = (uint8_t*)arbp->vlarbPre;
                    cpySize = sizeof(arbp->vlarbPre);
                    break;
                case STL_VLARB_PREEMPT_MATRIX:
                    dest = (uint8_t*)arbp->vlarbMatrix;
                    cpySize = sizeof(arbp->vlarbMatrix);
                    break;
                default:
                    return VSTATUS_BAD;
            }

            memcpy(dest, ((uint8_t*)buffer) + (j%blksPerMad)*blkSize, cpySize);
            switch (vlarbSec[i]) {
                case STL_VLARB_LOW_ELEMENTS:
                    portp->portData->current.vlarbLow = 1;
                    break;
                case STL_VLARB_HIGH_ELEMENTS:
                    portp->portData->current.vlarbHigh = 1;
                    break;
                case STL_VLARB_PREEMPT_ELEMENTS:
                    portp->portData->current.vlarbPre = 1;
                    break;
                case STL_VLARB_PREEMPT_MATRIX:
                    portp->portData->current.vlarbMatrix = 1;
                    break;
            }
        }
    }

    return s;
}

static Status_t
sm_node_updateFromTopo(Node_t * nodep, Topology_t * oldTopop, Topology_t * curTopop)
{
	if (nodep->nodeInfo.NodeType != NI_TYPE_SWITCH)
		return VSTATUS_OK;

	if (&old_topology != oldTopop)
		return VSTATUS_BAD; // only support copying from old_topology now

	int copyFailed = 0; 

	// If we have a valid topology, and we believe that nothing has
	// changed, iterate through the ports copying data from the old topo to
	// the new one.
	//
	uint32_t skipWrite = (sm_config.skipAttributeWrite & (SM_SKIP_WRITE_MAPS | SM_SKIP_WRITE_VLARB) ? 1 : 0); 
	if ((topology_passcount >= 1) && 
		(!sm_config.forceAttributeRewrite || (sm_config.forceAttributeRewrite == skipWrite)) && 
		(old_topology.qosEnforced == curTopop->qosEnforced) && 
		!nodep->initPorts.nset_m && bitset_test(&old_switchesInUse, nodep->swIdx)) {
		if (nodep->oldExists) {
			Node_t * oldNodep = nodep->old;
			if (oldNodep && !oldNodep->slscChange) {
				Port_t * portp;
				// Already programmed, copy the mappings and return.
				for_all_ports(nodep, portp) {
					if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) 
						continue; 
					
					Port_t * oldPortp = sm_get_port(oldNodep, portp->index); 
					if (!sm_valid_port(oldPortp)) {
						copyFailed = 1; 
						break;
					}
					
					// Copy sl, sc, and vl related mapping tables
					portp->portData->slscMap = oldPortp->portData->slscMap; 
					portp->portData->current.slsc = 1;
					portp->portData->scslMap = oldPortp->portData->scslMap; 
					portp->portData->current.scsl = 1;
					portp->portData->scvltMap = oldPortp->portData->scvltMap; 
					portp->portData->current.scvlt = 1;
					portp->portData->scvlntMap = oldPortp->portData->scvlntMap; 
					portp->portData->current.scvlnt = 1;

					//
					// Copy vlarb
					memcpy(portp->portData->curArb.vlarbLow, oldPortp->portData->curArb.vlarbLow, sizeof(portp->portData->curArb.vlarbLow)); 
					portp->portData->current.vlarbLow = 1;
					memcpy(portp->portData->curArb.vlarbHigh, oldPortp->portData->curArb.vlarbHigh, sizeof(portp->portData->curArb.vlarbHigh)); 
					portp->portData->current.vlarbHigh = 1;
					memcpy(portp->portData->curArb.vlarbPre, oldPortp->portData->curArb.vlarbPre, sizeof(portp->portData->curArb.vlarbPre)); 
					portp->portData->current.vlarbPre = 1;
					memcpy(portp->portData->curArb.vlarbMatrix, oldPortp->portData->curArb.vlarbMatrix, sizeof(portp->portData->curArb.vlarbMatrix));
					portp->portData->current.vlarbMatrix = 1;
				}
			}
		}
	}

	return (!copyFailed? VSTATUS_OK : VSTATUS_BAD);
}

void
sm_FillVlarbTableDefault(Port_t * portp, struct _PortDataVLArb * arb)
{
	uint8_t numVls = portp->portData->vl1;

	int i, currentVl;
	// One entry per data VL, one credit per entry, fill the rest with (15,0)
	for (i = currentVl = 0; i < numVls && currentVl < STL_MAX_LOW_CAP; currentVl++, i++) {
		if (currentVl == 15) currentVl++; // Skip VL15.
		
		arb->vlarbLow[i].s.VL = currentVl;
		arb->vlarbLow[i].Weight = 1;
	}
	for (; i< STL_MAX_LOW_CAP; i++) {
		arb->vlarbLow[i].s.VL = 15;
		arb->vlarbLow[i].Weight = 0;
	}

	// Filling high table with (15,0)
	for (i = 0; i < STL_MAX_LOW_CAP; i++) {
		arb->vlarbHigh[i].s.VL = 15;
		arb->vlarbHigh[i].Weight = 0;
	}

	// Filling pre-empt table with (15,0).
	for (i = 0; i < STL_MAX_PREEMPT_CAP; i++) {
		arb->vlarbPre[i].s.VL = 15;
		arb->vlarbPre[i].Weight = 0;
	}

	memset(arb->vlarbMatrix, 0, sizeof(arb->vlarbMatrix));
}
