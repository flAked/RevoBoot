/*
 * Copyright (c) 1999-2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Portions Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 2.0 (the "License").  You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON- INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1990 Carnegie-Mellon University
 * Copyright (c) 1989 Carnegie-Mellon University
 * All rights reserved.  The CMU software License Agreement specifies
 * the terms and conditions for use and redistribution.
 */

/*
 *          INTEL CORPORATION PROPRIETARY INFORMATION
 *
 *  This software is supplied under the terms of a license  agreement or 
 *  nondisclosure agreement with Intel Corporation and may not be copied 
 *  nor disclosed except in accordance with the terms of that agreement.
 *
 *  Copyright 1988, 1989 by Intel Corporation
 */

/*
 * Copyright 1993 NeXT Computer, Inc. All rights reserved.
 *
 * Completely reworked by Sam Streeper (sam_s@NeXT.com)
 * Reworked again by Curtis Galloway (galloway@NeXT.com)
 */

/* 
 * Refactorized by DHP in 2010 and 2011.
 */


#include "boot.h"
#include "bootstruct.h"
#include "sl.h"
#include "libsa.h"

// DHP: Dump all global junk a.s.a.p.

long gBootMode = kBootModeQuiet; // no longer defaults to 0 aka kBootModeNormal

//==============================================================================
// Local adler32 function.

unsigned long Adler32(unsigned char *buf, long len)
{
	#define BASE 65521L // largest prime smaller than 65536
	#define NMAX 5000
	// NMAX (was 5521) the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1
	
	#define DO1(buf, i)  {s1 += buf[i]; s2 += s1;}
	#define DO2(buf, i)  DO1(buf, i); DO1(buf, i + 1);
	#define DO4(buf, i)  DO2(buf, i); DO2(buf, i + 2);
	#define DO8(buf, i)  DO4(buf, i); DO4(buf, i + 4);
	#define DO16(buf)   DO8(buf, 0); DO8(buf, 8);

	int k;

	unsigned long s1 = 1;	// adler & 0xffff;
	unsigned long s2 = 0;	// (adler >> 16) & 0xffff;
	unsigned long result;

	
	while (len > 0)
	{
		k = len < NMAX ? len : NMAX;
		len -= k;

        while (k >= 16)
		{
			DO16(buf);
			buf += 16;
			k -= 16;
		}

		if (k != 0)
		{
			do
			{
				s1 += *buf++;
				s2 += s1;
			} while (--k);
		}

		s1 %= BASE;
		s2 %= BASE;
	}

	result = (s2 << 16) | s1;

	return OSSwapHostToBigInt32(result);
}


//==============================================================================

static void zeroBSS()
{
    extern char _DATA__bss__begin, _DATA__bss__end;
    extern char _DATA__common__begin, _DATA__common__end;

    bzero( &_DATA__bss__begin, (&_DATA__bss__end - &_DATA__bss__begin) );
    bzero( &_DATA__common__begin, (&_DATA__common__end - &_DATA__common__begin) );
}

/*
 * The SAFE_MALLOC related code (adding 768 bytes) should only be used for 
 * debugging purposes – I have never seen this memory allocation error!
 */

#if SAFE_MALLOC
static void mallocError(char *addr, size_t size, const char *file, int line)
{
    stop("\nMemory allocation error! Addr=0x%x, Size=0x%x, File=%s, Line=%d\n", (unsigned)addr, (unsigned)size, file, line);
}
#else
static void mallocError(char *addr, size_t size)
{
    printf("\nMemory allocation error (0x%x, 0x%x)\n", (unsigned)addr, (unsigned)size);
    asm volatile ("hlt");
}
#endif


//==============================================================================
// Entrypoint from real-mode.

void boot(int biosdev)
{
    zeroBSS();
    mallocInit(0, 0, 0, mallocError);

#if MUST_ENABLE_A20
    // Enable A20 gate before accessing memory above 1 MB.
	if (fastEnableA20() != 0)
	{
		enableA20(); // Fast enable failed. Try legacy method.
	}
#endif

	bool	haveCABootPlist	= false;
    bool	quietBootMode	= true;

	void *binary = (void *)kLoadAddr;

	char bootFile[128];
	char rootUUID[37];

	char * kernelFlags = NULL;

	const char * val;

	int length	= 0;
	int kernelFlagsLength = 0;

	bootFile[0] = '\0';
	rootUUID[0] = '\0';

#if PRE_LINKED_KERNEL_SUPPORT
	bool	mayUseKernelCache	= false;

	long flags, cachetime;
#endif

	initPlatform(biosdev);	// Passing on the boot drive.

#if DEBUG_STATE_ENABLED
	// Don't switch graphics mode / show boot logo when DEBUG is set to 1.
	printf("\ngArchCPUType (CPU): %s\n", (gArchCPUType == CPU_TYPE_X86_64) ? "x86_64" : "i386");
	sleep(1); // Silent sleep.
#else
	showBootLogo();
#endif

	initPartitionChain();

	#define loadCABootPlist() loadSystemConfig(&bootInfo->bootConfig)

	// Loading: /Library/Preferences/SystemConfiguration/com.apple.Boot.plist
	// TODO: Check if everything works <i>without</i> having this plist.
	if (loadCABootPlist() == STATE_SUCCESS)
	{
		_BOOT_DEBUG_DUMP("com.apple.Boot.plist located.\n");

		// Load successful. Change state accordantly.
		haveCABootPlist = true;	// Checked <i>before</i> calling key functions.

		// Check the value of <key>Kernel Flags</key> for stuff we are interested in.
		// Note: We need to know about: arch= and the boot flags: -s, -v, -f and -x
		if (getValueForKey(kKernelFlagsKey, &val, &kernelFlagsLength, &bootInfo->bootConfig))
		{
			// Got it. Do we have anything to work with?
			if (kernelFlagsLength)
			{
				kernelFlagsLength++;

				// Yes. Allocate memory for it and copy the kernel flags into it.
				kernelFlags = malloc(kernelFlagsLength);
				strlcpy(kernelFlags, val, kernelFlagsLength);

				// Is 'arch=<i386/x86_64>' specified as kernel flag?
				if (getValueForBootKey((const char *)kernelFlags, "arch", &val, &length)) //  && len >= 4)
				{
					gArchCPUType = (strncmp(val, "x86_64", 6) == 0) ? CPU_TYPE_X86_64 : CPU_TYPE_I386;

					_BOOT_DEBUG_DUMP("gArchCPUType (c.a.B.plist): %s\n",  (gArchCPUType == CPU_TYPE_X86_64) ? "x86_64" : "i386");
				}
				
				// Check for -v (verbose) and -s (single user mode) flags.
				gVerboseMode =	getValueForBootKey((const char *)kernelFlags, kVerboseModeFlag, &val, &length) || 
								getValueForBootKey((const char *)kernelFlags, kSingleUserModeFlag, &val, &length);
				
				if (gVerboseMode)
				{
#if DEBUG_BOOT == false
					setVideoMode(VGA_TEXT_MODE);
#endif
				}

				// Check for -x (safe) and -f (flush cache) flags.
				if (getValueForBootKey((const char *)kernelFlags, kSafeModeFlag, &val, &length) || 
					getValueForBootKey((const char *)kernelFlags, kIgnoreCachesFlag, &val, &length))
				{
					gBootMode = kBootModeSafe;
				}

				// Is 'boot-uuid=<value>' specified as kernel flag?
				if (getValueForBootKey((const char *)kernelFlags, kBootUUIDKey, &val, &length) && length == 36)
				{
					// Yes. Copy its value into rootUUID.
					strlcpy(rootUUID, val, 37);
				}
			}
		}

		/* Enable touching of a single BIOS device by setting 'Scan Single Drive' to yes.
		if (getBoolForKey(kScanSingleDriveKey, &gScanSingleDrive, &bootInfo->bootConfig) && gScanSingleDrive)
		{
			gScanSingleDrive = true;
		} */
	}
	else
	{
		_BOOT_DEBUG_DUMP("No com.apple.Boot.plist found.\n");
	}
	
	// Was a target drive (per UUID) specified in com.apple.Boot.plist?
	if (rootUUID[0] == '\0')
	{
		_BOOT_DEBUG_DUMP("No UUID specified in com.apple.Boot.plist\n");

		// No, so are we booting from a System Volume?
		if (gPlatform.BootVolume->flags & kBVFlagSystemVolume)
		{
			_BOOT_DEBUG_DUMP("Booting from a System Volume, getting UUID.\n");

			// Yes, then let's get the UUID.
			if (HFSGetUUID(gPlatform.BootVolume, rootUUID) == STATE_SUCCESS)
			{
				_BOOT_DEBUG_DUMP("Success [%s]\n", rootUUID);
			}
		}
		else // Booting from USB-stick or SDboot media.
		{
			_BOOT_DEBUG_DUMP("Booting from a Non System Volume, getting UUID.\n");

			// Get target System Volume and UUID in one go.
			BVRef rootVolume = getTargetRootVolume(rootUUID);

			if (rootVolume)
			{
				_BOOT_DEBUG_DUMP("Success [%s]\n", rootUUID);

				// gPlatform.RootVolume = rootVolume;
			}
		}

		// This should never happen, but just to be sure.
		if (rootUUID[0] == '\0')
		{
			_BOOT_DEBUG_DUMP("Failed to get UUID for System Volume.\n");

			if (!gVerboseMode)
			{
				// Force verbose mode when we didn't find a UUID, so 
				// that people see what is going on in times of trouble.
				gVerboseMode = true;
			}
		}
	}

	/*
	 * At this stage we know exactly what boot mode we're in, and which disk to boot from
	 * any of which may or may not have been set/changed (in com.apple.Boot.plist) into a 
	 * non-default system setting and thus is this the place to update our EFI tree.
	 */

    updateEFITree(rootUUID);

	if (haveCABootPlist) // Check boolean before doing more time consuming tasks.
	{
		if (getBoolForKey(kQuietBootKey, &quietBootMode, &bootInfo->bootConfig) && !quietBootMode)
		{
			gBootMode = kBootModeNormal; // Reversed from: gBootMode |= kBootModeQuiet;
		}
	}

    // Parse args, load and start kernel.
    while (1)
    {
        // Initialize globals.

        sysConfigValid = 0;
        gErrors        = 0;

		int retStatus = -1;
		long hibSleeptime, hibFlags;

		getAndProcessBootArguments(kernelFlags);

		// Initialize bootFile (defaults to: mach_kernel).
		strcpy(bootFile, bootInfo->bootFile);
		
#if HIBERNATE_SUPPORT
		// Wake up from hibernation
		// based on Chameleon code
		while (1) {
			const char *tmp, *path;
			BVRef bvr;
			path = "/private/var/vm/sleepimage";
			
			// Do this first to be sure that root volume is mounted
			retStatus = GetFileInfo(0, path, &hibFlags, &hibSleeptime);

			if ((bvr = getBootVolumeRef(path, &tmp)) == NULL)
				break;
			
			if ((retStatus != 0) || ((hibFlags & kFileTypeMask) != kFileTypeFlat))
				break;
			
			if ((hibSleeptime + 3) < bvr->modTime) {
#if DEBUG_BOOT
				printf ("Hibernate image is too old by %d seconds.\n", bvr->modTime - hibSleeptime);
#endif
				break;
			}
				
			HibernateBoot((char *)val);
			break;
		}
#endif

#if PRE_LINKED_KERNEL_SUPPORT

		// Preliminary checks to prevent us from doing useless things.
        mayUseKernelCache = ((gBootMode & kBootModeSafe) == 0);
		
		/* 
		 * A pre-linked kernel, or kernelcache, requires you to have all essential kexts for your
		 * configuration, including FakeSMC.kext in: /System/Library/Extensions/ 
		 * Not in /Extra/Extensions/ because this directory will be ignored, completely when a 
		 * pre-linked kernel or kernelcache is used!
		 *
		 * Note: Not following this word of advise will render your system incapable of booting!
		 */
		
		if (mayUseKernelCache)
		{
			_BOOT_DEBUG_DUMP("Kernelcache path: %s\n", gPlatform.KernelCachePath);

			/*
			 * We might have been fired up from a USB thumbdrive (kickstart boot) and 
			 * thus we have to check the kernel cache path first (might not be there).
			 */

			if (GetFileInfo(NULL, gPlatform.KernelCachePath, &flags, &cachetime) == 0)
			{
#if MAKE_TARGET_OS == LION // Made for the all new and hip OS of X?

				_BOOT_DEBUG_DUMP("Checking for kernelcache...\n");

				// Do we have a kernelcache to work with?
				if (GetFileInfo(gPlatform.KernelCachePath, (char *)kKernelCache, &flags, &cachetime) == 0)
				{
					/*
					 * Starting with Lion, we can take a shortcut by simply pointing 
					 * the 'bootFile' to the kernel cache and we are done.
					 */
					
					sprintf(bootFile, "%s/%s", gPlatform.KernelCachePath, kKernelCache);

					_BOOT_DEBUG_DUMP("Kernelcache found.\n");
				}

				_BOOT_DEBUG_ELSE_DUMP("Failed to locate the kernelcache. Will load: %s!\n", bootInfo->bootFile);
			}

			_BOOT_DEBUG_ELSE_DUMP("Failed to locate the cache directory!\n");
		}
#else // Not for Lion, go easy with the Snow Leopard.

				static char preLinkedKernelPath[128];
				static char adler32Key[PLATFORM_NAME_LEN + ROOT_PATH_LEN];

				unsigned long adler32 = 0;

				preLinkedKernelPath[0] = '\0';

				_BOOT_DEBUG_DUMP("Checking for pre-linked kernel...\n");

				// Zero out platform info (name and kernel root path).
				bzero(adler32Key, sizeof(adler32Key));
				
				// Construct key for the pre-linked kernel checksum (generated by adler32). 
				sprintf(adler32Key, gPlatform.ModelID);
				sprintf(adler32Key + PLATFORM_NAME_LEN, "%s", BOOT_DEVICE_PATH);
				sprintf(adler32Key + (PLATFORM_NAME_LEN + 38), "%s", bootInfo->bootFile);
				
				adler32 = Adler32((unsigned char *)adler32Key, sizeof(adler32Key));
				
				_BOOT_DEBUG_DUMP("adler32: %08X\n", adler32);
				
				// Create path to pre-linked kernel.
				sprintf(preLinkedKernelPath, "%s/%s_%s.%08lX", gPlatform.KernelCachePath, kKernelCache, 
						((gArchCPUType == CPU_TYPE_X86_64) ? "x86_64" : "i386"), adler32);

				// Check if this file exists.
				if ((GetFileInfo(NULL, preLinkedKernelPath, &flags, &cachetime) == 0) && ((flags & kFileTypeMask) == kFileTypeFlat))
				{
					_BOOT_DEBUG_DUMP("Pre-linked kernel cache located!\nLoading pre-linked kernel: %s\n", preLinkedKernelPath);
					
					// Returns -1 on error, or the actual filesize.
					if (LoadFile((const char *)preLinkedKernelPath))
					{
						retStatus = 1;
						binary = (void *)kLoadAddr;
						bootFile[0] = 0;
					}

					_BOOT_DEBUG_ELSE_DUMP("Failed to load the pre-linked kernel. Will load: %s!\n", bootInfo->bootFile);
				}

				_BOOT_DEBUG_ELSE_DUMP("Failed to locate the pre-linked kernel!\n");
			}

			_BOOT_DEBUG_ELSE_DUMP("Failed to locate the cache directory!\n");
		}
#endif // #if MAKE_TARGET_OS == LION

#endif // PRE_LINKED_KERNEL_SUPPORT

		/*
		 * The bootFile normally points to 'mach_kernel' but it will be empty when a
		 * pre-linked kernel was processed, and that is why we check the length here.
		 */

		if (strlen(bootFile))
		{
			retStatus = LoadThinFatFile(bootFile, &binary);

			if (retStatus <= 0 && gArchCPUType == CPU_TYPE_X86_64)
			{
				_BOOT_DEBUG_DUMP("Load failed for arch=x86_64, trying arch=i386 now.\n");

				gArchCPUType = CPU_TYPE_I386;

				retStatus = LoadThinFatFile(bootFile, &binary);
			}

			_BOOT_DEBUG_DUMP("LoadStatus(%d): %s\n", retStatus, bootFile);
		}

		_BOOT_DEBUG_ELSE_DUMP("bootFile empty!\n");	// Should not happen, but helped me once already.

		/*
		 * Time to fire up the kernel - previously known as execKernel()
		 * Note: retStatus should now be anything but <= 0
		 */

		if (retStatus)
		{
			_BOOT_DEBUG_SLEEP(5);

			_BOOT_DEBUG_DUMP("execKernel-0\n");
			
			entry_t kernelEntry;
			bootArgs->kaddr = bootArgs->ksize = 0;
			
			_BOOT_DEBUG_DUMP("execKernel-1\n");
			
			if (decodeKernel(binary, &kernelEntry, (char **) &bootArgs->kaddr, (int *)&bootArgs->ksize) != 0)
			{
				stop("DecodeKernel() failed!");
			}
			
			_BOOT_DEBUG_DUMP("execKernel-2\n");
			
			// Allocate and copy boot args.
			moveKernelBootArgs();
			
			_BOOT_DEBUG_DUMP("execKernel-3\n");
			
			// Do we need to load kernel MKexts?
			// Skipped when a pre-linked kernel / kernelcache is being used.
			if (gLoadKernelDrivers)
			{
				_BOOT_DEBUG_DUMP("Calling loadDrivers()\n");

				// Yes. Load boot drivers from root path.
				loadDrivers("/");
			}
			
			_BOOT_DEBUG_DUMP("execKernel-4\n");
			
			finalizeEFITree(); // rootUUID);
			
			_BOOT_DEBUG_DUMP("execKernel-5\n");
			
#if DEBUG_BOOT
			if (gErrors)
			{
				printf("Errors encountered while starting up the computer.\n");
				printf("Pausing %d seconds...\n", kBootErrorTimeout);
				sleep(kBootErrorTimeout);
			}
#endif
			
			_BOOT_DEBUG_DUMP("execKernel-6\n");
			
			finalizeKernelBootConfig();
			
			_BOOT_DEBUG_DUMP("execKernel-7 / gVerboseMode is %s\n", gVerboseMode ? "true" : "false");
			
			// Did we switch to graphics mode yet (think verbose mode)?
			if (gVerboseMode || bootArgs->Video.v_display == VGA_TEXT_MODE)
			{
				
				_BOOT_DEBUG_SLEEP(6);
				
				// Switch to graphics mode and show the Apple logo on a gray-ish background.
				showBootLogo(); // formerly drawBootGraphics();
			}
			
			_BOOT_DEBUG_DUMP("execKernel-8\n");

			startMachKernel(kernelEntry, bootArgs); // asm.s
        }

		_BOOT_DEBUG_ELSE_DUMP("Can't find: %s\n", bootFile);

    } /* while(1) */
}
