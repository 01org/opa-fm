/* BEGIN_ICS_COPYRIGHT6 ****************************************

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

** END_ICS_COPYRIGHT6   ****************************************/

#include "ipci.h"

uint64 PciGetMemoryPciPhysAddr(IN PCI_DEVICE *pDev, IN uint8 barNumber)
{
	uint32 addr_l;
	uint32 addr_h = 0;

#if defined(BSP_N450) || defined(BSP_Q7)
	if (! PciReadConfig(pDev, 
			offsetof(IBA_PCI_COMMON_CONFIG,u.type0.BaseAddresses[barNumber]),
			4, &addr_l))
#else
	if (! PciReadConfig32(pDev, 
			offsetof(IBA_PCI_COMMON_CONFIG,u.type0.BaseAddresses[barNumber]),
			&addr_l))
#endif
	{
		return 0;
	}
	if ((addr_l & IBA_PCI_BAR_CNTL_TYPE_MASK) == IBA_PCI_BAR_CNTL_TYPE_64BIT)
	{
		// 64 bit BAR, get high bits
#if defined(BSP_N450) || defined(BSP_Q7)
		if (! PciReadConfig(pDev, 
				offsetof(IBA_PCI_COMMON_CONFIG,u.type0.BaseAddresses[barNumber+1]),
				4, &addr_h))
#else
		if (! PciReadConfig32(pDev, 
				offsetof(IBA_PCI_COMMON_CONFIG,u.type0.BaseAddresses[barNumber+1]),
				&addr_h))
#endif
		{
			return 0;
		}
	}
	return (addr_l & ~IBA_PCI_BAR_CNTL_MASK) | ((uint64)addr_h << 32);
}
