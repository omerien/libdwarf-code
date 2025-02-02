/*
Copyright (C) 2011-2012 SN Systems Ltd. All Rights Reserved.
Portions Copyright (C) 2011-2019 David Anderson. All Rights Reserved.

  This program is free software; you can redistribute it and/or
  modify it under the terms of version 2 of the GNU General
  Public License as published by the Free Software Foundation.

  This program is distributed in the hope that it would be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

  Further, this software is distributed without any warranty
  that it is free of the rightful claim of any third person
  regarding infringement or the like.  Any license provided
  herein, whether implied or otherwise, applies only to this
  software file.  Patent licenses, if any, provided herein
  do not apply to combinations of this program with other
  software, or any other product whatsoever.

  You should have received a copy of the GNU General Public
  License along with this program; if not, write the Free
  Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
  Boston MA 02110-1301, USA.

*/
/*

   These simple list-processing functions are in support
   of checking DWARF for compiler-errors of various sorts.

*/

#include <config.h>

#include <stdio.h>  /* printf() */
#include <stdlib.h> /* calloc() free() */
#include <assert.h> /* assert() */ /* If you get error with it on Mac, try reinstalling it in /usr/local/include */
#include <string.h> /* strcmp() */

/* Windows specific header files */
#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#ifdef HAVE_STDINT_H
#include <stdint.h> /* uintptr_t */
#endif /* HAVE_STDINT_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "libdwarf_private.h"
#include "dd_globals.h"
#include "dd_esb.h"

/* Guessing a sensible length for max section name.  */
#define SECTION_NAME_LEN 2048

/* Private function */
static void DumpFullBucketGroup(Bucket_Group *pBucketGroup);
static Dwarf_Signed FindDataIndexInBucket(Bucket_Group *pBucketGroup,
    Bucket_Data *pBucketData);
static void PrintBucketData(Bucket_Group *pBucketGroup,
    Bucket_Data *pBucketData);
static void ProcessBucketGroup(Bucket_Group *pBucketGroup,
    void (*pFunction)(Bucket_Group *pBucketGroup,
        Bucket_Data *pBucketData));

Bucket_Group *
AllocateBucketGroup(int kind)
{
    Bucket_Group *pBucketGroup = (Bucket_Group *)calloc(1,
        sizeof(Bucket_Group));
    if (!pBucketGroup) {
        return NULL;
    }
    pBucketGroup->kind = kind;
    return pBucketGroup;
}

void
ReleaseBucketGroup(Bucket_Group *pBucketGroup)
{
    Bucket *pBucket = 0;
    Bucket *pNext = 0;

    assert(pBucketGroup);
    for (pBucket = pBucketGroup->pHead; pBucket; pBucket = pNext ) {
        pNext = pBucket->pNext;
        free(pBucket);
    }
    pBucketGroup->pHead = NULL;
    pBucketGroup->pTail = NULL;
    free(pBucketGroup);
}

void
ResetBucketGroup(Bucket_Group *pBucketGroup)
{
    Bucket *pBucket = 0;

    assert(pBucketGroup);
    for (pBucket = pBucketGroup->pHead; pBucket;
        pBucket = pBucket->pNext) {
        pBucket->nEntries = 0;
    }
    ResetSentinelBucketGroup(pBucketGroup);
}

/* Reset sentinels in a Bucket Group. */
void
ResetSentinelBucketGroup(Bucket_Group *pBucketGroup)
{
    /* Sanity checks */
    assert(pBucketGroup);
    pBucketGroup->pFirst = NULL;
    pBucketGroup->pLast = NULL;
}

void PrintBucketGroup(Bucket_Group *pBucketGroup,Dwarf_Bool bFull)
{
    if (pBucketGroup) {
        if (bFull) {
            DumpFullBucketGroup(pBucketGroup);
        } else {
            if (pBucketGroup->pFirst && pBucketGroup->pLast) {
                printf("\nBegin Traversing, First = 0x%08" DW_PR_DUx
                    ", Last = 0x%08" DW_PR_DUx "\n",
                pBucketGroup->pFirst->key,pBucketGroup->pLast->key);
                ProcessBucketGroup(pBucketGroup,PrintBucketData);
            } else { /* Nothing to print */ }
        }
    }
}

static void
PrintBucketData(Bucket_Group *pBucketGroup,Bucket_Data *pBucketData)
{
    Dwarf_Signed nCount = 0;
    assert(pBucketGroup);
    assert(pBucketData);

    nCount = FindDataIndexInBucket(pBucketGroup,pBucketData);
    printf("[%06" DW_PR_DSd
        "] Key = 0x%08" DW_PR_DUx
        ", Base = 0x%08" DW_PR_DUx
        ", Low = 0x%08" DW_PR_DUx
        ", High = 0x%08" DW_PR_DUx
        ", Flag = %d, Name = '%s'\n",
        ++nCount,
        pBucketData->key,
        pBucketData->base,
        pBucketData->low,
        pBucketData->high,
        pBucketData->bFlag,
        pBucketData->name);
}

static void
DumpFullBucketGroup(Bucket_Group *pBucketGroup)
{
    int nBucketNo = 1;
    int nIndex = 0;
    int nCount = 0;
    Bucket *pBucket = 0;
    Bucket_Data *pBucketData = 0;

    assert(pBucketGroup);
    printf("\nBucket Group at 0x%" DW_PR_DUx
        " [lower 0x%" DW_PR_DUx " upper 0x%" DW_PR_DUx "]\n",
        (Dwarf_Unsigned)(uintptr_t)pBucketGroup,
        (Dwarf_Unsigned)pBucketGroup->lower,
        (Dwarf_Unsigned)pBucketGroup->upper);
    for (pBucket = pBucketGroup->pHead; pBucket && pBucket->nEntries;
        pBucket = pBucket->pNext) {

        printf("LowPC & HighPC records for bucket %d, at 0x%08"
            DW_PR_DUx "\n",
            nBucketNo++,
            (Dwarf_Unsigned)(uintptr_t)pBucket);
        for (nIndex = 0; nIndex < pBucket->nEntries; ++nIndex) {
            pBucketData = &pBucket->Entries[nIndex];
            printf("[%06d] Key = 0x%08" DW_PR_DUx
                ", Base = 0x%08" DW_PR_DUx
                ", Low = 0x%08" DW_PR_DUx ", High = 0x%08" DW_PR_DUx
                ", Flag = %d, Name = '%s'\n",
                ++nCount,
                pBucketData->key,
                pBucketData->base,
                pBucketData->low,
                pBucketData->high,
                pBucketData->bFlag,
                pBucketData->name);
        }
    }
}

/*  Insert entry into Bucket Group.
    We make no check for duplicate information. */
void
AddEntryIntoBucketGroup(Bucket_Group *pBucketGroup,
    Dwarf_Addr key,Dwarf_Addr base,
    Dwarf_Addr low,Dwarf_Addr high,
    const char *name,
    Dwarf_Bool bFlag)
{
    Bucket *pBucket = 0;
    Bucket_Data data;

    data.bFlag = bFlag;
    data.name = name;
    data.key = key;
    data.base = base;
    data.low = low;
    data.high = high;

    assert(pBucketGroup);
    if (!pBucketGroup->pHead) {
        /* Allocate first bucket */
        pBucket = (Bucket *)calloc(1,sizeof(Bucket));
        if (!pBucket) {
            return;
        }
        pBucketGroup->pHead = pBucket;
        pBucketGroup->pTail = pBucket;
        pBucket->nEntries = 1;
        pBucket->Entries[0] = data;
        return;
    }
    pBucket = pBucketGroup->pTail;

    /*  Check if we have a previous allocated set of
        buckets (have been cleared */
    if (pBucket->nEntries) {
        if (pBucket->nEntries < BUCKET_SIZE) {
            pBucket->Entries[pBucket->nEntries++] = data;
        } else {
            /* Allocate new bucket */
            pBucket = (Bucket *)calloc(1,sizeof(Bucket));
            if (!pBucket) {
                return;
            }
            pBucketGroup->pTail->pNext = pBucket;
            pBucketGroup->pTail = pBucket;
            pBucket->nEntries = 1;
            pBucket->Entries[0] = data;
        }
    } else {
        /*  We have an allocated bucket with zero entries;
            search for the
            first available bucket to be used as the current
            insertion point */
        for (pBucket = pBucketGroup->pHead; pBucket;
            pBucket = pBucket->pNext) {

            if (pBucket->nEntries < BUCKET_SIZE) {
                pBucket->Entries[pBucket->nEntries++] = data;
                break;
            }
        }
    }
}

/*  For Groups where entries are individually deleted, this does
    that work.  */
Dwarf_Bool
DeleteKeyInBucketGroup(Bucket_Group *pBucketGroup,Dwarf_Addr key)
{
    int nIndex = 0;
    Bucket *pBucket = 0;
    Bucket_Data *pBucketData = 0;

    /* Sanity checks */
    assert(pBucketGroup);

    /* For now do a linear search */
    for (pBucket = pBucketGroup->pHead; pBucket && pBucket->nEntries;
        pBucket = pBucket->pNext) {

        for (nIndex = 0; nIndex < pBucket->nEntries; ++nIndex) {
            pBucketData = &pBucket->Entries[nIndex];
            if (pBucketData->key == key) {
                Bucket_Data data = {FALSE,NULL,0,0,0,0};
                int nStart;
                for (nStart = nIndex + 1; nStart < pBucket->nEntries;
                    ++nStart) {

                    pBucket->Entries[nIndex] =
                        pBucket->Entries[nStart];
                    ++nIndex;
                }
                pBucket->Entries[nIndex] = data;
                --pBucket->nEntries;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/*  Search to see if the address is in the range between
    low and high addresses in some Bucked Data record.
    This matches == if high is exact match (which usually means
    one-past-true-high).  */
Dwarf_Bool
FindAddressInBucketGroup(Bucket_Group *pBucketGroup,
    Dwarf_Addr address)
{
    int nIndex = 0;
    Bucket *pBucket = 0;
    Bucket_Data *pBucketData = 0;

    assert(pBucketGroup);
    /* For now do a linear search */
    for (pBucket = pBucketGroup->pHead; pBucket && pBucket->nEntries;
        pBucket = pBucket->pNext) {

        for (nIndex = 0; nIndex < pBucket->nEntries; ++nIndex) {
            pBucketData = &pBucket->Entries[nIndex];
            if (address >= pBucketData->low &&
                address <= pBucketData->high) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/*  Search an entry (Bucket Data) in the Bucket Set */
Bucket_Data *FindDataInBucketGroup(Bucket_Group *pBucketGroup,
    Dwarf_Addr key)
{
    int mid = 0;
    int low = 0;
    int high = 0;
    Bucket *pBucket = 0;
    Bucket_Data *pBucketData = 0;

    assert(pBucketGroup);

    for (pBucket = pBucketGroup->pHead; pBucket;
        pBucket = pBucket->pNext) {
        /* Get lower and upper references */
        if (pBucket->nEntries) {
            low = 0;
            high = pBucket->nEntries;
            while (low < high) {
                mid = low + (high - low) / 2;
                if (pBucket->Entries[mid].key < key) {
                    low = mid + 1;
                } else {
                    high = mid;
                }
            }
            if ((low < pBucket->nEntries) &&
                (pBucket->Entries[low].key == key)) {

                pBucketData = &pBucket->Entries[low];
                /* Update sentinels to allow traversing the table */
                if (!pBucketGroup->pFirst) {
                    pBucketGroup->pFirst = pBucketData;
                }
                pBucketGroup->pLast = pBucketData;
                return pBucketData;
            }
        }
    }
    return (Bucket_Data *)NULL;
}

/*  Find the Bucket that contains a given Bucket Data
    and return its index or -1 if none such. */
static Dwarf_Signed
FindDataIndexInBucket(Bucket_Group *pBucketGroup,
    Bucket_Data *pBucketData)
{
    Bucket *pBucket = 0;
    Bucket_Data *pLower = 0;
    Bucket_Data *pUpper = 0;

    /* Sanity checks */
    assert(pBucketGroup);
    assert(pBucketData);

    /* Use sentinels if any. */
    if (pBucketGroup->pFirst && pBucketGroup->pLast &&
        pBucketData >= pBucketGroup->pFirst &&
        pBucketData <= pBucketGroup->pLast) {

        /* Find bucket that contains the first sentinel */
        for (pBucket = pBucketGroup->pHead;
            pBucket && pBucket->nEntries;
            pBucket = pBucket->pNext) {

            pLower = &pBucket->Entries[0];
            pUpper = &pBucket->Entries[pBucket->nEntries - 1];

            /* Check if the first sentinel is in this bucket. */
            if (pBucketGroup->pFirst >= pLower &&
                pBucketGroup->pFirst <= pUpper) {
                /* We have found the bucket, return the index. */
                return (Dwarf_Signed)
                    (pBucketData - pBucketGroup->pFirst);
            }
        }
    } else {
        /* Find bucket that contains the entry */
        for (pBucket = pBucketGroup->pHead;
            pBucket && pBucket->nEntries;
            pBucket = pBucket->pNext) {

            pLower = &pBucket->Entries[0];
            pUpper = &pBucket->Entries[pBucket->nEntries - 1];

            /* Check if the first sentinel is in this bucket */
            if (pBucketData >= pLower && pBucketData <= pUpper) {
                /* We have found the bucket, return the index */
                return (Dwarf_Signed)(pBucketData - pLower);
            }
        }
    }
    /* Invalid data; just return index indicating not-found */
    return (Dwarf_Signed)-1;
}

/*  Search an entry (Bucket Data) in the Bucket Group.
    The key is an offset, a DIE offset
    within Visited info. */
Bucket_Data *FindKeyInBucketGroup(Bucket_Group *pBucketGroup,
    Dwarf_Addr key)
{
    int nIndex = 0;
    Bucket *pBucket = 0;
    Bucket_Data *pBucketData = 0;

    /* Sanity checks */
    assert(pBucketGroup);

    /* For now do a linear search */
    for (pBucket = pBucketGroup->pHead; pBucket && pBucket->nEntries;
        pBucket = pBucket->pNext) {
        for (nIndex = 0; nIndex < pBucket->nEntries; ++nIndex) {
            pBucketData = &pBucket->Entries[nIndex];
            if (pBucketData->key == key) {
                return pBucketData;
            }
        }
    }
    return (Bucket_Data *)NULL;
}

/*  Search an entry (Bucket Data) in the Bucket Set by name.
    Used to find link-once section names. */
Bucket_Data *
FindNameInBucketGroup(Bucket_Group *pBucketGroup,char *name)
{
    int nIndex = 0;
    Bucket *pBucket = 0;
    Bucket_Data *pBucketData = 0;

    assert(pBucketGroup);
    /* For now do a linear search. */
    for (pBucket = pBucketGroup->pHead; pBucket && pBucket->nEntries;
        pBucket = pBucket->pNext) {
        for (nIndex = 0; nIndex < pBucket->nEntries; ++nIndex) {
            pBucketData = &pBucket->Entries[nIndex];
            if (!strcmp(pBucketData->name,name)) {
                return pBucketData;
            }
        }
    }
    return (Bucket_Data *)NULL;
}

/*  Check if an address valid or not. That is,
    check if it is in  the lower -> upper range of a bucket.
    It checks <= and >= so the lower end
    and  one-past on the upper end matches.
*/
Dwarf_Bool
IsValidInBucketGroup(Bucket_Group *pBucketGroup,Dwarf_Addr address)
{
    Bucket *pBucket = 0;
    Bucket_Data *pBucketData = 0;
    int nIndex = 0;

    assert(pBucketGroup);
    /* Check the address is within the allowed limits */
    if (address >= pBucketGroup->lower &&
        address <= pBucketGroup->upper) {
        pBucket = pBucketGroup->pHead;
        for ( ;
            pBucket && pBucket->nEntries;
            pBucket = pBucket->pNext) {
            for (nIndex = 0; nIndex < pBucket->nEntries; ++nIndex) {
                pBucketData = &pBucket->Entries[nIndex];
                if (address >= pBucketData->low &&
                    address <= pBucketData->high) {
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/*  Reset limits for values in the Bucket Set */
void
ResetLimitsBucketSet(Bucket_Group *pBucketGroup)
{
    assert(pBucketGroup);
    pBucketGroup->lower = 0;
    pBucketGroup->upper = 0;
}

/*  Limits are set only for ranges, so only in pRangesInfo.
    But is used for ranges and location lists.
    The default is set from object data (virt addr,
    size in object file) but that does not work
    sensibly in PE object files. */
void
SetLimitsBucketGroup(Bucket_Group *pBucketGroup,
    Dwarf_Addr lower,Dwarf_Addr upper)
{
    assert(pBucketGroup);
    if (lower < upper) {
        pBucketGroup->lower = lower;
        pBucketGroup->upper = upper;
    }
}

/* Traverse Bucket Set and execute a supplied function */
static void
ProcessBucketGroup(Bucket_Group *pBucketGroup,
    void (*pFunction)(Bucket_Group *pBucketGroup,
        Bucket_Data *pBucketData))
{
    Dwarf_Signed nIndex = 0;
    Dwarf_Signed nStart = 0;
    Bucket      *pBucket = 0;
    Bucket_Data *pBucketData = 0;
    Bucket_Data *pLower = 0;
    Bucket_Data *pUpper = 0;
    Dwarf_Bool   bFound = FALSE;

    /* Sanity checks */
    if (!pBucketGroup) {
        return;
    }

    /* No sentinels present; do nothing */
    if (!pBucketGroup->pFirst || !pBucketGroup->pLast) {
        return;
    }

    /* Find bucket that contains the first sentinel */
    for (pBucket = pBucketGroup->pHead; pBucket && pBucket->nEntries;
        pBucket = pBucket->pNext) {

        pLower = &pBucket->Entries[0];
        pUpper = &pBucket->Entries[pBucket->nEntries - 1];

        /* Check if the first sentinel is in this bucket */
        if (pBucketGroup->pFirst >= pLower &&
            pBucketGroup->pFirst <= pUpper) {
            /* Low sentinel is in this bucket */
            bFound = TRUE;
            break;
        }
    }

    /* Invalid sentinel; do nothing */
    if (!bFound) {
        return;
    }

    /*  Calculate index for first sentinel
        ASSERT: nStart >= 0 */
    nStart = pBucketGroup->pFirst - pLower;
    if (nStart < 0) {
        return; /* Impossible. */
    }

    /* Start traversing from found bucket */
    for (; pBucket && pBucket->nEntries; pBucket = pBucket->pNext) {
        for (nIndex = nStart; nIndex < pBucket->nEntries; ++nIndex) {
            pBucketData = &pBucket->Entries[nIndex];
            if (pBucketData > pBucketGroup->pLast) {
                return;
            }
            /* Call the user supplied function */
            if (pFunction) {
                pFunction(pBucketGroup,pBucketData);
            }
        }
        /* For next bucket start with first entry */
        nStart = 0;
    }
}

/*  Check if a given (lopc,hipc) are valid for a linkonce.
    We pass in the linkonce  (instead of
    referencing the global pLinkonceInfo) as that means
    searches for pLinkonceInfo find all the uses,
    making understanding of the code a tiny bit easier.
    The section name created is supposed to be the appropriate
    linkonce section name.
*/
Dwarf_Bool IsValidInLinkonce(Bucket_Group *pLo,
    const char *name,Dwarf_Addr lopc,Dwarf_Addr hipc)
{
    static char section_name[SECTION_NAME_LEN];
    Bucket_Data *pBucketData = 0;
    /*  Since text is quite uniformly just this name,
        no need to get it
        from elsewhere, though it will not work for non-elf.  */
    const char *lo_text = ".text.";

    /*  Build the name that represents the linkonce section (.text).
        This is not defined in DWARF so not correct for all
        compilers. */
    struct esb_s sn;

    esb_constructor_fixed(&sn,section_name,sizeof(section_name));
    esb_append(&sn,lo_text);
    esb_append(&sn,name);
    pBucketData = FindNameInBucketGroup(pLo,esb_get_string(&sn));
    esb_destructor(&sn);
    if (pBucketData) {
        if (lopc >= pBucketData->low && lopc <= pBucketData->high) {
            if (hipc >= pBucketData->low &&
                hipc <= pBucketData->high) {
                return TRUE;
            }
        }
    }
    return FALSE;
}
