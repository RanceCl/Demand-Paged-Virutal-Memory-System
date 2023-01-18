/* 
CPSC/ECE 3220, Spring 2021
Roderick "Rance" White
Program 4

The following program functions as a demand-paged virtual memory system.
The command argument -v will cause the code to be performed verbose.
*/



#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#define MAXLINE 256
#define PAGE_TABLE_ENTRIES 65536


/* Structure for Page Table Entries */
typedef struct{
	unsigned char presence;		/* could be single bit */
	unsigned short pad;							//Not used?
	unsigned char pfn;			/* 8 bits */
} PTE_t;

/* Structure for TLB Entries */
typedef struct{
	unsigned char valid;		/* could be single bit */
	unsigned short vpn;			/* 16 bits */		//Not used?
	unsigned char pfn;			/* 8 bits */
} TLBE_t;

/* Structure for Core Map Entries */
typedef struct{
	unsigned char valid;		/* could be single bit */
	unsigned char use_vector;	/* 8 bits for pseudo-LRU replacement */
	unsigned short vpn;			/* 16 bits */
} CME_t;

/* Global variables */
int FIFOIndex = 0;
int access_count=0;
int tlb_miss_count=0;
int page_fault_count=0;
int Verbose = 0;

/* Function declarations */
void AddressTranslation(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, int UP);
int TLBHitCheck(TLBE_t *TLBE, int TLBN, unsigned int VA);
void TLBMissFunc(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, unsigned int VA);
void UpdateTLB(TLBE_t *TLBE, int TLBN, unsigned int va, unsigned int pa);

void PageHitFunc(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, unsigned int VA);
void PageFaultFunc(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, unsigned int VA);

void CMUseVectorUpdate(CME_t *CME, int CMN, unsigned int PFN);
int FindFreeFrame(CME_t *CME, int CMN);
int FindLowestUseFrame(CME_t *CME, int CMN);
void FreeFrameFound(TLBE_t *TLBE,int TLBN,PTE_t *PTE,CME_t *CME,int CMN,unsigned int VA,int Index);
void LowUseFound(TLBE_t *TLBE,int TLBN,PTE_t *PTE,CME_t *CME,int CMN,unsigned int VA,int Index);

int ConfigureFileRead(FILE *fp, int ReadNum);
void InitializePageTable(PTE_t *PTE);
void IntializeTLB(TLBE_t *TLBE, int TLBN);
void InitializeCoreMap(CME_t *CME, int CMN);

void PrintPageTable(PTE_t *PTE);
void PrintTLB(TLBE_t *TLBE, int TLBN);
void PrintCoreMap(CME_t *CME, int CMN);
void DebugPrint(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, int UP);

int main(int argc, char **argv)
{
	FILE *fp;
	int c, PageFraNum, TLBEntNum, ObsPeriod;
	PTE_t *PageTable;
	TLBE_t *TLB;
	CME_t *CoreMap;

	/* Look through command line to see if verbose */
	while ((c = getopt(argc, argv, "v")) != -1)
		if(c == 'v')
			Verbose = 1;

	/* Read information from file */
	fp = fopen("paging.cfg", "r");								//Opens file for reading
	// Make sure the file is open
	if(fp == NULL){
		printf( "error in opening paging.config file\n" );
		exit(1);
	}
	/* Read the three simulation parameters from the file */
	PageFraNum = ConfigureFileRead(fp, 1);						//Read number of page frames in CM
	TLBEntNum = ConfigureFileRead(fp, 2);						//Read number of TLB entries
	ObsPeriod = ConfigureFileRead(fp, 3);						//Read observation period
	fclose(fp);													//Close file after use

	/* Print the numbers for each array */
	if(Verbose == 1){
		printf( "paging simulation\n" );
		printf( "  %d virtual pages in the virtual address space\n", PAGE_TABLE_ENTRIES );
		printf( "  %d physical page frames\n", PageFraNum );
		printf( "  %d TLB entries\n", TLBEntNum );
		printf( "  use vectors in core map are shifted every %d accesses\n\n", ObsPeriod );
	}

	/* Allocate space for the entries of the data structures */
	PageTable = (PTE_t*)calloc(PAGE_TABLE_ENTRIES, sizeof(PTE_t));
	TLB = (TLBE_t*)calloc(TLBEntNum, sizeof(TLBE_t));			//TLBEntNum is the number in array
	CoreMap = (CME_t*)calloc(PageFraNum, sizeof(CME_t));		//PageFraNum is the number in array

	if(sizeof(PageTable)==0 || sizeof(TLB)==0 || sizeof(CoreMap)==0){
		printf( "error in allocating table arrays\n" );
		exit(1);
	}

	/* Initialize all values to 0 */
	InitializePageTable(PageTable);
	IntializeTLB(TLB,TLBEntNum);
	InitializeCoreMap(CoreMap,PageFraNum);

	/* Translate virtual addresses from trace file to physical addresses */
	AddressTranslation(TLB, TLBEntNum, PageTable, CoreMap, PageFraNum, ObsPeriod);

	printf( "statistics\n" );
	printf( "  accesses    = %d\n", access_count );
	printf( "  tlb misses  = %d\n", tlb_miss_count );
	printf( "  page faults = %d\n", page_fault_count );

	/* Prints the values in each structure array */
	if(Verbose == 1){
		PrintTLB(TLB,TLBEntNum);
		PrintCoreMap(CoreMap,PageFraNum);
		PrintPageTable(PageTable);			//Prints the first 10 entries
	}
	printf( "\n" );

	/* Free the memory for the arrays at the end */
	free(PageTable);
	free(TLB);
	free(CoreMap);
	return 0;
}


/* ---------------------------------------------------------------

						TLB Check Functions

 ---------------------------------------------------------------*/


/* Read in the virtual addresses from the trace file and translates them 
 * to physical addresses
 */
void AddressTranslation(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, int UP)
{
	int i=0, Match;
    char line[MAXLINE];
	unsigned int va, vatemp;

	//fgets until the end of the file
	while(fgets(line, MAXLINE, stdin) != NULL){
		sscanf(line, "%x", &va);					//Scan value from trace into address variable

		/* Make sure size of hex value doesn't exceed 24 bits */
		i=0;
		vatemp=va;									//Used to increment through hex value
		//Counts up 1 until there are no more bits to count
		while(vatemp){
			i++;
			vatemp=vatemp>>1;
		}

		/* If value exceeds 24 bits, debug print is triggered */
		if(i > 24) 
			DebugPrint(TLBE, TLBN, PTE, CME, CMN, UP);

		/* If value is within 24 bit range, it is a memory address and continue */
		else{
			access_count++;
			if(Verbose == 1) {
				printf( "access %d:\n", access_count );
				printf( "  virtual address is              0x%06x\n", va );
			}

			Match = TLBHitCheck(TLBE, TLBN, va);

			// TLB miss if no match found
			if(Match == 0)
				TLBMissFunc(TLBE, TLBN, PTE, CME, CMN, va);

			/* Shift Core Map's use vectors once access count loops through range */
			if( access_count%UP == 0) {
				//Shift use vectors in the core memory
				for(i=0; i < CMN; i++)
					CME[i].use_vector = CME[i].use_vector >> 1;

				if(Verbose == 1) 
					printf( "shift use vectors\n" );
			}
		}
	}
	if(Verbose == 1) 
		printf("\n");
}


/* ---------------------------------------------------------------
TLB Check Functions
 ---------------------------------------------------------------*/

/* Check the TLB entries for a hit 
 * Returns 1 if a match is found.		(TLB hit)
 * Returns 0 if a no match is found.	(TLB miss)
 * Physical address: PFN VA offset
 */
int TLBHitCheck(TLBE_t *TLBE, int TLBN,  unsigned int VA)
{
	unsigned int VPN = VA>>8;
	int i;
	unsigned int pa;

	/* Loop through TLB entries to search for a TLB hit */
	for(i=0; i < TLBN; i++){
		// TLB hit if new virtual address' VPN equals the TLB entry's VPN 
		if(TLBE[i].valid == 1 && VPN == TLBE[i].vpn){
			/* If TLB hit, use PFN from TLB entry to obtain the physcial address */
			pa = (TLBE[i].pfn<<8) + (VA &(0xFF));			// Offset TLB's PFN by VA's offset

			if(Verbose == 1) 
				printf( "  tlb hit, physical address is      0x%04x\n", pa );

			return 1;										// Return 1 if TLB hit
		}
	}
	return 0;												// Return 0 if no TLB hit		
}


/* Function for if there is a TLB miss
 * Checks the Page Table for a page hit 
 */
void TLBMissFunc(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, unsigned int VA)
{
	if(Verbose == 1) 
		printf( "  tlb miss\n" );
	tlb_miss_count++;

	// Page hit if presence bit is on
	if(PTE[VA>>8].presence == 1)
		PageHitFunc(TLBE, TLBN, PTE, CME, CMN, VA);

	// Page Fault if presence bit is off
	else
		PageFaultFunc(TLBE, TLBN, PTE, CME, CMN, VA);
}


/* Function to update the TLB
 * First look for an empty entry (valid == 0)
 * If no empty entry is found, use a FIFO replacement index to choose a valid entry to replace
 */
void UpdateTLB(TLBE_t *TLBE, int TLBN, unsigned int va, unsigned int pa)
{
	int i;
	/* First check for an empty TLB entry */
	// Loop through TLB entries to search for an empty TLB
	for(i=0; i < TLBN; i++){
		// Entry is empty if valid is equal to 0
		if(TLBE[i].valid == 0){
			TLBE[i].vpn = va>>8;					// VPN is first 16 bits of virtual address
			TLBE[i].pfn = pa>>8;					// PFN is first 8 bits of physical address
			TLBE[i].valid = 1;
			break;
		}
	}
	/* Replace an entry using the FIFOIndex if no empty entry found */
	// i will be equal to the number of TLB entries if no empty entry
	if(i == TLBN){
		TLBE[FIFOIndex].vpn = va>>8;				// VPN is first 16 bits of virtual address
		TLBE[FIFOIndex].pfn = pa>>8;				// PFN is first 8 bits of physical address
		i = FIFOIndex;
		FIFOIndex=(FIFOIndex+1)%TLBN;				// Modulo resets index once out of range
	}
	if(Verbose == 1)
		printf("  tlb update of vpn 0x%04x with pfn 0x%02x\n", TLBE[i].vpn, TLBE[i].pfn );
}




/* ---------------------------------------------------------------
Page Table Check Functions
 ---------------------------------------------------------------*/

/* Function for if there is a Page Hit 
 * Use the PFN from the Page Table Entry to obtain the physical address
 * Update the TLB
 * Physical address: PFN VA offset
 */
void PageHitFunc(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, unsigned int VA)
{
	unsigned int VPN = VA>>8;
	unsigned int pa;
	/* Update the TLB if there is a page hit */
	CMUseVectorUpdate(CME, CMN, PTE[VPN].pfn);			// Update use vector for the pfn
	pa = (PTE[VPN].pfn<<8) + (VA &(0xFF));				// Offset PT entry's PFN by VA's offset
	if(Verbose == 1) 
		printf( "  page hit, physical address is     0x%04x\n", pa );
	UpdateTLB(TLBE, TLBN, VA, pa);						// Updates the TLB
}

/* Function for if there is a Page Fault 
 * Check for free frames frames in the Core Map
 */
void PageFaultFunc(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, unsigned int VA)
{
	int FrameIndex = 0;
	unsigned int pa;

	if(Verbose == 1) 
		printf( "  page fault\n" );
	page_fault_count++;

	/* If Page Fault, look for frame in Core Map */
	FrameIndex = FindFreeFrame(CME,CMN);

	// If a free frame was found
	if(FrameIndex != -1)
		FreeFrameFound(TLBE, TLBN, PTE, CME, CMN, VA, FrameIndex);

	// If no free frame was found
	else{
		FrameIndex = FindLowestUseFrame(CME,CMN);
		LowUseFound(TLBE, TLBN, PTE, CME, CMN, VA, FrameIndex);
	}

	CMUseVectorUpdate(CME, CMN, FrameIndex);		//Update the use vector bits of the frame
	pa = (FrameIndex<<8) + (VA &(0xFF));
	if(Verbose == 1) 
		printf( "  physical address is               0x%04x\n", pa );
	UpdateTLB(TLBE, TLBN, VA, pa);					// Updates the TLB
}



/* ---------------------------------------------------------------
Core Frame Check Functions
 ---------------------------------------------------------------*/

/* Checks the core map for an empty frame 
 * Returns the index of the frame that will be used
 */
int FindFreeFrame(CME_t *CME, int CMN)
{
	int i;
	/* First check for an empty frame in the core map */
	for(i=0; i < CMN; i++){
		// Frame is empty if valid is equal to 0
		if(CME[i].valid == 0){
			if(Verbose == 1) 
				printf("  unused page frame allocated\n" );
			CME[i].valid = 1;
			return i;
		}
	}
	/* If no empty entry found, indicate that another search must be done */
	return -1;
}

/* Checks the core map for the first frame with the lowest-valued use vector
 * Returns the index of the frame that will be used
 */
int FindLowestUseFrame(CME_t *CME, int CMN)
{
	int i, LowestUseIndex=0;
	if(Verbose == 1) 
		printf("  page replacement needed\n" );
	/* First check for an empty frame in the core map */
	/* Keep track of use vectors as well in case there is not empty frame */
	for(i=0; i < CMN; i++){
		// Change the index if a lower use vector has been encountered
		if(CME[i].use_vector < CME[LowestUseIndex].use_vector)
			LowestUseIndex = i;						// i is the index of the lowest use
	}
	/* If no empty entry found, return index of lowest used entry */
	if(Verbose == 1) 
		printf( "  replace frame %d\n", LowestUseIndex );
	return LowestUseIndex;
}


/* Function if there's a free frame found 
 * Map the virtual page to the frame by updating the page table, core map, and TLB
 */
void FreeFrameFound(TLBE_t *TLBE,int TLBN,PTE_t *PTE,CME_t *CME,int CMN,unsigned int VA,int Index)
{
	unsigned int VPN = VA>>8;

	/* Map the virtal page to that frame */
	CME[Index].vpn = VPN;							// The index of the page table entry is the VPN
	PTE[VPN].pfn = Index;							// The index of the Core Map is the Match
	PTE[VPN].presence = 1;
}

/* Function if no free frame was found 
 * Replace lowest used frame with new frame
 * Invalidate the corresponding TLB entry
 */
void LowUseFound(TLBE_t *TLBE,int TLBN,PTE_t *PTE,CME_t *CME,int CMN,unsigned int VA,int Index)
{
	unsigned int VPN = VA>>8;
	int i;

	if(Verbose == 1)
		printf("  TLB invalidate of vpn 0x%x\n", CME[Index].vpn );  //printf old vpn invalidated

	/* Search to invalidate the TLB entry if it exists */
	for(i=0; i<TLBN;i++){
		if(TLBE[i].vpn == CME[Index].vpn)
			TLBE[i].valid = 0;
	}

	PTE[CME[Index].vpn].pfn = 0;
	PTE[CME[Index].vpn].presence = 0;
	CME[Index].vpn = VPN;
	CME[Index].valid = 1;
	CME[Index].use_vector = 0;
	PTE[VPN].pfn = Index;						// The index of the Core Map is the Match
	PTE[VPN].presence = 1;
}

/* Records a 1 at the highest bit of the Core Memory Entry's use vector */
void CMUseVectorUpdate(CME_t *CME, int CMN, unsigned int PFN)
{
	int i;
	//Find the highest 0 bit
	for(i=0; i<8; i++){
		if((CME[PFN].use_vector & 1<<(7-i))>>(7-i) == 0){
			CME[PFN].use_vector= CME[PFN].use_vector + (1<<(7-i));
			break;
		}
	}
}




/* ---------------------------------------------------------------

						Initializing Functions

 ---------------------------------------------------------------*/

/* Paging Configure File Read */
int ConfigureFileRead(FILE *fp, int ReadNum)
{
    char ConfLine[MAXLINE];
    char Identifier[MAXLINE];
    char junk[MAXLINE];
	int Param;

	fgets(ConfLine, MAXLINE, fp);

	/* Error if there is a NULL value */
	if(ConfLine == NULL){
		switch (ReadNum){
			case 1:		//Error for PF if first read
				printf( "error in reading PF (pages frames) from configuration file\n" );
				break;
			case 2:		//Error for TE if second read
				printf( "error in reading TE (TLB entries) from configuration file\n" );
				break;
			case 3:		//Error for UP if third read
				printf( "error in reading UP (use bit period) from configuration file\n" );
		}
		exit(1);
	}
	sscanf(ConfLine, "%s%d%s", Identifier, &Param, junk);
	return Param;
}

/* Initializes all entries in the Page Table array to 0 */
void InitializePageTable(PTE_t *PTE)
{
	int i;
	for(i=0;i<PAGE_TABLE_ENTRIES;i++){
		PTE[i].presence = 0;
		PTE[i].pad = 0;
		PTE[i].pfn = 0;
	}
}
/* Initializes all entries in the TLB array to 0 */
void IntializeTLB(TLBE_t *TLBE, int TLBN)
{
	int i;
	for(i=0;i<TLBN;i++){
		TLBE[i].valid = 0;
		TLBE[i].vpn = 0;
		TLBE[i].pfn = 0;
	}
}
/* Initializes all entries in the Core Map array to 0 */
void InitializeCoreMap(CME_t *CME, int CMN)
{
	int i;
	for(i=0;i<CMN;i++){
		CME[i].valid = 0;
		CME[i].use_vector = 0;
		CME[i].vpn = 0;
	}
}





/* ---------------------------------------------------------------

						Printing Functions

 ---------------------------------------------------------------*/

/* Prints the entries in the TLB array */
void PrintTLB(TLBE_t *TLBE, int TLBN)
{
	int i;
	printf( "\ntlb\n" );
	for(i=0;i<TLBN;i++)
		printf( "  valid = %x, vpn = 0x%04x, pfn = 0x%02x\n", TLBE[i].valid, TLBE[i].vpn, TLBE[i].pfn );
}
/* Prints the entries in the Core Map array */
void PrintCoreMap(CME_t *CME, int CMN)
{
	int i;
	printf( "\ncore map table\n" );
	for(i=0;i<CMN;i++)
		printf( "  pfn = 0x%02x: valid = %d, use vector = 0x%02x, vpn = 0x%04x\n", i, CME[i].valid, CME[i].use_vector, CME[i].vpn );
}
/* Prints the first 10 entries in the Page Table array */
void PrintPageTable(PTE_t *PTE)
{
	int i;
	printf( "\nfirst ten entries of page table\n" );
	for(i=0;i<10;i++)
		printf( "  vpn = 0x%04x: presence = %d, pfn = 0x%02x\n", i, PTE[i].presence, PTE[i].pfn );
}
/* Prints information if a hex value that is read exceeds 24 bits in length */
void DebugPrint(TLBE_t *TLBE, int TLBN, PTE_t *PTE, CME_t *CME, int CMN, int UP)
{
	printf( "\nstatistics\n" );
	printf( "  accesses    = %d\n", access_count );
	printf( "  tlb misses  = %d\n", tlb_miss_count );
	printf( "  page faults = %d\n", page_fault_count );

	/* Prints the values in each structure array */
	PrintTLB(TLBE,TLBN);
	PrintCoreMap(CME,CMN);
	PrintPageTable(PTE);			//Prints the first 10 entries
	printf( "\n" );
}


/* commands specified to vim. ts: tabstop, sts: soft tabstop sw: shiftwidth */
/* vi:set ts=4 sts=4 sw=4 net: */
