/*
 *  resume.c
 *  
 *
 *  Created by mackerintel on 1/22/09.
 *  Copyright 2009 mackerintel. All rights reserved.
 *
 *  - small fixups by flAked, 2011
 */



#include "saio_internal.h"
#include "libsa.h"
#include "IOHibernatePrivate.h"
#include "memory.h"
#include "bootstruct.h"
#include "boot.h"
#include "pci.h"

#if HIBERNATE_SUPPORT

/*extern int previewTotalSectors;
extern int previewLoadedSectors;
extern uint8_t *previewSaveunder;*/

static unsigned long
getmemorylimit(void)
{
  int line;
  int i;
  MemoryRange *mp = bootInfo->memoryMap;
  
  // Activate and clear page 1
  line = 1;
  for (i = 0; i < bootInfo->memoryMapCount; i++)
  {
    if((mp->type == 1) && ((unsigned long)mp->base == 0x100000))
    {
      return (unsigned long)(mp->base + mp->length);
    }
    mp++;
  }
  return 0x10000000;
}

static void WakeKernel(IOHibernateImageHeader * header)
{
	uint32_t proc;
	unsigned long cnt, newSP;
	unsigned long *src, *dst;
	unsigned int 	count;
	unsigned int 	page;
	unsigned int 	compressedSize;
	int32_t   	byteCnt;
	u_int32_t 	lowHalf, highHalf;
	u_int32_t 	sum;
	
	printf("\nRestoring hibernation image...\n");
	
	dst   = (unsigned long *) (header->restore1CodePage << 12);
	count = header->restore1PageCount;
	proc  = (header->restore1CodeOffset + ((uint32_t) dst));
	newSP = header->restore1StackOffset + (header->restore1CodePage << 12);
	
	src  = (unsigned long *) (((u_int32_t) &header->fileExtentMap[0]) 
							  + header->fileExtentMapSize);
	sum  = 0;
	
	for (page = 0; page < count; page++)
	{
		compressedSize = 4096;
		
		lowHalf = 1;
		highHalf = 0;
		
		for (cnt = 0; cnt < compressedSize; cnt += 0x20) {
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = src[3];
			dst[4] = src[4];
			dst[5] = src[5];
			dst[6] = src[6];
			dst[7] = src[7];
			for (byteCnt = 0; byteCnt < 0x20; byteCnt++) {
				lowHalf += ((u_int8_t *) dst)[byteCnt];
				highHalf += lowHalf;
			}
			src += 8;
			dst += 8;
		}
		
		lowHalf  %= 65521L;
		highHalf %= 65521L;
		sum += (highHalf << 16) | lowHalf;
	}
	header->actualRestore1Sum = sum;
	startMachKernel (proc, header);
	
	return;
}

void HibernateBoot(char *image_filename)
{
	long long size, imageSize, codeSize, allocSize;
	long mem_base;
	IOHibernateImageHeader _header;
	IOHibernateImageHeader * header = &_header;
	long buffer;
	
	size = ReadFileAtOffset (image_filename, header, 0, sizeof(IOHibernateImageHeader));
	
#if DEBUG_BOOT
	printf("header read size %x\n", size);
#endif	

	imageSize = header->image1Size;
	codeSize  = header->restore1PageCount << 12;
	if (kIOHibernateHeaderSignature != header->signature)
	{
		printf ("Incorrect image signature\n");
		return;
	}
	if (header->encryptStart)
	{
		printf ("Resuming from Encrypted image is unsupported.\n"
				"Uncheck \"Use secure virtual memory\" in \"Security\" pane on system preferences.\n"
				"Press any key to proceed with normal boot.\n");
		getchar ();
		return;
	}
	
	allocSize = imageSize + ((4095 + sizeof(hibernate_graphics_t)) & ~4095);
	
	mem_base = getmemorylimit() - allocSize;//TODO: lower this
	
#if DEBUG_BOOT	
	printf("mem_base %x\n", mem_base);
#endif

	// Rek : hibernate fix 
	if (!((long long)mem_base+allocSize<1024*bootInfo->extmem+0x100000))
	{
		printf ("Not enough space to restore image. Press any key to proceed with normal boot.\n");
		getchar ();
		return;
	}
	
	bcopy(header, (void *) mem_base, sizeof(IOHibernateImageHeader));
	header = (IOHibernateImageHeader *) mem_base;
	
	imageSize -= sizeof(IOHibernateImageHeader);
	buffer = (long)(header + 1);
	
	/*if (header->previewSize)
	{
		uint64_t preview_offset = header->fileExtentMapSize - sizeof(header->fileExtentMap) + codeSize;
		uint8_t progressSaveUnder[kIOHibernateProgressCount][kIOHibernateProgressSaveUnderSize];
		
		ReadFileAtOffset (image_filename, (char *)buffer, sizeof(IOHibernateImageHeader), preview_offset+header->previewSize);
		//drawPreview ((void *)(long)(buffer+preview_offset + header->previewPageListSize), &(progressSaveUnder[0][0]));
		previewTotalSectors = (imageSize-(preview_offset+header->previewSize))/512;
		previewLoadedSectors = 0;
		previewSaveunder = &(progressSaveUnder[0][0]);
		if (preview_offset+header->previewSize<imageSize)
			ReadFileAtOffset (image_filename, (char *)(long)(buffer+preview_offset+header->previewSize),
							  sizeof(IOHibernateImageHeader)+preview_offset+header->previewSize,
							  imageSize-(preview_offset+header->previewSize));
		previewTotalSectors = 0;
		previewLoadedSectors = 0;
		previewSaveunder = 0;
	}
	else*/
		ReadFileAtOffset (image_filename, (char *)buffer, sizeof(IOHibernateImageHeader), imageSize);
	
	WakeKernel(header);
}

#endif
