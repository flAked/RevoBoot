/*
 * Original source code (dsdt_patcher for Chameleon) by mackerintel (2008)
 * Overhaul by Master Chief in 2009.
 *
 * Updates:
 *
 *			- Refactorized for Revolution by DHP in 2010.
 *			- A complete new implementation written for RevoBoot by DHP in 2011.
 *			- Automatic SSDT_PR creation added by DHP in June 2011.
 */


extern ACPI_RSDP * getACPIBaseAddress();


#if PATCH_ACPI_TABLE_DATA

#if AUTOMATIC_SSDT_PR_CREATION
	#include "ssdt_pr_generator.h"
#endif

#if LOAD_EXTRA_ACPI_TABLES && (LOAD_DSDT_TABLE_FROM_EXTRA_ACPI || LOAD_SSDT_TABLE_FROM_EXTRA_ACPI)
//==============================================================================

int loadACPITable(int tableIndex)
{
	_ACPI_DEBUG_DUMP("loadACPITable(%s / ", customTables[tableIndex].name);

	char dirspec[32];
	sprintf (dirspec, "/Extra/ACPI/%s.aml", customTables[tableIndex].name);
	int fd = open(dirspec, 0);

	if (fd < 0)
	{
		_ACPI_DEBUG_DUMP("Error: File not found.)\n");

		return -1;
	}

	int fileSize = file_size(fd);
	void * kernelAddress = malloc(fileSize);

	if (kernelAddress)
	{
		read(fd, kernelAddress, fileSize);
	}

	close(fd);

	_ACPI_DEBUG_DUMP("%d bytes).\n", fileSize);

	customTables[tableIndex].table			= kernelAddress;
	customTables[tableIndex].tableLength	= fileSize;

	return 0;
}


//==============================================================================

void loadACPITables(void)
{
	// DHP: We might want to change this and walk through the /Extra/ACPI/ folder for target tabels.

#if LOAD_DSDT_TABLE_FROM_EXTRA_ACPI
	loadACPITable(DSDT);
#endif // LOAD_DSDT_TABLE_FROM_EXTRA_ACPI

#if LOAD_SSDT_TABLE_FROM_EXTRA_ACPI
	loadACPITable(SSDT);
	loadACPITable(SSDT_PR);	// Overrides STATIC_SSDT_PR_TABLE_DATA / AUTOMATIC_SSDT_PR_CREATION when found!
	loadACPITable(SSDT_USB);
	loadACPITable(SSDT_GPU);
	loadACPITable(SSDT_SATA);
#endif	// LOAD_SSDT_TABLE_FROM_EXTRA_ACPI

}
#endif // LOAD_EXTRA_ACPI_TABLES && (LOAD_DSDT_TABLE_FROM_EXTRA_ACPI || LOAD_SSDT_TABLE_FROM_EXTRA_ACPI)



//==============================================================================

bool replaceTable(ENTRIES * xsdtEntries, int entryIndex, int tableIndex)
{
	// Get the target table type.
	int type = essentialTables[tableIndex].type;

	_ACPI_DEBUG_DUMP("Replacing table %s\n", customTables[type].name);
	_ACPI_DEBUG_DUMP("type: %d\n", type);

#if REPLACE_EXISTING_SSDT_TABLES
	int i = SSDT_GPU;

	// SSDT tables can be replaced with optionally added tables.
	if (type == SSDT && customTables[SSDT].tableAddress == 0)
	{
		// Walk through the optional SSDT tables to find a replacement table.
		for (; i < SSDT_USB; i++)
		{
			// Do we have one?
			if (customTables[i].tableAddress)
			{
				// Yes. Use the index as type (same values).
				type = i;
				break;
			}
		}
	}
#endif	// REPLACE_EXISTING_SSDT_TABLES

	// Check address to prevent a KP.
	if (customTables[type].tableAddress)
	{
		_ACPI_DEBUG_DUMP("Replaced table: %s with custom one.\n", customTables[type].name);

		xsdtEntries[entryIndex] = (uint32_t) customTables[type].tableAddress;

		customTables[type].table = NULL;

		return true;
	}

	return false;
}


//==============================================================================

bool patchFACPTable(ENTRIES * xsdtEntries, int tableIndex, int dropOffset)
{
	_ACPI_DEBUG_DUMP("FACP table patching");
		
	ACPI_FADT *factoryFADT = (ACPI_FADT *)(uint32_t)xsdtEntries[tableIndex];
	ACPI_FADT *patchedFADT = (ACPI_FADT *)AllocateKernelMemory(244); // 0xF4
		
	// The table length should be 244, but may be shorter, or greater than the amount 
	// of allocated kernel memory and thus we check it here (better safe than sorry).
	int tableLength = (factoryFADT->Length < 244) ? factoryFADT->Length : 244;
		
	// Copy factory table into kernel memory as the 'to-be-patched' table replacement.
	memcpy(patchedFADT, factoryFADT, tableLength);
		
	// Update table length (factory table might be too short so fix it).
	patchedFADT->Length = 244;
		
	// Blatantly ignoring the original revision info (using the latest and greatest). 
	patchedFADT->Revision = 4;
		
	// Macro's for some Apple styling. 
	_ACPI_SET_APPLE_OEMID(patchedFADT);
	_ACPI_SET_APPLE_OEMTargetID(patchedFADT);
		
	// Update platform type (ignoring the original value here -= 32 bytes).
	patchedFADT->PM_Profile = gPlatform.Type;
		
	if (gPlatform.CPU.Vendor == 0x756E6547) // Intel only!
	{
		_ACPI_DEBUG_DUMP(", fixed reset GAS.\n");

		// Restart fix (by Duvel300) for ACPI 2.0 and greater.
		// Note: This patch is basically a port of Master Chief's OSXRestart.kext with one 
		//		 addition; The initialization of the reset GAS (when missing).
		patchedFADT->Flags				|= 0x400;
		patchedFADT->ResetSpaceID		= 0x01;
		patchedFADT->ResetBitWidth		= 0x08;
		patchedFADT->ResetBitOffset		= 0x00;
		patchedFADT->ResetAccessWidth	= 0x01;
		patchedFADT->ResetAddress		= 0x0cf9;
		patchedFADT->ResetValue			= 0x06;
	}
#if DEBUG_ACPI
	else
	{
		printf(".\n");	
	}
#endif	// DEBUG_ACPI

		
#if STATIC_DSDT_TABLE_INJECTION || (LOAD_EXTRA_ACPI_TABLES && LOAD_DSDT_TABLE_FROM_EXTRA_ACPI)
	patchedFADT->DSDT = (uint32_t)customTables[DSDT].tableAddress; // The original DSDT without DSDT table injection!
		
	_ACPI_DEBUG_DUMP("Replacing factory DSDT with ");

	if (customTables[DSDT].tableAddress)
	{
#if STATIC_DSDT_TABLE_INJECTION
		_ACPI_DEBUG_DUMP("static DSDT data");
#else
		_ACPI_DEBUG_DUMP("loaded DSDT.aml");
#endif	// STATIC_DSDT_TABLE_INJECTION
		_ACPI_DEBUG_DUMP(" @ 0x%x\n", customTables[DSDT].tableAddress);

		patchedFADT->X_DSDT = (uint32_t)customTables[DSDT].tableAddress;

		customTables[DSDT].table = NULL;
	}
#if DEBUG_ACPI
	else
	{
		_ACPI_DEBUG_DUMP("Failed to locate DSDT replacement!\n");
	}
#endif	// DEBUG_ACPI
	
#endif	// STATIC_DSDT_TABLE_INJECTION || (LOAD_EXTRA_ACPI_TABLES && LOAD_DSDT_TABLE_FROM_EXTRA_ACPI)

#if STATIC_FACS_TABLE_INJECTION
	// Replace the factory FACS table.
	_ACPI_DEBUG_DUMP("patchedFADT->FIRMWARE_CTRL: 0x%08x\n", patchedFADT->FIRMWARE_CTRL);
		
	patchedFADT->FIRMWARE_CTRL = (uint32_t)customTables[FACS].tableAddress;
	// Zero out this field conform the ACPI specification.
	patchedFADT->X_FIRMWARE_CTRL = (uint64_t) 0;

	customTables[FACS].table = NULL;
#endif	// STATIC_FACS_TABLE_INJECTION
	patchedFADT->Checksum = 0;
	patchedFADT->Checksum = 256 - checksum8(patchedFADT, sizeof(ACPI_FADT));
		
	xsdtEntries[tableIndex - dropOffset] = (uint32_t)patchedFADT;

	return true;
}
#endif // PATCH_ACPI_TABLE_DATA


/*==============================================================================
 * Function setupACPI can simply obtain the ACPI base address - for people with 
 * the perfect BIOS on their mainboards - or do more serious work like patching, 
 * updating, replacing and/or injecting new tables.
 *
 * Note: Requires the PATCH_ACPI_TABLE_DATA directive in private_data.h to be 1.
 */

void setupACPI(void)
{
	_ACPI_DEBUG_DUMP("\nEntering setupACPI(%d)\n", PATCH_ACPI_TABLE_DATA);
	
	ACPI_RSDP * factoryRSDP = getACPIBaseAddress();

	_ACPI_DEBUG_DUMP("factoryRSDP: 0x%x\n", factoryRSDP);
	_ACPI_DEBUG_SLEEP(5);

	// _ACPI_DUMP_RSDP_TABLE(factoryRSDP, "Factory");

#if PATCH_ACPI_TABLE_DATA

	ACPI_XSDT * factoryXSDT = (ACPI_XSDT *) ACPI_RXSDT_ADDRESS;

	// _ACPI_DUMP_XSDT_TABLE(factoryXSDT, "Factory");

#if AUTOMATIC_SSDT_PR_CREATION
	generateSSDT_PR();
#endif	// AUTOMATIC_SSDT_PR_CREATION

#if LOAD_EXTRA_ACPI_TABLES
	loadACPITables();
#endif	// LOAD_EXTRA_ACPI_TABLES

	_ACPI_DEBUG_DUMP("\n");

	int cti = 0; // CustomTableIndex

	for (; customTables[cti].table; cti++)
	{
		if (customTables[cti].tableLength)
		{
			_ACPI_DEBUG_DUMP("customTable[%2d] %9s length: %d\n", cti, customTables[cti].name, customTables[cti].tableLength);

			customTables[cti].tableAddress = (void *)AllocateKernelMemory(customTables[cti].tableLength);
			memcpy((void *)customTables[cti].tableAddress, (void *)customTables[cti].table, customTables[cti].tableLength);
			
			// Did we load this table from file?
			if (customTables[cti].loaded)
			{
				// Yes. Return previously allocated memory.
				free(customTables[cti].table);
			}
		}
		else
		{
			// _ACPI_DEBUG_DUMP("No data for: %s (cleared).\n", customTables[cti].name);

			customTables[cti].table = NULL;
		}
		
		// _ACPI_DEBUG_SLEEP(1);
	}

	/*
	 * Main loop with some basic validation checks.
	 *
	 * Note: 196 offers space for twenty 64-bit table addresses.
	 *       196 - 36 = 160 / 8 = 20 tables.
	 */

	if (factoryXSDT && factoryXSDT->Length < 196 && VALID_ADDRESS(factoryRSDP, factoryXSDT))
	{
		int i, dropOffset = 0;

		// Creates a copy of the RSDP aka our to-be-patched-table.
		ACPI_RSDP * patchedRSDP = (ACPI_RSDP *) AllocateKernelMemory(sizeof(ACPI_RSDP));
		memcpy(patchedRSDP, factoryRSDP, RSDP_LENGTH);

		// Keep address for efi.c
		gPlatform.ACPI.BaseAddress = (uint32_t)patchedRSDP;

		// Creates a copy of the XSDP aka our to-be-patched-table.
		ACPI_XSDT * patchedXSDT = (ACPI_XSDT *) AllocateKernelMemory(factoryXSDT->Length);
		memcpy(patchedXSDT, factoryXSDT, factoryXSDT->Length);

		// Pseudo code to fake Apple ID's.
		_ACPI_SET_APPLE_OEMID(patchedRSDP);
		_ACPI_SET_APPLE_OEMID(patchedXSDT);
		_ACPI_SET_APPLE_OEMTargetID(patchedXSDT);

		int entryCount = (patchedXSDT->Length - sizeof(ACPI_XSDT)) / ADDRESS_WIDTH;

		// ACPI 1.0 -> ACPI 2.0 (or initializes patchedRSDP->XsdtAddress).
		updateACPITableData(patchedRSDP, patchedXSDT, entryCount);

		_ACPI_DUMP_RSDP_TABLE(patchedRSDP, "Modified");
		_ACPI_DUMP_XSDT_TABLE(patchedXSDT, "Modified");

		// Gives us a 32/64-bit pointer to the table entries (depends on compiler directives).
		ENTRIES * xsdtEntries = (ENTRIES *) (patchedXSDT + 1);

		_ACPI_DEBUG_DUMP("\nWe have %d entries to work with\n", entryCount);

		// Main loop; walks through the XSDT entries.
		for (i = 0; i < entryCount; i++)
		{
			bool tableMatch = false;

			int idx = 0;

			// Get a pointer to the current table.
			void *table = (void *)((uint32_t)xsdtEntries[i]);

			_ACPI_DEBUG_DUMP_TABLENAME(table, &tableName, i);

			// Get table signature from table.
			uint32_t tableSignature = * (uint32_t *)table;

			// Secondary loop; sequencial search for potential table targets.
			for (idx = 0; essentialTables[idx].tableSignature; idx++)
			{
				// _ACPI_DEBUG_DUMP("idx: %d\n", idx);

				// Check the list with essential tables for a match.
				if (essentialTables[idx].tableSignature == tableSignature)
				{
					tableMatch = true;

					_ACPI_DEBUG_DUMP("(Essential table)\n");

					xsdtEntries[i - dropOffset] = xsdtEntries[i];						
					
					if (essentialTables[idx].action & kPatchTable)
					{
						// Does it have a table action assigned to it?
						if (essentialTables[idx].tableAction)
						{
							_ACPI_DEBUG_DUMP("Calling tableAction(kPatchTable)\n");
							
							// Yes. Call it and let it do its thing.
							essentialTables[idx].tableAction(xsdtEntries, i, dropOffset);
						}
					}
					else if (essentialTables[idx].action & kReplaceTable)
					{
						// Does it have a table action assigned to it?
						if (essentialTables[idx].tableAction)
						{
							int type = essentialTables[idx].type;

							// 
							if (customTables[type].table)
							{
								_ACPI_DEBUG_DUMP("Calling tableAction(kReplaceTable) for %s\n", customTables[type].name);
							
								// Yes. Call it and let it do its thing.
								essentialTables[idx].tableAction(xsdtEntries, (i - dropOffset), idx);
							}
						}
					}
					else if (essentialTables[idx].action & kDropTable)
					{
						_ACPI_DEBUG_DUMP("Dropping table: %s (on request)\n", tableName);
						
						dropOffset++;
					}
				}

				if (tableMatch)
				{
					break;
				}
			}

			if (!tableMatch)
			{
				_ACPI_DEBUG_DUMP("(Dropped. Unused in OS X)\n");

				dropOffset++;
			}
		}

		_ACPI_DEBUG_DUMP("Dropped table count: %d\n", dropOffset);

		// printf("size: %d\n", sizeof(customTables) / sizeof(ACPITable)); // 308

		i = (entryCount - dropOffset);
		int newTableEntries = 0;

		// Now wade through the custom table entries to see if they have been assigned already.
		for (cti = 0; cti < 13; cti++)
		{
			// We need an address so check it.
			if (customTables[cti].table)
			{
				// Do we have a previously dropped table entry available for use?
				if (dropOffset)
				{
					_ACPI_DEBUG_DUMP("Using void XSDT entry[%d] for table: %s.\n", dropOffset, customTables[cti].name);

					// Yes. Reassigning XSDT table entry.
					xsdtEntries[i++] = (uint32_t)customTables[cti].tableAddress;

					customTables[cti].table = NULL;

					// One free spot filled up.
					dropOffset--;
				}
				else // Adding new entries.
				{
					_ACPI_DEBUG_DUMP("Adding new XSDT entry for table: %s.\n", customTables[cti].name);

					// Expand entry table.
					xsdtEntries[i++] = (uint32_t)customTables[cti].tableAddress;

					newTableEntries++;

					// Adjust table length.
					patchedXSDT->Length += ADDRESS_WIDTH;
				}
			}
		}

		_ACPI_DEBUG_DUMP("patchedXSDT->Length (current): %d\n", patchedXSDT->Length);

		// Did we drop tables without using them for new tables?
		if (dropOffset)
		{
			// Yes, fix length.
			patchedXSDT->Length -= (ADDRESS_WIDTH * dropOffset);

			_ACPI_DEBUG_DUMP("patchedXSDT->Length (changed): %d\n", patchedXSDT->Length);
		}

		_ACPI_DEBUG_DUMP("\nRecalculating checksums / ");

		// Patch the checksum of the new XSDP table.
        patchedXSDT->Checksum = 0;
        patchedXSDT->Checksum = 256 - checksum8(patchedXSDT, patchedXSDT->Length);

		// Patch the checksum of the new RSDP table.
		patchedRSDP->Checksum = 0;
		patchedRSDP->Checksum = 256 - checksum8(patchedRSDP, 20);

		// Patch the (extended) checksum of the RSDP for table revision 1 and greater. 
		if (patchedRSDP->Revision)
		{
			patchedRSDP->ExtendedChecksum = 0;
			patchedRSDP->ExtendedChecksum = 256 - checksum8(patchedRSDP, patchedRSDP->Length);
		}
    }

#else // No patching done.

	// Init base address, pointing to newly allocated kernel memory,
	// used to initialize the "ACPI_20" efi->configuration-table.
	gPlatform.ACPI.BaseAddress = (uint32_t)factoryRSDP;

	_ACPI_DEBUG_DUMP("Factory ACPI tables untouched \n");

#endif // PATCH_ACPI_TABLE_DATA

	_ACPI_DEBUG_DUMP("About to exit setupACPI() ");

	_ACPI_DEBUG_SLEEP(20);

}
