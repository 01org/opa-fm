/* BEGIN_ICS_COPYRIGHT5 ****************************************

Copyright (c) 2015-2020, Intel Corporation

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

** END_ICS_COPYRIGHT5   ****************************************/

/* [ICS VERSION STRING: unknown] */

//===========================================================================//
//									     //
// FILE NAME								     //
//    sa_DeviceTree.c							     //
//									     //
// DESCRIPTION								     //
//    This file contains the routines to process the SA requests for 	     //
//    records of the DeviceTreeMember type.				     //
//									     //
// DATA STRUCTURES							     //
//    None								     //
//									     //
// FUNCTIONS								     //
//    sa_DeviceTreeMemberRecord						     //
//									     //
//									     //
//===========================================================================//


#include "os_g.h"
#include "ib_mad.h"
#include "ib_sa.h"
#include "iba/stl_sa_priv.h"
#include "ib_status.h"
#include "cs_g.h"
#include "mai_g.h"
#include "sm_counters.h"
#include "sm_l.h"
#include "sa_l.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

static Status_t
sa_DeviceTreeMemberRecord_GetTable(Mai_t *, uint32_t *);

Status_t
sa_DeviceTreeMemberRecord(Mai_t *maip, sa_cntxt_t* sa_cntxt ) {
	uint32_t			records, recordLength;
	uint16_t			attribOffset;

	IB_ENTER("sa_DeviceTreeMemberRecord", maip, 0, 0, 0);

//
//	Assume failure.
//
	records = 0;
	recordLength = 0;
	attribOffset = 0;

	// Check Method
	if (maip->base.method == SA_CM_GET) {
		INCREMENT_COUNTER(smCounterSaRxGetDtMemberRecord);
	} else if (maip->base.method == SA_CM_GETTABLE) {
		INCREMENT_COUNTER(smCounterSaRxGetTblDtMemberRecord);
	} else {
		// Generate an error response and return.
		maip->base.status = MAD_STATUS_BAD_METHOD;
		IB_LOG_WARN_FMT(__func__, "invalid Method: %s (%u)",
			cs_getMethodText(maip->base.method), maip->base.method);
		(void)sa_send_reply(maip, sa_cntxt);
		IB_EXIT(__func__, VSTATUS_OK);
		return VSTATUS_OK;
	}
	// Check Base and Class Version
	if (maip->base.bversion == STL_BASE_VERSION && maip->base.cversion == STL_SA_CLASS_VERSION) {
		recordLength = sizeof(STL_DEVICE_TREE_MEMBER_RECORD);
		(void)sa_DeviceTreeMemberRecord_GetTable(maip, &records);
	} else {
		// Generate an error response and return.
		maip->base.status = MAD_STATUS_BAD_CLASS;
		IB_LOG_WARN_FMT(__func__, "invalid Base and/or Class Versions: Base %u, Class %u",
			maip->base.bversion, maip->base.cversion);
		(void)sa_send_reply(maip, sa_cntxt);
		IB_EXIT(__func__, VSTATUS_OK);
		return VSTATUS_OK;
	}

	//
	// Determine reply status
	//
	if (maip->base.status != MAD_STATUS_OK) {
		records = 0;
	} else if (records == 0) {
		maip->base.status = MAD_STATUS_SA_NO_RECORDS;
	} else if ((maip->base.method == SA_CM_GET) && (records != 1)) {
		IB_LOG_WARN_FMT(__func__, "sa_DeviceTreeMemberRecord: too many records for SA_CM_GET: %d", records);
		records = 0;
		maip->base.status = MAD_STATUS_SA_TOO_MANY_RECS;
	}

	attribOffset =  recordLength + Calculate_Padding(recordLength);

	/* setup attribute offset for possible RMPP transfer */
	sa_cntxt->attribLen = attribOffset;
	sa_cntxt_data( sa_cntxt, sa_data, records * attribOffset);
	(void)sa_send_reply(maip, sa_cntxt);

	IB_EXIT("sa_DeviceTreeMemberRecord", VSTATUS_OK);
	return(VSTATUS_OK);
}

static Status_t
sa_DeviceTreeMemberRecord_GetTable(Mai_t *maip, uint32_t *records) {
	uint8_t		*data;
	uint32_t	bytes;
	STL_SA_MAD	samad;
	Status_t	status;
	Node_t		*pNode;
	Port_t		*pPort;
	int 		i;
	STL_LID		portLid = 0;
	bool_t          checkLid;
	int 		foundLid = 0;

	STL_DEVICE_TREE_MEMBER_RECORD record = {0};

	IB_ENTER("sa_DeviceTreeMemberRecord_GetTable", maip, *records, 0, 0);

	*records = 0;
	data = sa_data;
	bytes = Calculate_Padding(sizeof(STL_DEVICE_TREE_MEMBER_RECORD));

	//  Verify the size of the data received for the request
	if ( maip->datasize-sizeof(STL_SA_MAD_HEADER) < sizeof(STL_DEVICE_TREE_MEMBER_RECORD) ) {
		IB_LOG_ERROR_FMT(__func__,
			"invalid MAD length; size of STL_DEVICE_TREE_MEMBER_RECORD[%"PRISZT"], datasize[%d]",
			sizeof(STL_DEVICE_TREE_MEMBER_RECORD), (int)(maip->datasize-sizeof(STL_SA_MAD_HEADER)));
		maip->base.status = MAD_STATUS_SA_REQ_INVALID;
		IB_EXIT("sa_DeviceTreeMemberRecord_GetTable", MAD_STATUS_SA_REQ_INVALID);
		return (MAD_STATUS_SA_REQ_INVALID);
	}

	BSWAPCOPY_STL_SA_MAD((STL_SA_MAD*)maip->data, &samad, sizeof(STL_DEVICE_TREE_MEMBER_RECORD));

	status = sa_create_template_mask(maip->base.aid, samad.header.mask);
	if (status != VSTATUS_OK) {
		IB_LOG_WARN_FMT(__func__,
			"sa_create_template_mask failed.  maip->base.aid 0x%x samad.header.mask 0x%"PRIx64,
			maip->base.aid, samad.header.mask);
		IB_EXIT("sa_DeviceTreeMemberRecord_GetTable", VSTATUS_OK);
		return VSTATUS_OK;
	}

	checkLid = samad.header.mask & STL_DEVICE_GROUP_COMPONENTMASK_LID;
	if (checkLid) {
		portLid = ntoh32(((STL_DEVICE_GROUP_MEMBER_RECORD*)(samad.data))->LID);
	}

	(void)vs_rdlock(&old_topology_lock);

	for_all_nodes(&old_topology, pNode) {

		for (i = 0; i < STL_MAX_PORTMASK; i++) {
			record.portMaskAct[i] = 0;
			record.portMaskPortLinkMode[i] = 0;
		}

		record.NodeType = pNode->nodeInfo.NodeType;
		record.NumPorts = pNode->nodeInfo.NumPorts;
		record.SystemImageGUID = pNode->nodeInfo.SystemImageGUID;
		record.NodeDescription = pNode->nodeDesc;

		// Port Specific information should be collected from SWP0, or 
		// FI port 1.
		if (sm_valid_port( pPort = sm_get_port(pNode, PORT_A0(pNode)) )) {
			record.LID = pPort->portData->lid;
			record.GUID = pPort->portData->guid;
		}

		for_all_ports(pNode, pPort) {

			if (!sm_valid_port(pPort)) continue;

			// If we're looking for a matching LID, and this port doesn't match,
			// then stop further processing for this port and search the remaining ports on this node.
			if (checkLid) {
				if (pNode->nodeInfo.NodeType == NI_TYPE_SWITCH){
					if (portLid != pNode->port[0].portData->lid) continue;
				}
				else {
					if (portLid != pPort->portData->lid) continue;
				}
				foundLid = 1;
			}

			// Record the active state of the port.
			if (pPort->portData->portInfo.PortStates.s.PortState == IB_PORT_ACTIVE) {
				StlAddPortToPortMask(record.portMaskAct, pPort->index);
			}


		}

		if ((status = sa_check_len(data, sizeof(STL_DEVICE_TREE_MEMBER_RECORD), bytes)) != VSTATUS_OK) {
			maip->base.status = MAD_STATUS_SA_NO_RESOURCES;
			IB_LOG_ERROR_FMT("sa_DeviceTreeMemberRecord_GetTable", "Reached size limit at %d records", *records);
			goto done;
		}

		BSWAPCOPY_STL_DEVICE_TREE_MEMBER_RECORD(&record, (STL_DEVICE_TREE_MEMBER_RECORD*)data);
		(void)sa_template_test_mask(samad.header.mask, samad.data, &data, sizeof(STL_DEVICE_TREE_MEMBER_RECORD), bytes, records);

		if (foundLid) break;
	}
done:
	(void)vs_rwunlock(&old_topology_lock);

	IB_EXIT("sa_DeviceTreeMemberRecord_GetTable", VSTATUS_OK);
	return VSTATUS_OK;
}
