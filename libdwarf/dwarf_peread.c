/*
Copyright (c) 2018, David Anderson All rights reserved.

Redistribution and use in source and binary forms, with
or without modification, are permitted provided that the
following conditions are met:

    Redistributions of source code must retain the above
    copyright notice, this list of conditions and the following
    disclaimer.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials
    provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


/*  This file reads the parts of a Windows PE
    file appropriate to reading DWARF debugging data.
*/

#include "config.h"
#include <stdio.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif /* HAVE_MALLOC_H */
#include <stdlib.h>
#include <string.h> /* memcpy */
#include <stdlib.h>
#include <sys/types.h> /* open() */
#include <sys/stat.h> /* open() */
#include <fcntl.h> /* open() */
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h> /* lseek read close */
#endif /* HAVE_UNISTD_H */
#include "libdwarf.h"
#include "dwarf_base_types.h"
#include "libdwarfdefs.h"
#include "dwarf_opaque.h"
#include "memcpy_swap.h"
#include "dwarf_error.h" /* for _dwarf_error() declaration */
#include "dwarf_reading.h"
#include "dwarf_object_read_common.h"
#include "dwarf_object_detector.h"
#include "dwarf_pe_descr.h"
#include "dwarf_peread.h"

#ifdef HAVE_UNUSED_ATTRIBUTE
#define  UNUSEDARG __attribute__ ((unused))
#else
#define  UNUSEDARG
#endif

#define DOS_HEADER_LEN 64

#ifndef TYP
#define TYP(n,l) char n[l]
#endif /* TYP */


#ifndef SIZEOFT32
#define SIZEOFT32 4
#endif /* SIZEOFT32 */

static int _dwarf_pe_object_access_init(
    int  fd,
    int   lib_owns_fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    Dwarf_Unsigned access,
    Dwarf_Obj_Access_Interface **binary_interface,
    int *localerrnum);

static unsigned long
magic_copy(char *d, unsigned len)
{
    unsigned i = 0;
    unsigned long v = 0;

    v = d[0];
    for(i = 1 ; i < len; ++i) {
        v <<= 8;
        v |=  0xff&d[i];
    }
    return v;
}

#ifdef WORDS_BIGENDIAN
#define ASNAR(func,t,s)                         \
    do {                                        \
        unsigned tbyte = sizeof(t) - sizeof(s); \
        t = 0;                                  \
        func(((char *)&t)+tbyte ,&s[0],sizeof(s));  \
    } while (0)
#else /* LITTLE ENDIAN */
#define ASNAR(func,t,s)                         \
    do {                                        \
        t = 0;                                  \
        func(&t,&s[0],sizeof(s));               \
    } while (0)
#endif /* end LITTLE- BIG-ENDIAN */

/*  name_array is 8 byte string */
static int
pe_section_name_get(dwarf_pe_object_access_internals_t *pep,
    const char *name_array,
    const char ** name_out,
    int *errcode)
{

    if (name_array[0] == '/') {
        long v = 0;
        unsigned long u = 0;
        const char *s = 0;
        char temp_array[9];

        memcpy(temp_array,name_array+1,7);
        temp_array[7] = 0;
        v = atoi(temp_array);
        if (v < 0) {
            *errcode = DW_DLE_STRING_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        u = v;
        if (u > pep->pe_string_table_size) {
            *errcode = DW_DLE_STRING_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        s = pep->pe_string_table +u;
        *name_out = s;
        return DW_DLV_OK;
    }
    *name_out = name_array;
    return DW_DLV_OK;
}


static Dwarf_Endianness
pe_get_byte_order (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_byteorder;
}


static Dwarf_Small
pe_get_length_size (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_offsetsize/8;
}

static Dwarf_Small
pe_get_pointer_size (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_pointersize/8;
}


static Dwarf_Unsigned
pe_get_section_count (void *obj)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);
    return pep->pe_section_count;
}

static int
pe_get_section_info (void *obj,
    Dwarf_Half section_index,
    Dwarf_Obj_Access_Section *return_section,
    UNUSEDARG int *error)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);


    if (section_index < pep->pe_section_count) {
        struct dwarf_pe_generic_image_section_header *sp = 0;
        sp = pep->pe_sectionptr + section_index;
        return_section->addr = 0;
        return_section->type = 0;
        return_section->size = sp->SizeOfRawData;
        return_section->name = sp->dwarfsectname;
        return_section->link = 0;
        return_section->info = 0;
        return_section->entrysize = 0;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}


static int
load_optional_header32(dwarf_pe_object_access_internals_t *pep,
    Dwarf_Unsigned offset, int*errcode)
{
    int res = 0;
    IMAGE_OPTIONAL_HEADER32 hdr;

    pep->pe_optional_header_size = sizeof(IMAGE_OPTIONAL_HEADER32);

    if ((pep->pe_optional_header_size + offset) >
        pep->pe_filesize) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }

    res =  _dwarf_object_read_random(pep->pe_fd,
        (char *)&hdr,
        offset, sizeof(IMAGE_OPTIONAL_HEADER32), errcode);
    if (res != DW_DLV_OK) {
        return res;
    }

    /* This is a subset of fields. */
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.Magic,
        hdr.Magic);
    pep->pe_OptionalHeader.MajorLinkerVersion= hdr.MajorLinkerVersion;
    pep->pe_OptionalHeader.MinorLinkerVersion= hdr.MinorLinkerVersion;
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfCode,
        hdr.SizeOfCode);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfImage,
        hdr.SizeOfImage);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfHeaders,
        hdr.SizeOfHeaders);
    pep->pe_OptionalHeader.SizeOfDataDirEntry =
        sizeof(IMAGE_DATA_DIRECTORY);
    return DW_DLV_OK;
}
static int
load_optional_header64(dwarf_pe_object_access_internals_t *pep,
    Dwarf_Unsigned offset, int*errcode )
{
    IMAGE_OPTIONAL_HEADER64 hdr;
    int res = 0;

    pep->pe_optional_header_size = sizeof(IMAGE_OPTIONAL_HEADER64);
    if ((pep->pe_optional_header_size + offset) >
        pep->pe_filesize) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }
    res =  _dwarf_object_read_random(pep->pe_fd,
        (char *)&hdr,
        offset, sizeof(IMAGE_OPTIONAL_HEADER64), errcode);
    if (res != DW_DLV_OK) {
        return res;
    }

    /* This is a subset of fields. */
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.Magic,
        hdr.Magic);
    pep->pe_OptionalHeader.MajorLinkerVersion= hdr.MajorLinkerVersion;
    pep->pe_OptionalHeader.MinorLinkerVersion= hdr.MinorLinkerVersion;
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfCode,
        hdr.SizeOfCode);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfImage,
        hdr.SizeOfImage);
    ASNAR(pep->pe_copy_word,pep->pe_OptionalHeader.SizeOfHeaders,
        hdr.SizeOfHeaders);
    pep->pe_OptionalHeader.SizeOfDataDirEntry =
        sizeof(IMAGE_DATA_DIRECTORY);
    return DW_DLV_OK;
}

static int
pe_load_section (void *obj, Dwarf_Half section_index,
    Dwarf_Small **return_data, int *error)
{
    dwarf_pe_object_access_internals_t *pep =
        (dwarf_pe_object_access_internals_t*)(obj);

    if (0 < section_index &&
        section_index < pep->pe_section_count) {
        int res = 0;

        struct dwarf_pe_generic_image_section_header *sp =
            pep->pe_sectionptr + section_index;
        if(sp->loaded_data) {
            *return_data = sp->loaded_data;
            return DW_DLV_OK;
        }
        if (!sp->SizeOfRawData) {
            return DW_DLV_NO_ENTRY;
        }
        if ((sp->SizeOfRawData + sp->PointerToRawData) >
            pep->pe_filesize) {
            *error = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        sp->loaded_data = malloc(sp->SizeOfRawData);
        if(!sp->loaded_data) {
            *error = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        res = _dwarf_object_read_random(pep->pe_fd,
            (char *)sp->loaded_data,
            sp->PointerToRawData, sp->SizeOfRawData, error);
        if (res != DW_DLV_OK) {
            free(sp->loaded_data);
            sp->loaded_data = 0;
            return res;
        }
        *return_data = sp->loaded_data;
        return DW_DLV_OK;
    }
    return DW_DLV_NO_ENTRY;
}

void
_dwarf_destruct_pe_access(
    struct Dwarf_Obj_Access_Interface_s *aip)
{
    dwarf_pe_object_access_internals_t *pep = 0;
    Dwarf_Unsigned i = 0;

    if (!aip) {
        return;
    }
    pep = (dwarf_pe_object_access_internals_t*)(aip->object);
    if (pep->pe_destruct_close_fd) {
        close(pep->pe_fd);
        pep->pe_fd = -1;
    }
    free((char *)pep->pe_path);
    pep->pe_path = 0;
    if (pep->pe_sectionptr) {
        struct dwarf_pe_generic_image_section_header  *sp = 0;

        sp = pep->pe_sectionptr;
        for( i=0; i < pep->pe_section_count; ++i,++sp) {
            if (sp->loaded_data) {
                free(sp->loaded_data);
                sp->loaded_data = 0;
            }
            free(sp->name);
            sp->name = 0;
            free(sp->dwarfsectname);
            sp->dwarfsectname = 0;
        }
        free(pep->pe_sectionptr);
        pep->pe_section_count = 0;
    }
    free(pep->pe_string_table);
    pep->pe_string_table = 0;
    free(pep);
    free(aip);
    return;
}


static int
dwarf_pe_load_dwarf_section_headers(
    dwarf_pe_object_access_internals_t *pep,int *errcode)
{
    Dwarf_Unsigned i = 0;
    Dwarf_Unsigned input_count =
        pep->pe_FileHeader.NumberOfSections;
    Dwarf_Unsigned offset_in_input = pep->pe_section_table_offset;
    Dwarf_Unsigned section_hdr_size = sizeof(IMAGE_SECTION_HEADER);
    struct dwarf_pe_generic_image_section_header *sec_outp = 0;
    Dwarf_Unsigned cur_offset = offset_in_input;
    Dwarf_Unsigned past_end_hdrs = offset_in_input +
        section_hdr_size*input_count;

    /* internal sections include null initial section */
    pep->pe_section_count = input_count+1;

    if (past_end_hdrs > pep->pe_filesize) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        return DW_DLV_ERROR;
    }

    if (!offset_in_input) {
        *errcode = DW_DLE_PE_OFFSET_BAD;
        return DW_DLV_ERROR;
    }
    pep->pe_sectionptr =
        (struct dwarf_pe_generic_image_section_header * )
        calloc(pep->pe_section_count,
        sizeof(struct dwarf_pe_generic_image_section_header));



    if (!pep->pe_sectionptr) {
        *errcode = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    sec_outp = pep->pe_sectionptr;
    sec_outp->name = strdup("");
    sec_outp->dwarfsectname = strdup("");
    sec_outp++;
    for ( ;  i < input_count;
        ++i, cur_offset += section_hdr_size, sec_outp++) {

        int res = 0;
        IMAGE_SECTION_HEADER filesect;
        char safe_name[9];
        const char *expname = 0;

        res =  _dwarf_object_read_random(pep->pe_fd,
            (char *)&filesect,cur_offset,
            sizeof(filesect),errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
        strncpy(safe_name,filesect.Name,sizeof(filesect.Name));
        safe_name[8] = 0;
        sec_outp->name = strdup(safe_name);
        res = pe_section_name_get(pep,
            safe_name,&expname,errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
        sec_outp->dwarfsectname = strdup(expname);

        if ( !sec_outp->name || !sec_outp->dwarfsectname) {
            *errcode = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        sec_outp->SecHeaderOffset = cur_offset;
        ASNAR(pep->pe_copy_word,sec_outp->VirtualSize,
            filesect.Misc.VirtualSize);
        ASNAR(pep->pe_copy_word,sec_outp->VirtualAddress,
            filesect.VirtualAddress);
        ASNAR(pep->pe_copy_word,sec_outp->SizeOfRawData,
            filesect.SizeOfRawData);
        ASNAR(pep->pe_copy_word,sec_outp->PointerToRawData,
            filesect.PointerToRawData);
        if(sec_outp->SizeOfRawData > pep->pe_filesize ||
            sec_outp->PointerToRawData > pep->pe_filesize ||
            (sec_outp->SizeOfRawData+
                sec_outp->PointerToRawData > pep->pe_filesize)) {
            *errcode = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        ASNAR(pep->pe_copy_word,sec_outp->PointerToRelocations,
            filesect.PointerToRelocations);
        ASNAR(pep->pe_copy_word,sec_outp->PointerToLinenumbers,
            filesect.PointerToLinenumbers);
        ASNAR(pep->pe_copy_word,sec_outp->NumberOfRelocations,
            filesect.NumberOfRelocations);
        ASNAR(pep->pe_copy_word,sec_outp->NumberOfLinenumbers,
            filesect.NumberOfLinenumbers);
        ASNAR(pep->pe_copy_word,sec_outp->Characteristics,
            filesect.Characteristics);
        /* sec_outp->loaded data set when we load a section */
    }
    return DW_DLV_OK;
}


static int
dwarf_load_pe_sections(
    dwarf_pe_object_access_internals_t *pep,int *errcode)
{
    struct dos_header dhinmem;
    IMAGE_FILE_HEADER ifh;
    void (*word_swap) (void *, const void *, unsigned long);
    unsigned locendian = 0;
    int res = 0;
    Dwarf_Unsigned dos_sig = 0;
    Dwarf_Unsigned nt_address = 0;
    char nt_sig_array[4];
    unsigned long nt_signature = 0;

    if ( (sizeof(ifh) + sizeof(dhinmem))  >= pep->pe_filesize) {
        /* corrupt object. */
        *errcode = DW_DLE_PE_SIZE_SMALL;
        return DW_DLV_ERROR;
    }
    res = _dwarf_object_read_random(pep->pe_fd,(char *)&dhinmem,
        0, sizeof(dhinmem), errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    dos_sig = magic_copy((char *)dhinmem.dh_mz,
        sizeof(dhinmem.dh_mz));
    if (dos_sig == IMAGE_DOS_SIGNATURE) {
        /*  IMAGE_DOS_SIGNATURE assumes bytes reversed by little-endian
            load, so we intrepet a match the other way. */
        /* BIG ENDIAN. From looking at hex characters in object  */
#ifdef WORDS_BIGENDIAN
        word_swap = _dwarf_memcpy_noswap_bytes;
#else  /* LITTLE ENDIAN */
        word_swap = _dwarf_memcpy_swap_bytes;
#endif /* LITTLE- BIG-ENDIAN */
        locendian = DW_ENDIAN_BIG;
    } else if (dos_sig == IMAGE_DOS_REVSIGNATURE) {
        /* raw load, so  intrepet a match the other way. */
        /* LITTLE ENDIAN */
#ifdef WORDS_BIGENDIAN
        word_swap = _dwarf_memcpy_swap_bytes;
#else   /* LITTLE ENDIAN */
        word_swap = _dwarf_memcpy_noswap_bytes;
#endif  /* LITTLE- BIG-ENDIAN */
        locendian = DW_ENDIAN_LITTLE;
    } else {
        /* Not dos header not a PE file we recognize */
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }
    if (locendian != pep->pe_byteorder) {
        /*  Really this is a coding botch somewhere here,
            not an object corruption. */
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }
    pep->pe_copy_word = word_swap;
    ASNAR(word_swap,nt_address,dhinmem.dh_image_offset);
    if (pep->pe_filesize < (nt_address + sizeof(nt_sig_array))) {
        /*  The nt_address is really a file offset. */
        *errcode = DW_DLE_FILE_TOO_SMALL;
        /* Not dos header not a PE file we recognize */
        return DW_DLV_ERROR;
    }

    res =  _dwarf_object_read_random(pep->pe_fd,
        (char *)&nt_sig_array[0],
        nt_address, sizeof(nt_sig_array),errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    {   unsigned long lsig = 0;

        ASNAR(word_swap,lsig,nt_sig_array);
        nt_signature = lsig;
    }
    if (nt_signature != IMAGE_NT_SIGNATURE) {
        *errcode = DW_DLE_FILE_WRONG_TYPE;
        return DW_DLV_ERROR;
    }

    pep->pe_nt_header_offset = nt_address  + SIZEOFT32;
    if (pep->pe_filesize < (pep->pe_nt_header_offset +
        sizeof(ifh))) {
        *errcode = DW_DLE_FILE_TOO_SMALL;
        /* Not image header not a PE file we recognize */
        return DW_DLV_ERROR;
    }
    res = _dwarf_object_read_random(pep->pe_fd,(char *)&ifh,
        pep->pe_nt_header_offset, sizeof(ifh), errcode);
    if (res != DW_DLV_OK) {
        return res;
    }
    ASNAR(word_swap,pep->pe_FileHeader.Machine,ifh.Machine);
    ASNAR(word_swap,pep->pe_FileHeader.NumberOfSections,
        ifh.NumberOfSections);
    ASNAR(word_swap,pep->pe_FileHeader.TimeDateStamp,
        ifh.TimeDateStamp);
    ASNAR(word_swap,pep->pe_FileHeader.PointerToSymbolTable,
        ifh.PointerToSymbolTable);
    ASNAR(word_swap,pep->pe_FileHeader.NumberOfSymbols,
        ifh.NumberOfSymbols);
    ASNAR(word_swap,pep->pe_FileHeader.SizeOfOptionalHeader,
        ifh.SizeOfOptionalHeader);
    ASNAR(word_swap,pep->pe_FileHeader.Characteristics,
        ifh.Characteristics);

    pep->pe_optional_header_offset = pep->pe_nt_header_offset+
        sizeof(ifh);
    if (pep->pe_offsetsize == 32) {
        res = load_optional_header32(pep,
            pep->pe_optional_header_offset,errcode);
        pep->pe_optional_header_size = sizeof(IMAGE_OPTIONAL_HEADER32);
    } else if (pep->pe_offsetsize == 64) {
        res = load_optional_header64(pep,
            pep->pe_optional_header_offset,errcode);
        pep->pe_optional_header_size = sizeof(IMAGE_OPTIONAL_HEADER64);
    } else {
        *errcode = DW_DLE_OFFSET_SIZE;
        return DW_DLV_ERROR;
    }
    if (res != DW_DLV_OK) {
        return res;
    }

    pep->pe_section_table_offset = pep->pe_optional_header_offset
        + pep->pe_optional_header_size;
    pep->pe_symbol_table_offset =
        pep->pe_FileHeader.PointerToSymbolTable;
    if (pep->pe_symbol_table_offset >= pep->pe_filesize) {
        *errcode = DW_DLE_OFFSET_SIZE;
        return DW_DLV_ERROR;
    }
    if (pep->pe_symbol_table_offset) {
        pep->pe_string_table_offset  =
            pep->pe_symbol_table_offset +
            (pep->pe_FileHeader.NumberOfSymbols *
            IMAGE_SIZEOF_SYMBOL);
    }

    if (pep->pe_string_table_offset >= pep->pe_filesize) {
        *errcode = DW_DLE_OFFSET_SIZE;
        pep->pe_string_table_size = 0;
        return DW_DLV_ERROR;
    }
    if (pep->pe_string_table_offset) {
        /*  https://docs.microsoft.com/en-us/windows/desktop/debug/pe-format#coff-string-table  */
        /* The first 4 bytes of the string table contain
            the size of the string table. */
        char size_field[4];

        if ((pep->pe_string_table_offset+sizeof(size_field)) >
            pep->pe_filesize) {
            *errcode = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        memset(size_field,0,sizeof(size_field));
        res =  _dwarf_object_read_random(pep->pe_fd,
            (char *)size_field, pep->pe_string_table_offset,
            sizeof(size_field), errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
        ASNAR(pep->pe_copy_word,pep->pe_string_table_size,
            size_field);
        if( pep->pe_string_table_size >= pep->pe_filesize ) {
            *errcode = DW_DLE_PE_OFFSET_BAD;
            return DW_DLV_ERROR;
        }
        if ((pep->pe_string_table_offset+pep->pe_string_table_size) >
            pep->pe_filesize) {
            *errcode = DW_DLE_FILE_TOO_SMALL;
            return DW_DLV_ERROR;
        }
        pep->pe_string_table =
            (char *)malloc(pep->pe_string_table_size);
        if (!pep->pe_string_table) {
            *errcode = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        res = _dwarf_object_read_random(pep->pe_fd,
            (char *)pep->pe_string_table, pep->pe_string_table_offset,
            pep->pe_string_table_size,errcode);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    res = dwarf_pe_load_dwarf_section_headers(pep,errcode);
    return res;
}

int
_dwarf_pe_setup(int fd,
    char *true_path,
    int lib_owns_fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    Dwarf_Unsigned access,
    unsigned groupnumber,
    Dwarf_Handler errhand,
    Dwarf_Ptr errarg,
    Dwarf_Debug *dbg,Dwarf_Error *error)
{
    Dwarf_Obj_Access_Interface *binary_interface = 0;
    dwarf_pe_object_access_internals_t *pep = 0;
    int res = DW_DLV_OK;
    int localerrnum = 0;

    res = _dwarf_pe_object_access_init(
        fd,
        lib_owns_fd,
        ftype,endian,offsetsize,filesize,access,
        &binary_interface,
        &localerrnum);
    if (res != DW_DLV_OK) {
        if (res == DW_DLV_NO_ENTRY) {
            return res;
        }
        _dwarf_error(NULL, error, localerrnum);
        return DW_DLV_ERROR;
    }
    /*  allocates and initializes Dwarf_Debug,
        generic code */
    res = dwarf_object_init_b(binary_interface, errhand, errarg,
        groupnumber, dbg, error);
    if (res != DW_DLV_OK){
        _dwarf_destruct_pe_access(binary_interface);
        return res;
    }
    pep = binary_interface->object;
    pep->pe_path = strdup(true_path);
    return res;
}

static Dwarf_Obj_Access_Methods pe_methods = {
    pe_get_section_info,
    pe_get_byte_order,
    pe_get_length_size,
    pe_get_pointer_size,
    pe_get_section_count,
    pe_load_section,
    0 /* ignore pe relocations. */
};

static int
_dwarf_pe_object_access_internals_init(
    dwarf_pe_object_access_internals_t * internals,
    int  fd,
    int   lib_owns_fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    UNUSEDARG Dwarf_Unsigned access,
    int *errcode)
{
    dwarf_pe_object_access_internals_t * intfc = internals;
    struct Dwarf_Obj_Access_Interface_s *localdoas;
    int res = 0;

    /*  Must malloc as _dwarf_destruct_pe_access()
        forces that due to other uses. */
    localdoas = (struct Dwarf_Obj_Access_Interface_s *)
        malloc(sizeof(struct Dwarf_Obj_Access_Interface_s));
    if (!localdoas) {
        *errcode = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    memset(localdoas,0,sizeof(struct Dwarf_Obj_Access_Interface_s));
    intfc->pe_ident[0]    = 'P';
    intfc->pe_ident[1]    = '1';
    intfc->pe_fd          = fd;
    intfc->pe_destruct_close_fd = lib_owns_fd;
    intfc->pe_is_64bit    = ((offsetsize==64)?TRUE:FALSE);
    intfc->pe_offsetsize  = offsetsize;
    intfc->pe_pointersize = offsetsize;
    intfc->pe_filesize    = filesize;
    intfc->pe_ftype       = ftype;
    /* pe_path set by caller */

#ifdef WORDS_BIGENDIAN
    if (endian == DW_ENDIAN_LITTLE ) {
        intfc->pe_copy_word = _dwarf_memcpy_swap_bytes;
        intfc->pe_byteorder = DW_ENDIAN_LITTLE;
    } else {
        intfc->pe_copy_word = _dwarf_memcpy_noswap_bytes;
        intfc->pe_byteorder = DW_ENDIAN_BIG;
    }
#else  /* LITTLE ENDIAN */
    if (endian == DW_ENDIAN_LITTLE ) {
        intfc->pe_copy_word = _dwarf_memcpy_noswap_bytes;
        intfc->pe_byteorder = DW_ENDIAN_LITTLE;
    } else {
        intfc->pe_copy_word = _dwarf_memcpy_swap_bytes;
        intfc->pe_byteorder = DW_ENDIAN_BIG;
    }
#endif /* LITTLE- BIG-ENDIAN */
    res = dwarf_load_pe_sections(intfc,errcode);
    if (res != DW_DLV_OK) {
        localdoas = (struct Dwarf_Obj_Access_Interface_s *)
            malloc(sizeof(struct Dwarf_Obj_Access_Interface_s));
        if (!localdoas) {
            *errcode = DW_DLE_ALLOC_FAIL;
            return DW_DLV_ERROR;
        }
        localdoas->object = intfc;
        localdoas->methods = 0;
        _dwarf_destruct_pe_access(localdoas);
        return res;
    }
    free(localdoas);
    return DW_DLV_OK;
}


static int
_dwarf_pe_object_access_init(
    int  fd,
    int   lib_owns_fd,
    unsigned ftype,
    unsigned endian,
    unsigned offsetsize,
    size_t filesize,
    Dwarf_Unsigned access,
    Dwarf_Obj_Access_Interface **binary_interface,
    int *localerrnum)
{

    int res = 0;
    dwarf_pe_object_access_internals_t *internals = 0;
    Dwarf_Obj_Access_Interface *intfc = 0;

    internals = malloc(sizeof(dwarf_pe_object_access_internals_t));
    if (!internals) {
        *localerrnum = DW_DLE_ALLOC_FAIL;
        /* Impossible case, we hope. Give up. */
        return DW_DLV_ERROR;
    }
    memset(internals,0,sizeof(*internals));
    res = _dwarf_pe_object_access_internals_init(internals,
        fd,
        lib_owns_fd,
        ftype, endian, offsetsize, filesize,
        access,
        localerrnum);
    if (res != DW_DLV_OK){
        /* *err is already set. */
        free(internals);
        return DW_DLV_ERROR;
    }
    internals->pe_destruct_close_fd = lib_owns_fd;

    intfc = malloc(sizeof(Dwarf_Obj_Access_Interface));
    if (!intfc) {
        /* Impossible case, we hope. Give up. */
        free(internals);
        *localerrnum = DW_DLE_ALLOC_FAIL;
        return DW_DLV_ERROR;
    }
    /* Initialize the interface struct */
    intfc->object = internals;
    intfc->methods = &pe_methods;
    *binary_interface = intfc;
    return DW_DLV_OK;
}