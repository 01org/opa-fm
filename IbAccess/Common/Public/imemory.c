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

#include "imemory.h"
#include "idebug.h"
#include "itimer.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#if defined(MEM_TRACK_ON)
#if defined(VXWORKS)
#include "tickLib.h"
#define TICK tickGet()
#else
#define TICK 0 // TBD need to specify value for Linux and MAC
#endif
#define MEM_TRACK_FTR
#include "imemtrack.h"

// Default number of headers to allocate at a time.
#define MEM_HDR_ALLOC_SIZE	50

MEM_TRACKER	*pMemTracker = NULL;
static uint32 last_reported_allocations;
static uint32 total_allocations;
static uint32 last_reported_secs;
static uint32 current_allocations;
static uint32 current_allocated;
static uint32 max_allocations;
static uint32 max_allocated;

static void MemoryTrackerDereference(MemoryTrackerFileName_t *trk);
static MemoryTrackerFileName_t *MemoryTrackerBuckets[MEMORY_TRACKER_BUCKETS];
#endif	// MEM_TRACK_ON

#ifdef VXWORKS
extern unsigned long long strtoull (const char *__s, char **__endptr, int __base);
extern long long strtoll (const char *__s, char **__endptr, int __base);
#endif

// convert a string to a uint64.  Very similar to strtoull except that
// base=0 implies base 10 or 16, but excludes base 8
// hence allowing leading 0's for base 10.
//
// Also provides easier to use status returns and error checking.
//
// Can also optionally skip trailing whitespace, when skip_trail_whietspace is
// FALSE, trailing whitespace is treated as a FERROR
//
// When endptr is NULL, trailing characters (after optional whitespace) are 
// considered an FERROR.  When endptr is non-NULL, for a FSUCCESS conversion,
// it points to the characters after the optional trailing whitespace.
// Errors:
//	FSUCCESS - successful conversion, *endptr points to 1st char after value
//	FERROR - invalid contents, non-numeric
//	FINVALID_SETTING - value out of range
//	FINVALID_PARAMETER - invalid function arguments (NULL value or str)
FSTATUS StringToUint64(uint64 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	char *end = NULL;
	uint64 temp;

	if (! str || ! value)
		return FINVALID_PARAMETER;
	errno = 0;
	temp = strtoull(str, &end, base?base:10);
	if ( ! base && ! (temp == IB_UINT64_MAX && errno)
		&& (end && temp == 0 && *end == 'x' && end != str)) {
		// try again as base 16
		temp = strtoull(str, &end, 16);
	}
	if ((temp == IB_UINT64_MAX && errno)
		|| (end && end == str)) {
		if (errno == ERANGE)
			return FINVALID_SETTING;
		else
			return FERROR;
	}

	// skip whitespace
	if (end && skip_trail_whitespace) {
		while (isspace(*end)) {
			end++;
		}
	}
	if (endptr)
		*endptr = end;
	else if (end && *end != '\0')
		return FERROR;
	*value = temp;
	return FSUCCESS;
}

FSTATUS StringToUint32(uint32 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	uint64 temp;
	FSTATUS status;

	status = StringToUint64(&temp, str, endptr, base, skip_trail_whitespace);
	if (status != FSUCCESS)
		return status;
	if (temp > IB_UINT32_MAX)
		return FINVALID_SETTING;
	*value = (uint32)temp;
	return FSUCCESS;
}

FSTATUS StringToUint16(uint16 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	uint64 temp;
	FSTATUS status;

	status = StringToUint64(&temp, str, endptr, base, skip_trail_whitespace);
	if (status != FSUCCESS)
		return status;
	if (temp > IB_UINT16_MAX)
		return FINVALID_SETTING;
	*value = (uint16)temp;
	return FSUCCESS;
}

FSTATUS StringToUint8(uint8 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	uint64 temp;
	FSTATUS status;

	status = StringToUint64(&temp, str, endptr, base, skip_trail_whitespace);
	if (status != FSUCCESS)
		return status;
	if (temp > IB_UINT8_MAX)
		return FINVALID_SETTING;
	*value = (uint8)temp;
	return FSUCCESS;
}

// convert a string to a int64.  Very similar to strtoull except that
// base=0 implies base 10 or 16, but excludes base 8
// hence allowing leading 0's for base 10.
//
// Also provides easier to use status returns and error checking.
//
// Can also optionally skip trailing whitespace, when skip_trail_whitespace is
// FALSE, trailing whitespace is treated as a FERROR
//
// When endptr is NULL, trailing characters (after optional whitespace) are 
// considered an FERROR.  When endptr is non-NULL, for a FSUCCESS conversion,
// it points to the characters after the optional trailing whitespace.
// Errors:
//	FSUCCESS - successful conversion, *endptr points to 1st char after value
//	FERROR - invalid contents, non-numeric
//	FINVALID_SETTING - value out of range
//	FINVALID_PARAMETER - invalid function arguments (NULL value or str)
FSTATUS StringToInt64(int64 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	char *end = NULL;
	int64 temp;

	if (! str || ! value)
		return FINVALID_PARAMETER;
	errno = 0;
	temp = strtoll(str, &end, base?base:10);
	if ( ! base && ! ((temp == IB_INT64_MAX || temp == IB_INT64_MIN) && errno)
		&& (end && temp == 0 && *end == 'x' && end != str)) {
		// try again as base 16
		temp = strtoll(str, &end, 16);
	}
	if (((temp == IB_INT64_MAX || temp == IB_INT64_MIN) && errno)
		|| (end && end == str)) {
		if (errno == ERANGE)
			return FINVALID_SETTING;
		else
			return FERROR;
	}

	// skip whitespace
	if (end && skip_trail_whitespace) {
		while (isspace(*end)) {
			end++;
		}
	}
	if (endptr)
		*endptr = end;
	else if (end && *end != '\0')
		return FERROR;
	*value = temp;
	return FSUCCESS;
}

FSTATUS StringToInt32(int32 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	int64 temp;
	FSTATUS status;

	status = StringToInt64(&temp, str, endptr, base, skip_trail_whitespace);
	if (status != FSUCCESS)
		return status;
	if (temp < IB_INT32_MIN || temp > IB_INT32_MAX)
		return FINVALID_SETTING;
	*value = (int32)temp;
	return FSUCCESS;
}

FSTATUS StringToInt16(int16 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	int64 temp;
	FSTATUS status;

	status = StringToInt64(&temp, str, endptr, base, skip_trail_whitespace);
	if (status != FSUCCESS)
		return status;
	if (temp < IB_INT16_MIN || temp > IB_INT16_MAX)
		return FINVALID_SETTING;
	*value = (int16)temp;
	return FSUCCESS;
}

FSTATUS StringToInt8(int8 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	int64 temp;
	FSTATUS status;

	status = StringToInt64(&temp, str, endptr, base, skip_trail_whitespace);
	if (status != FSUCCESS)
		return status;
	if (temp < IB_INT8_MIN || temp > IB_INT8_MAX)
		return FINVALID_SETTING;
	*value = (int8)temp;
	return FSUCCESS;
}

// GID of form hValue:lValue
// values must be base 16, as such 0x prefix is optional for both hValue and
// lValue
// whitespace is permitted before and after :
FSTATUS StringToGid(uint64 *hValue, uint64 *lValue, const char* str, char **endptr, boolean skip_trail_whitespace)
{
	FSTATUS status;
	char *end;

	status = StringToUint64(hValue, str, &end, 16, TRUE);
	if (status != FSUCCESS)
		return status;
	if (end == NULL  || *end != ':')
		return FERROR;
	end++;
	return StringToUint64(lValue, end, endptr, 16, skip_trail_whitespace);
}

// Byte Count as an integer followed by an optional suffix of:
// K, KB, M, MB, G or GB
// (K==KB, etc)
// converted to an absolute number of bytes
FSTATUS StringToUint64Bytes(uint64 *value, const char* str, char **endptr, int base, boolean skip_trail_whitespace)
{
	char *end;
	FSTATUS status;
	uint64 temp;

	status = StringToUint64(&temp, str, &end, base, skip_trail_whitespace);
	if (status != FSUCCESS)
		return status;
	if (end) {
		char *units = end;
		// skip whitespace
		while (isspace(*units)) {
			units++;
		}
		// parse optional units
		if (strncmp(units, "KB",2) == 0) {
			temp *= 1024;
			end = units+2;
		} else if (strncmp(units, "K",1) == 0) {
			temp *= 1024;
			end = units+1;
		} else if (strncmp(units, "MB",2) == 0) {
			temp *= 1024*1024;
			end = units+2;
		} else if (strncmp(units, "M",1) == 0) {
			temp *= 1024*1024;
			end = units+1;
		} else if (strncmp(units, "GB",2) == 0) {
			temp *= (1024*1024*1024ULL);
			end = units+2;
		} else if (strncmp(units, "G",1) == 0) {
			temp *= (1024*1024*1024ULL);
			end = units+1;
		}
	}
	
	// skip whitespace
	if (end && skip_trail_whitespace) {
		while (isspace(*end)) {
			end++;
		}
	}
	if (endptr)
		*endptr = end;
	else if (end && *end != '\0')
		return FERROR;
	*value = temp;
	return FSUCCESS;
}

#if !defined(VXWORKS)
#define MEMORY_ALLOCATE_PRIV(size, flags, tag) MemoryAllocatePriv(size, flags, tag)
#define MEMORY_ALLOCATE_PHYS_CONT_PRIV(size) MemoryAllocatePhysContPriv(size)
#define MEMORY_DEALLOCATE_PHYS_CONT_PRIV(size) MemoryDeallocatePhysContPriv(size)
#define MEMORY_DEALLOCATE_PRIV(ptr) MemoryDeallocatePriv(ptr)
#else
#define MEMORY_ALLOCATE_PRIV(size, flags, tag) MemoryAllocatePriv(size, __builtin_return_address(0))
#define MEMORY_ALLOCATE_PHYS_CONT_PRIV(size) MemoryAllocatePhysContPriv(size, __builtin_return_address(0))
#define MEMORY_DEALLOCATE_PHYS_CONT_PRIV(size) MemoryDeallocatePhysContPriv(size, __builtin_return_address(0))
#define MEMORY_DEALLOCATE_PRIV(ptr) MemoryDeallocatePriv(ptr, __builtin_return_address(0))
#endif

#if defined(MEM_TRACK_ON)
//
// Destroy the memory tracker object.
//
static __inline void
DestroyMemTracker( void )
{
	if( !pMemTracker )
		return;

	// Destory all objects in the memory tracker object.
	QListDestroy( &pMemTracker->FreeHrdList );
	SpinLockDestroy( &pMemTracker->Lock );
	QListDestroy( &pMemTracker->AllocList );

	// Free the memory allocated for the memory tracker object.
	MEMORY_DEALLOCATE_PRIV( pMemTracker );
	pMemTracker = NULL;
}

//
// Allocate and initialize the memory tracker object.
//
static __inline boolean
CreateMemTracker( void )
{
	if( pMemTracker )
		return TRUE;

	// Allocate the memory tracker object.
	pMemTracker = (MEM_TRACKER*)MEMORY_ALLOCATE_PRIV( sizeof(MEM_TRACKER), IBA_MEM_FLAG_LEGACY, TRK_TAG );

	if( !pMemTracker )
		return FALSE;

	// Pre-initialize all objects in the memory tracker object.
	QListInitState( &pMemTracker->AllocList );
	SpinLockInitState( &pMemTracker->Lock );
	QListInitState( &pMemTracker->FreeHrdList );

	// Initialize the list.
	if( !QListInit( &pMemTracker->AllocList ) )
	{
		DestroyMemTracker();
		return FALSE;
	}

	// Initialize the spin lock to protect list operations.
	if( !SpinLockInit( &pMemTracker->Lock ) )
	{
		DestroyMemTracker();
		return FALSE;
	}

	// Initialize the free list.
	if( !QListInit( &pMemTracker->FreeHrdList ) )
	{
		DestroyMemTracker();
		return FALSE;
	}

//	MsgOut( "\n\n\n*** Memory tracker object address = %p ***\n\n\n", pMemTracker );
	MsgOut( "\n*** Memory tracker enabled ***\n" );

	return TRUE;
}
#endif

//
// Enables memory allocation tracking.
//
static __inline void
MemoryTrackStart( void )
{
#if defined(MEM_TRACK_ON)
	if( pMemTracker )
		return;

	CreateMemTracker();
#endif	// MEM_TRACK_ON
}


//
// Clean up memory tracking.
//
static __inline void
MemoryTrackStop( void )
{
#if defined(MEM_TRACK_ON)
	LIST_ITEM	*pListItem;

	if( !pMemTracker )
		return;

	if( QListCount( &pMemTracker->AllocList ) )
	{
		// There are still items in the list.  Print them out.
		MemoryDisplayUsage(1, 0, 0);
	} else {
		MsgOut( "\n*** Memory tracker stopped, no leaks detected ***\n" );
		MsgOut("IbAccess max allocations=%u bytes=%u\n",
						max_allocations, max_allocated);
	}

	// Free all allocated headers.
	SpinLockAcquire( &pMemTracker->Lock );
	while( (pListItem = QListRemoveHead( &pMemTracker->AllocList )) != NULL )
	{
		SpinLockRelease( &pMemTracker->Lock );
		MEMORY_DEALLOCATE_PRIV( PARENT_STRUCT( pListItem, MEM_ALLOC_HDR, ListItem ) );
		SpinLockAcquire( &pMemTracker->Lock );
	}
	while( (pListItem = QListRemoveHead( &pMemTracker->FreeHrdList )) != NULL )
	{
		SpinLockRelease( &pMemTracker->Lock );
		MEMORY_DEALLOCATE_PRIV( PARENT_STRUCT( pListItem, MEM_ALLOC_HDR, ListItem ) );
		SpinLockAcquire( &pMemTracker->Lock );
	}
	SpinLockRelease( &pMemTracker->Lock );

	DestroyMemTracker();
#endif	// MEM_TRACK_ON
}


//
// Enables memory allocation tracking.
//
void
MemoryTrackUsage( 
	IN boolean Start )
{
	if( Start )
		MemoryTrackStart();
	else
		MemoryTrackStop();
}

#if defined(MEM_TRACK_ON)
static void
MemoryTrackerShow(
		IN char *prefix, 
		IN MEM_ALLOC_HDR	*pHdr,
		IN char *suffix)
{
#if defined(VXWORKS)
	if ((int)pHdr->LineNum < 0) {
		MsgOut( "%s%p(%u) %s ra=%p tick=%u%s\n", prefix,
				pHdr->ListItem.pObject, pHdr->Bytes, pHdr->trk->filename,
				(void *)pHdr->LineNum, pHdr->tick, suffix );
	} else 
		MsgOut( "%s%p(%u) in file %s line %d tick=%u%s\n", prefix,
				pHdr->ListItem.pObject, pHdr->Bytes, pHdr->trk->filename,
				pHdr->LineNum, pHdr->tick, suffix );
#else
		MsgOut( "%s%p(%u) in file %s line %d%s\n", prefix,
				pHdr->ListItem.pObject, pHdr->Bytes, pHdr->trk->filename,
				pHdr->LineNum, suffix );
#endif
}

static void
MemoryTrackerCheckOverrun(
		IN MEM_ALLOC_HDR	*pHdr )
{
#ifdef MEM_TRACK_FTR
	// Check that the user did not overrun his memory allocation.
	if( (pHdr->pFtr != NULL) && (pHdr->pFtr->OutOfBound != TRK_TAG) )
	{
		MemoryTrackerShow("*** User overrun detected ", pHdr, "");
	}
#endif
}

/* unlink a header from the allocated list */
/* must be called with pMemTracker->Lock held */
static void
MemoryTrackerUnlink(
		IN MEM_ALLOC_HDR	*pHdr )
{
	// Remove the item from the list.
	QListRemoveItem( &pMemTracker->AllocList, &pHdr->ListItem );

	--current_allocations;
	current_allocated -= pHdr->Bytes;
	if (pHdr->reported)
		MemoryTrackerShow("", pHdr, " FREED");
	MemoryTrackerDereference(pHdr->trk);
	// Return the header to the free header list.
	QListInsertHead( &pMemTracker->FreeHrdList, &pHdr->ListItem );
}
#endif	// MEM_TRACK_ON

//
// Display memory usage.
//
void
MemoryDisplayUsage( int method, uint32 minSize, uint32 minTick )
{
#if defined(MEM_TRACK_ON)
	uint32 allocated = 0;
	uint32 allocations = 0;
	MEM_ALLOC_HDR	*pHdr;
	LIST_ITEM *item, *next, *tail, *head;
	unsigned int allocations_per_sec = 0;
	uint32 currentTime;
	boolean all = (method == 1);

	if( !pMemTracker ) {
		MsgOut( "*** IbAccess Memory Tracking is disabled ***\n" );
		return;
	}

	/* "lock" present allocations by setting
	 * displaying flag, so other allocates/frees will not affect them
	 * This gives us a snapshot while permitting the system to run
	 * while we perform the output (the output itself may use memory allocate)
	 * However, our report loop below must stay within head/tail
	 */
	SpinLockAcquire( &pMemTracker->Lock );
	tail = QListTail(&pMemTracker->AllocList);
	head = QListHead(&pMemTracker->AllocList);
	for(item = head; item != NULL; item = QListNext(&pMemTracker->AllocList, item)) {
		pHdr = PARENT_STRUCT( item, MEM_ALLOC_HDR, ListItem );
		pHdr->displaying = TRUE;
	}
	SpinLockRelease (&pMemTracker->Lock);
	
	MsgOut( "*** IbAccess Memory Usage %s minSize=%d minTick=%d ***\n", all?"All":"Unreported", minSize, minTick );

	if (head && tail) {
		item = head;
		do {
			next = QListNext(&pMemTracker->AllocList, item);
			pHdr = PARENT_STRUCT( item, MEM_ALLOC_HDR, ListItem );
	
	#ifdef MEM_TRACK_FTR
			// Check that the user did not overrun his memory allocation.
			if (pHdr->deallocate == FALSE) {
				MemoryTrackerCheckOverrun(pHdr);
			}
	#endif	// MEM_TRACK_FTR
			if ((pHdr->Bytes >= minSize) && (pHdr->tick >= minTick) && (all || (pHdr->reported == 0))) {
				// method 2 just marks all current allocations as reported, without actually reporting them
				// method 3 displays the items without changing their reported state (allows us to avoid the FREED messages)
				if (method != 2)
					MemoryTrackerShow("", pHdr, "");
				if (method != 3)
					pHdr->reported = 1;
			}
			allocated += pHdr->Bytes;
			++allocations;
			SpinLockAcquire( &pMemTracker->Lock );
			pHdr->displaying = FALSE;
			if (pHdr->deallocate) {
				MemoryTrackerUnlink(pHdr);
			}
			SpinLockRelease (&pMemTracker->Lock);
			item = next;
		} while (&pHdr->ListItem != tail && item != NULL);
	}
	currentTime = GetTimeStampSec();
	if (last_reported_secs && currentTime != last_reported_secs) {
		allocations_per_sec = (total_allocations - last_reported_allocations) / (currentTime - last_reported_secs);
	}
	last_reported_secs = currentTime;
	last_reported_allocations = total_allocations;
	MsgOut("IbAccess current allocations=%u bytes=%u max allocations=%u bytes=%u p/s=%d\n",
			allocations, allocated, max_allocations, max_allocated, allocations_per_sec);
#endif	// MEM_TRACK_ON
}


#if defined(MEM_TRACK_ON)
unsigned int hashValue(const char *key) {
	unsigned int  nHash = 0;
	while (*key)
		nHash = (nHash<<5) + nHash + *key++;
	return nHash;
}

static MemoryTrackerFileName_t *MemoryTrackerFileNameLookup(const char *filename, unsigned int *hash) {
	unsigned int hashVal;
	MemoryTrackerFileName_t *trk;
	int len = strlen(filename);

	hashVal = hashValue(filename) % MEMORY_TRACKER_BUCKETS;
	*hash = hashVal;

	for(trk = MemoryTrackerBuckets[hashVal]; trk != NULL; trk = trk->next) {
		if (trk->filenameLen == len) {
			if (memcmp(&trk->filename[0], filename, len) == 0) {
				return trk;
			}
		}
	}
	return NULL;
}

static MemoryTrackerFileName_t *MemoryTrackerFileNameAlloc(const char *filename, int filenameLen, unsigned int hash) {
	MemoryTrackerFileName_t *trk;

	trk = (MemoryTrackerFileName_t*)MEMORY_ALLOCATE_PRIV(
					 	sizeof( MemoryTrackerFileName_t ) + filenameLen + 1,
						IBA_MEM_FLAG_LEGACY, TRK_TAG );
	if (trk != NULL) {
		trk->referenceCount = 1;
		trk->filenameLen = filenameLen;
		memcpy(&trk->filename, filename, filenameLen + 1);
		trk->next = MemoryTrackerBuckets[hash];
		MemoryTrackerBuckets[hash] = trk;
		// MsgOut("Added len=%d name=(%p)%s\n", filenameLen, trk->filename, trk->filename);
	}
	return trk;
}

static MemoryTrackerFileName_t *MemoryTrackerReference(const char *filename) {
	MemoryTrackerFileName_t *trk;
	int len = strlen(filename);
	unsigned int hash;

	trk = MemoryTrackerFileNameLookup(filename, &hash);
	if (trk == NULL) {
		trk = MemoryTrackerFileNameAlloc(filename, len, hash);
		if (trk == NULL)
			return NULL;
	} else {
		++trk->referenceCount;
	}
	return trk;
}

static void MemoryTrackerDereference(MemoryTrackerFileName_t *trk) {
	if (trk == NULL) {
		MsgOut("Could not find reference to trk=%p\n", trk);
	} else {
		--trk->referenceCount;
	}
}

static void
MemoryTrackerTrackAllocation(
	IN const char *pFileName, 
	IN int32 nLine, 
	IN uint32 Bytes, 
	IN uint32 flags,
	IN void *pMem,
	IN MEM_ALLOC_FTR *pFtr)
{
	MEM_ALLOC_HDR	*pHdr;
	LIST_ITEM		*pListItem;

#ifdef MEM_TRACK_FTR
	if (pFtr)
		pFtr->OutOfBound = TRK_TAG;
#endif  // MEM_TRACK_FTR

	if( !pMemTracker )
		return;

	// Get a header from the free header list.
	SpinLockAcquire( &pMemTracker->Lock );
	pListItem = QListRemoveHead( &pMemTracker->FreeHrdList );
	SpinLockRelease( &pMemTracker->Lock );

	if( pListItem )
	{
		// Set the header pointer to the header retrieved from the list.
		pHdr = PARENT_STRUCT( pListItem, MEM_ALLOC_HDR, ListItem );
	}
	else
	{
		// We failed to get a free header.  Allocate one.
		// we can prempt if caller allows it, however we do not want
		// pageable, nor short duration memory
		pHdr = (MEM_ALLOC_HDR*)MEMORY_ALLOCATE_PRIV( sizeof( MEM_ALLOC_HDR ),
								flags & IBA_MEM_FLAG_PREMPTABLE, TRK_TAG );
		if( !pHdr )
		{
			// We failed to allocate the header, don't track this allocate
			return;
		}
	}
	pHdr->LineNum = nLine;
	pHdr->tick = TICK;
	pHdr->Bytes = Bytes;
	pHdr->reported = 0;
	pHdr->displaying = FALSE;
	pHdr->deallocate = FALSE;
	// We store the pointer to the memory returned to the user.  This allows
	// searching the list of allocated memory even if the buffer allocated is 
	// not in the list without dereferencing memory we do not own.
	pHdr->ListItem.pObject = pMem;

#ifdef MEM_TRACK_FTR
	pHdr->pFtr = pFtr;
#else
	pHdr->pFtr = NULL;
#endif  // MEM_TRACK_FTR

	SpinLockAcquire( &pMemTracker->Lock );
	pHdr->trk = MemoryTrackerReference(pFileName);
	++total_allocations;
	if (++current_allocations > max_allocations)
		max_allocations = current_allocations;
	if ((current_allocated += pHdr->Bytes) > max_allocated)
		max_allocated = current_allocated;

	// Insert the header structure into our allocation list.
	QListInsertTail( &pMemTracker->AllocList, &pHdr->ListItem );
	SpinLockRelease( &pMemTracker->Lock );

	return;
}

static int
MemoryTrackerTrackDeallocate( 
	IN void *pMemory )
{
	MEM_ALLOC_HDR	*pHdr;
	LIST_ITEM		*pListItem;
	int				result = 0;

	if( pMemTracker )
	{
		SpinLockAcquire( &pMemTracker->Lock );

		// Removes an item from the allocation tracking list given a pointer
		// To the user's data and returns the pointer to header referencing the
		// allocated memory block.
		pListItem = 
			QListFindFromTail( &pMemTracker->AllocList, NULL, pMemory );

		if( pListItem )
		{
			// Get the pointer to the header.
			pHdr = PARENT_STRUCT( pListItem, MEM_ALLOC_HDR, ListItem );
#ifdef MEM_TRACK_FTR
			MemoryTrackerCheckOverrun(pHdr);
#endif	// MEM_TRACK_FTR

			if (pHdr->displaying) {
				pHdr->deallocate = TRUE;
			} else {
				// Remove the item from the list.
				MemoryTrackerUnlink(pHdr);
			}
		} else {
			result = 1;
#if defined(VXWORKS)
			MsgOut( "UNMATCHED FREE %p ra=%p\n", pMemory, __builtin_return_address(0));
#else
			MsgOut( "BAD FREE %p\n", pMemory);
#endif
			DumpStack();
		}
		SpinLockRelease( &pMemTracker->Lock );
	}
	return result;
}

//
// Allocates memory and stores information about the allocation in a list.
// The contents of the list can be printed out by calling the function
// "MemoryReportUsage".  Memory allocation will succeed even if the list 
// cannot be created.
//
void*
MemoryAllocateDbg(
	IN const char *pFileName, 
	IN int32 nLine, 
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MemoryAllocate2Dbg(pFileName, nLine, Bytes,
					(IsPageable?IBA_MEM_FLAG_PAGEABLE:IBA_MEM_FLAG_NONE)
					|IBA_MEM_FLAG_LEGACY, Tag);
}

#if defined(VXWORKS)
void
MemoryAllocateVxWorksTrack(
	IN void *result,
	IN uint32 Bytes,
	IN char *reason,
	IN void *caller) 
{
	MemoryTrackerTrackAllocation(reason, (int)caller, Bytes, IBA_MEM_FLAG_NONE, result, NULL);
}
#endif

void*
MemoryAllocate2Dbg(
	IN const char *pFileName, 
	IN int32 nLine, 
	IN uint32 Bytes, 
	IN uint32 flags,
	IN uint32 Tag )
{
	void			*pMem;

#ifdef MEM_TRACK_FTR
	// Increase the size of our allocation to account for the footer.
	Bytes += sizeof( MEM_ALLOC_FTR );
	Bytes = (Bytes + 3) >> 2 << 2;
#endif  // MEM_TRACK_FTR

	// Allocate the memory first, so that we give the user's allocation 
	// priority over the the header allocation.
	pMem = MEMORY_ALLOCATE_PRIV( Bytes, flags, Tag );

	if( !pMem )
		return NULL;

	MemoryTrackerTrackAllocation(pFileName, nLine, Bytes, flags, pMem,
#ifdef MEM_TRACK_FTR
			(MEM_ALLOC_FTR*)((uchar*)pMem + Bytes - sizeof( MEM_ALLOC_FTR ))
#else
			NULL
#endif
			);

	return pMem;
}

void*
MemoryAllocateRel(
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MemoryAllocateDbg( "Unknown", 0, Bytes, IsPageable, Tag );
}
void*
MemoryAllocate2Rel(
	IN uint32 Bytes, 
	IN uint32 flags,
	IN uint32 Tag )
{
	return MemoryAllocate2Dbg( "Unknown", 0, Bytes, flags, Tag );
}

#if defined(VXWORKS)
void*
MemoryAllocatePhysContDbg(
	IN const char *pFileName, 
	IN int32 nLine, 
	IN uint32 Bytes )
{
	void			*pMem;

	/* no footer on PhysCont allocates, they tend to be a full page
	 * and a footer would waste another page, TBD we could round up
	 * provided resulting Bytes was still same number of pages
	 */

	// Allocate the memory first, so that we give the user's allocation 
	// priority over the the header allocation.
	pMem = MEMORY_ALLOCATE_PHYS_CONT_PRIV( Bytes );

	if( !pMem )
		return NULL;

	MemoryTrackerTrackAllocation(pFileName, nLine, Bytes, IBA_MEM_FLAG_PREMPTABLE, pMem, NULL);

	return pMem;
}

void*
MemoryAllocatePhysContRel(
	IN uint32 Bytes )
{
	return MemoryAllocatePhysContDbg( "Unknown", 0, Bytes );
}
#endif /* defined(VXWORKS) */
#else	// !MEM_TRACK_ON
void*
MemoryAllocateDbg(
	IN const char *pFileName, 
	IN int32 nLine, 
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MEMORY_ALLOCATE_PRIV( Bytes,
					(IsPageable?IBA_MEM_FLAG_PAGEABLE:IBA_MEM_FLAG_NONE)
					|IBA_MEM_FLAG_LEGACY, Tag );
}

void*
MemoryAllocate2Dbg(
	IN const char *pFileName, 
	IN int32 nLine, 
	IN uint32 Bytes, 
	IN uint32 flags,
	IN uint32 Tag )
{
	return MEMORY_ALLOCATE_PRIV( Bytes, flags, Tag);
}
void*
MemoryAllocateRel(
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MEMORY_ALLOCATE_PRIV( Bytes, 
					(IsPageable?IBA_MEM_FLAG_PAGEABLE:IBA_MEM_FLAG_NONE)
					|IBA_MEM_FLAG_LEGACY, Tag );
}

void*
MemoryAllocate2Rel(
	IN uint32 Bytes, 
	IN uint32 flags,
	IN uint32 Tag )
{
	return MEMORY_ALLOCATE_PRIV( Bytes, flags, Tag);
}

#if defined(VXWORKS)
void*
MemoryAllocatePhysContDbg(
	IN const char *pFileName, 
	IN int32 nLine, 
	IN uint32 Bytes )
{
	return MEMORY_ALLOCATE_PHYS_CONT_PRIV( Bytes );
}

void*
MemoryAllocatePhysContRel(
	IN uint32 Bytes )
{
	return MEMORY_ALLOCATE_PHYS_CONT_PRIV( Bytes );
}
#endif /* defined(VXWORKS) */
#endif	// MEM_TRACK_ON


#if defined(MEM_TRACK_ON)
void*
MemoryAllocateAndClearDbg( 
	IN const char *pFileName, 
	IN int32 nLine,
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MemoryAllocate2AndClearDbg(pFileName, nLine, Bytes,
					(IsPageable?IBA_MEM_FLAG_PAGEABLE:IBA_MEM_FLAG_NONE)
					|IBA_MEM_FLAG_LEGACY, Tag );
}
void*
MemoryAllocate2AndClearDbg( 
	IN const char *pFileName, 
	IN int32 nLine,
	IN uint32 Bytes, 
	IN uint32 flags,
	IN uint32 Tag )
{
	void	*pBuffer;

	if( (pBuffer = MemoryAllocate2Dbg( pFileName, nLine, Bytes, flags, Tag )) != NULL )
	{
		MemoryClear( pBuffer, Bytes );
	}

	return pBuffer;
}

void*
MemoryAllocateAndClearRel( 
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MemoryAllocateAndClearDbg("Unknown", 0, Bytes, IsPageable, Tag);
}

void*
MemoryAllocate2AndClearRel( 
	IN uint32 Bytes, 
	IN uint32 flags,
	IN uint32 Tag )
{
	return MemoryAllocate2AndClearDbg("Unknown", 0, Bytes, flags, Tag);
}

#else	// !MEM_TRACK_ON
void*
MemoryAllocateAndClearDbg( 
	IN const char *pFileName, 
	IN int32 nLine,
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MemoryAllocate2AndClear(Bytes,
					(IsPageable?IBA_MEM_FLAG_PAGEABLE:IBA_MEM_FLAG_NONE)
					|IBA_MEM_FLAG_LEGACY, Tag );
}

void*
MemoryAllocate2AndClearDbg( 
	IN const char *pFileName, 
	IN int32 nLine,
	IN uint32 Bytes, 
	IN uint32 flags, 
	IN uint32 Tag )
{
	return MemoryAllocate2AndClear(Bytes, flags, Tag);
}

void*
MemoryAllocateAndClearRel( 
	IN uint32 Bytes, 
	IN boolean IsPageable, 
	IN uint32 Tag )
{
	return MemoryAllocate2AndClear(Bytes,
					(IsPageable?IBA_MEM_FLAG_PAGEABLE:IBA_MEM_FLAG_NONE)
					|IBA_MEM_FLAG_LEGACY, Tag );
}

void*
MemoryAllocate2AndClearRel( 
	IN uint32 Bytes, 
	IN uint32 flags,
	IN uint32 Tag )
{
	void	*pBuffer;

	if( (pBuffer = MEMORY_ALLOCATE_PRIV( Bytes, flags, Tag )) != NULL )
	{
		MemoryClear( pBuffer, Bytes );
	}

	return pBuffer;
}
#endif	// !MEM_TRACK_ON


int
MemoryDeallocate( 
	IN void *pMemory )
{
#if defined(MEM_TRACK_ON)
	int result;

	result = MemoryTrackerTrackDeallocate(pMemory );
	MEMORY_DEALLOCATE_PRIV( pMemory );
	return result;
#else
	MEMORY_DEALLOCATE_PRIV( pMemory );
	return 0;
#endif	// MEM_TRACK_ON

}

#if defined(VXWORKS)
void
MemoryDeallocatePhysCont( 
	IN void *pMemory )
{
#if defined(MEM_TRACK_ON)
	(void)MemoryTrackerTrackDeallocate(pMemory );
#endif	// MEM_TRACK_ON
	MEMORY_DEALLOCATE_PHYS_CONT_PRIV( pMemory );
}
#endif /* defined(VXWORKS) */

void
MemoryClear( 
	IN void *pMemory, 
	IN uint32 Bytes )
{
	MemoryFill( pMemory, 0, Bytes );
}


#if defined(MEM_TRACK_ON)
void*
MemoryAllocateObjectArrayRel(
	IN uint32 ObjectCount, 
	IN OUT uint32 *pObjectSize,  
	IN uint32 ByteAlignment, 
	IN uint32 AlignmentOffset,
	IN boolean IsPageable, 
	IN uint32 Tag,
	OUT void **ppFirstObject, 
	OUT uint32 *pArraySize )
{
	return MemoryAllocateObjectArrayDbg("Unknown", 0, ObjectCount, pObjectSize,
						ByteAlignment, AlignmentOffset, IsPageable, Tag,
						ppFirstObject, pArraySize);
}

void*
MemoryAllocateObjectArrayDbg(
	IN const char *pFileName, 
	int32 nLine,
	IN uint32 ObjectCount, 
	IN OUT uint32 *pObjectSize,  
	IN uint32 ByteAlignment, 
	IN uint32 AlignmentOffset,
	IN boolean IsPageable, 
	IN uint32 Tag,
	OUT void **ppFirstObject, 
	OUT uint32 *pArraySize )
#else	// !MEM_TRACK_ON
void*
MemoryAllocateObjectArrayDbg(
	IN const char *pFileName, 
	int32 nLine,
	IN uint32 ObjectCount, 
	IN OUT uint32 *pObjectSize,  
	IN uint32 ByteAlignment, 
	IN uint32 AlignmentOffset,
	IN boolean IsPageable, 
	IN uint32 Tag,
	OUT void **ppFirstObject, 
	OUT uint32 *pArraySize )
{
	return MemoryAllocateObjectArrayRel(ObjectCount, pObjectSize, ByteAlignment,
						AlignmentOffset, IsPageable, Tag,
						ppFirstObject, pArraySize);
}

void*
MemoryAllocateObjectArrayRel(
	IN uint32 ObjectCount, 
	IN OUT uint32 *pObjectSize,  
	IN uint32 ByteAlignment, 
	IN uint32 AlignmentOffset,
	IN boolean IsPageable, 
	IN uint32 Tag,
	OUT void **ppFirstObject, 
	OUT uint32 *pArraySize )
#endif	// MEM_TRACK_ON
{
	void	*pArray;

	ASSERT( ObjectCount && *pObjectSize && AlignmentOffset < *pObjectSize );

	if( ByteAlignment > 1)
	{
		// Fixup the object size based on the alignment specified.
		*pObjectSize = ((*pObjectSize) + ByteAlignment - 1) -
			(((*pObjectSize) + ByteAlignment - 1) % ByteAlignment);
	}

	// Determine the size of the buffer to allocate.
	*pArraySize = (ObjectCount * (*pObjectSize)) + ByteAlignment;

	// Allocate the array of objects.
#if defined(MEM_TRACK_ON)
	if( !(pArray = MemoryAllocateAndClearDbg( pFileName, nLine, *pArraySize, 
		IsPageable, Tag )) )
#else	// !MEM_TRACK_ON
	if( !(pArray = MemoryAllocateAndClear( *pArraySize, IsPageable, Tag )) )

#endif	// MEM_TRACK_ON
	{
		*pArraySize = 0;
		return NULL;
	}

	if( ByteAlignment > 1 )
	{
		// Calculate the pointer to the first object that is properly aligned.
		*ppFirstObject = (void*)(
			((uchar*)pArray + AlignmentOffset + ByteAlignment - 1) - 
			(((uintn)pArray + AlignmentOffset + ByteAlignment - 1) %
			ByteAlignment ));
	}
	else
	{
		*ppFirstObject = pArray;
	}
	return pArray;
}

