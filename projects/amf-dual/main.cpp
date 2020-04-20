#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <monkit.h>
#include <semaphore.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <time.h>

// Connectal DMA interface
#include "dmaManager.h"

// Connectal HW-SW interface
#include "AmfIndication.h"
#include "AmfRequest.h"

// Test Definitions
// #define TEST_AMF
// #define TEST_ERASE_ALL		 // eraseAll.exe's only test
// #define MINI_TEST_SUITE
// #define TEST_READ_SPEED
// #define TEST_WRITE_SPEED

#define DEFAULT_VERBOSE_REQ  false
#define DEFAULT_VERBOSE_RESP false

// Device Value (For test specific values, go to main)
// 256 pages/blk	=> pages 0, 1, 2 .. append only write
// 4096 blks/chip	=> erasure on blocks
// 8 chips/bus
// 8 buses

#define NUM_CARDS 2

#if defined(SIMULATION)
#define PAGES_PER_BLOCK 16
#define BLOCKS_PER_CHIP 128
#define CHIPS_PER_BUS 8
#define NUM_BUSES 2 // NAND_SIM (ControllerTypes.bsv)

#else
// MLC
#define PAGES_PER_BLOCK 256
#define BLOCKS_PER_CHIP 4096
#define CHIPS_PER_BUS 8
#define NUM_BUSES 8

#endif

// Page Size (Physical chip support up to 8224 bytes, but using 8192 bytes for now)
//  However, for DMA acks, we are using 8192*2 Bytes Buffer per TAG
#define FPAGE_SIZE (8192*2)
#define FPAGE_SIZE_VALID (8192)
#define NUM_TAGS 128

typedef enum {
	FREE = 0, // ready to be allocated
	USED, // allocated
	BAD,
	UNKNOWN
} FlashStatusT;

typedef struct {
	bool checkRead;
	bool busy;

	uint32_t lpa;

	uint8_t card;
	uint8_t bus;
	uint8_t chip;
	uint16_t block;
	uint16_t page;
} TagTableEntry;

typedef struct {
	bool checkRead;
	bool busy;
	uint32_t lpa;
} TagTableEntry2;

AmfRequestProxy *device;

pthread_mutex_t flashReqMutex;
sem_t aftlLoadedSem;
sem_t aftlBlkReadSem;

//16k * 128
size_t dstAlloc_sz = FPAGE_SIZE * NUM_TAGS * sizeof(unsigned char);
size_t srcAlloc_sz = FPAGE_SIZE * NUM_TAGS * sizeof(unsigned char);
int dstAlloc;
int srcAlloc;
unsigned int ref_dstAlloc;
unsigned int ref_srcAlloc;
unsigned int* dstBuffer;
unsigned int* srcBuffer;
unsigned int* readBuffers[NUM_TAGS];
unsigned int* writeBuffers[NUM_TAGS];

//TagTableEntry readTagTable[NUM_TAGS];
TagTableEntry writeTagTable[NUM_TAGS];

FlashStatusT flashStatus[NUM_CARDS][NUM_BUSES][CHIPS_PER_BUS][BLOCKS_PER_CHIP];
TagTableEntry eraseTagTable[NUM_TAGS];

TagTableEntry2 readTagTable[NUM_TAGS];

bool testPassed = false;
bool verbose_req  = DEFAULT_VERBOSE_REQ;
bool verbose_resp = DEFAULT_VERBOSE_RESP;

int curReadsInFlight = 0;
int curWritesInFlight = 0;
int curErasesInFlight = 0;

int blkInfoReads = 0;

double timespec_diff_sec( timespec start, timespec end ) {
	double t = end.tv_sec - start.tv_sec;
	t += ((double)(end.tv_nsec - start.tv_nsec)/1000000000L);
	return t;
}

unsigned int hashAddrToData(int bus, int chip, int blk, int word) {
	return ((bus<<24) + (chip<<20) + (blk<<16) + word);
}

bool checkReadData(uint8_t tag) {
	uint32_t lpa = readTagTable[tag].lpa;
	if (readBuffers[tag][0] != lpa) {
		fprintf(stderr, "LOG: **ERROR: read mismatch! tag=%u, expected/lpa=%u or %x, read=%u or %x\n", tag, lpa, lpa, readBuffers[tag][0], readBuffers[tag][0]);
		return false;
	}
	return true;
}
/*
bool checkReadData(int tag) {
	bool pass = true;
	TagTableEntry e = readTagTable[tag];

	unsigned int goldenData;
	if (flashStatus[e.card][e.bus][e.chip][e.block]==WRITTEN) {
		int numErrors = 0;
		for (unsigned int word=0; word<FPAGE_SIZE_VALID/sizeof(unsigned int); word++) {
			goldenData = hashAddrToData(e.bus, e.chip, e.block, word);
			if (goldenData != readBuffers[tag][word]) {
				fprintf(stderr, "LOG: **ERROR: read data mismatch! tag=%d, C%d %d %d %d %d, word=%d, Expected: %x, read: %x\n", tag, e.card, e.bus, e.chip, e.block, e.page, word, goldenData, readBuffers[tag][word]);
				numErrors++;
				pass = false;
				break;
			}
		}
		if (numErrors==0) {
			if(verbose_resp) fprintf(stderr, "LOG: Read data check passed on tag=%d!\n", tag);
		}
	}
	else if (flashStatus[e.card][e.bus][e.chip][e.block]==ERASED) {
		if (readBuffers[tag][0]==(unsigned int)-1) {
			if(verbose_resp) fprintf(stderr, "LOG: Read check pass on erased block!\n");
		}
		else if (readBuffers[tag][0]==0) {
			fprintf(stderr, "LOG: Warning: potential bad block, read erased data 0\n");
			fprintf(stderr, "LOG: tag=%d, C%d %d %d %d %d, Expected: %x, read: %x\n", tag, e.card, e.bus, e.chip, e.block, e.page, -1, readBuffers[tag][0]);
			pass = false;
		}
		else {
			fprintf(stderr, "LOG: **ERROR: read data mismatch! Expected: ERASED, read: %x\n", readBuffers[tag][0]);
			pass = false;
		}
	}
	else {
		fprintf(stderr, "LOG: **ERROR: flash block state unknown. Did you erase before write?\n");
		pass = false;
	}
	return pass;
}
*/

bool checker_done = false;

void *check_read_buffer_done(void *ptr) {
	uint8_t tag = 0;
	uint32_t flag_word_offset = FPAGE_SIZE_VALID/sizeof(unsigned int);
	while (!checker_done) {
		if ( readBuffers[tag][flag_word_offset] == (unsigned int)-1 ) {
			bool readPassed = false;

			if ( verbose_resp ) {
				//fprintf(stderr, "LOG: dma buffer (flash read) check done: tag=%d; inflight=%d\n", tag, curReadsInFlight );
				fprintf(stderr, "LOG: dma buffer (flash read) check done: lpa=%u; tag=%u; inflight=%d\n", readTagTable[tag].lpa, tag, curReadsInFlight );
				fflush(stderr);
			}

			if (readTagTable[tag].checkRead) readPassed = checkReadData(tag);

			pthread_mutex_lock(&flashReqMutex);
			if ( readTagTable[tag].checkRead && readPassed == false ) {
				testPassed = false;
				fprintf(stderr, "LOG: **ERROR: check read data failed @ tag=%u\n",tag);
			}
			if ( curReadsInFlight < 0 ) {
				fprintf(stderr, "LOG: **ERROR: Read requests in flight cannot be negative %d\n", curReadsInFlight );
			}
			if ( readTagTable[tag].busy == false ) {
				fprintf(stderr, "LOG: **ERROR: received unused buffer read done (duplicate) tag=%u\n", tag);
				testPassed = false;
			} else {
				curReadsInFlight --;
			}

			readTagTable[tag].busy = false;
			readBuffers[tag][flag_word_offset] = 0; // clear done

			pthread_mutex_unlock(&flashReqMutex);
		}
		tag = (tag+1)%NUM_TAGS;
	}

	return NULL;
}

class AmfIndication: public AmfIndicationWrapper {
	public:

		void readDone(uint8_t tag) {
			fprintf(stderr, "LOG: **ERROR: readDone should have never come\n");
		}

		void writeDone(uint8_t tag) {
			if ( verbose_resp ) {
				fprintf(stderr, "LOG: writedone, tag=%u\n", tag);
				fflush(stderr);
			}

			pthread_mutex_lock(&flashReqMutex);
			if ( curWritesInFlight < 0 ) {
				fprintf(stderr, "LOG: **ERROR: Write requests in flight cannot be negative %d\n", curWritesInFlight );
			}
			if ( writeTagTable[tag].busy == false) {
				fprintf(stderr, "LOG: **ERROR: received unknown write done (duplicate) tag=%u\n", tag);
				testPassed = false;
			} else {
				curWritesInFlight --;
			}
			writeTagTable[tag].busy = false;
			pthread_mutex_unlock(&flashReqMutex);

			fflush(stderr);
		}

		void eraseDone(uint8_t tag, uint8_t status) {
			uint8_t isRawCmd = (status & 2)>>1;
			uint8_t isBadBlock = status & 1;

			TagTableEntry entry = eraseTagTable[tag];
			
			if ( verbose_resp ) {
				fprintf(stderr, "LOG: eraseDone, tag=%u, isRawCmd=%u, isBad=%u\n", tag, isRawCmd, isBadBlock); fflush(stderr);
			}

			if (isBadBlock != 0) {
				fprintf(stderr, "LOG: detected bad block with tag = %u\n", tag);
			}

			if (isRawCmd) {
				flashStatus[entry.card][entry.bus][entry.chip][entry.block] = isBadBlock? BAD: FREE;
			}


			pthread_mutex_lock(&flashReqMutex);

			curErasesInFlight--;
			if ( curErasesInFlight < 0 ) {
				fprintf(stderr, "LOG: **ERROR: erase requests in flight cannot be negative %d\n", curErasesInFlight );
				curErasesInFlight = 0;
			}
			if ( eraseTagTable[tag].busy == false ) {
				fprintf(stderr, "LOG: **ERROR: received unused tag erase done %u\n", tag);
				testPassed = false;
			}
			eraseTagTable[tag].busy = false;

			pthread_mutex_unlock(&flashReqMutex);
		}

		void debugDumpResp (unsigned int debug0, unsigned int debug1,  unsigned int debug2, unsigned int debug3, unsigned int debug4, unsigned int debug5) {
			fprintf(stderr, "LOG: DEBUG DUMP: gearSend = %u, gearRec = %u, aurSend = %u, aurRec = %u, readSend=%u, writeSend=%u\n", debug0, debug1, debug2, debug3, debug4, debug5);
		}

		void respAftlFailed(AmfRequestT resp) {
			fprintf(stderr, "RespFAIL tag=%u cmd=%u lpa=%u\n", resp.tag, (unsigned int)resp.cmd, resp.lpa);
			pthread_mutex_lock(&flashReqMutex);
			if(resp.cmd == AmfREAD) {
				if (readTagTable[resp.tag].busy) {
					readTagTable[resp.tag].busy = false;
					curReadsInFlight--;
				}
			}
			else if (resp.cmd ==  AmfWRITE) {
				if (writeTagTable[resp.tag].busy) {
					writeTagTable[resp.tag].busy = false;
					curWritesInFlight--;
				}
			}
			else if (resp.cmd == AmfERASE) {
				if (eraseTagTable[resp.tag].busy) {
					eraseTagTable[resp.tag].busy = false;
					curErasesInFlight--;
				}
			}
			pthread_mutex_unlock(&flashReqMutex);
		}

		void respReadMapping(uint8_t allocated, uint16_t block_num) {
			fprintf(stderr, "respReadMapping: allocated?%u, blk=%u\n", allocated, block_num);
		}

		void respReadBlkInfo(const uint16_t* blkinfo_vec ) {
			//fprintf(stderr, "respReadBlkInfo:\n");
			for(int i =0; i<8; i++) {
				uint8_t card = (blkInfoReads >> 15);
				uint8_t bus = (blkInfoReads >> 12) & 7;
				uint8_t chip = (blkInfoReads >> 9) & 7;
				uint16_t blk = (blkInfoReads & 511)*8+i;

				uint8_t status = blkinfo_vec[i]>>14;

				fprintf(stderr, "%u %u %u %u: ", card, bus, chip, blk);
				if(status == FREE)
					fprintf(stderr, "FREE\n");
				else if(status == USED)
					fprintf(stderr, "USED\n");
				else if(status == BAD) 
					fprintf(stderr, "BAD\n");
				else fprintf(stderr, "UNKNOWN\n");
			}

			blkInfoReads++;
		}

		void respAftlLoaded(uint8_t resp) {
			fprintf(stderr, "AFTL loaded = %u\n", resp);
			sem_post(&aftlLoadedSem);
		}

		AmfIndication(unsigned int id, PortalTransportFunctions *transport = 0, void *param = 0, PortalPoller *poller = 0) : AmfIndicationWrapper(id, transport, param, poller){}
};


int getNumReadsInFlight() { return curReadsInFlight; }
int getNumWritesInFlight() { return curWritesInFlight; }
int getNumErasesInFlight() { return curErasesInFlight; }

//TODO: more efficient locking
int waitIdleEraseTag() {
	int tag = -1;
	while ( tag < 0 ) {
		pthread_mutex_lock(&flashReqMutex);
		for ( int t = 0; t < NUM_TAGS; t++ ) {
			if ( !eraseTagTable[t].busy ) {
				eraseTagTable[t].busy = true;
				tag = t;
				break;
			}
		}
		pthread_mutex_unlock(&flashReqMutex);
	}
	return tag;
}

//TODO: more efficient locking
int waitIdleWriteBuffer() {
	int tag = -1;
	while ( tag < 0 ) {
		pthread_mutex_lock(&flashReqMutex);
		for ( int t = 0; t < NUM_TAGS; t++ ) {
			if ( !writeTagTable[t].busy) {
				writeTagTable[t].busy = true;
				tag = t;
				break;
			}
		}
		pthread_mutex_unlock(&flashReqMutex);
	}
	return tag;
}

//TODO: more efficient locking
int waitIdleReadBuffer() {
	int tag = -1;
	while ( tag < 0 ) {
		pthread_mutex_lock(&flashReqMutex);
		for ( int t = 0; t < NUM_TAGS; t++ ) {
			if ( !readTagTable[t].busy ) {
				readTagTable[t].busy = true;
				tag = t;
				break;
			}
		}
		pthread_mutex_unlock(&flashReqMutex);
	}
	return tag;
}

void eraseBlock(uint32_t lpa, uint32_t tag) {
	pthread_mutex_lock(&flashReqMutex);
	curErasesInFlight ++;
	// flashStatus[card][bus][chip][block] = ERASED;
	pthread_mutex_unlock(&flashReqMutex);

	// if ( verbose_req ) fprintf(stderr, "LOG: sending erase block request with tag=%d %d@%d %d %d 0\n", tag, card, bus, chip, block );
	// device->eraseBlock(card, bus,chip,block,tag);
	
	if ( verbose_req ) fprintf(stderr, "LOG: sending Erase Block request with lpa=%u tag=%u\n", lpa, tag);

	AmfRequestT myReq;
	myReq.tag = (uint8_t)tag;
	myReq.cmd = AmfERASE;
	myReq.lpa = lpa;

	device->makeReq(myReq);
}

void writePage(uint32_t lpa, uint32_t tag) {
	pthread_mutex_lock(&flashReqMutex);
	curWritesInFlight ++;
	// flashStatus[card][bus][chip][block] = WRITTEN;
	pthread_mutex_unlock(&flashReqMutex);

	// if ( verbose_req ) fprintf(stderr, "LOG: sending write page request with tag=%d %d@%d %d %d %d\n", tag, card, bus, chip, block, page );
	// device->writePage(card, bus,chip,block,page,tag,tag*FPAGE_SIZE);

	if ( verbose_req ) fprintf(stderr, "LOG: sending Write Page request with lpa=%u tag=%u\n", lpa, tag);

	AmfRequestT myReq;
	myReq.tag = (uint8_t)tag;
	myReq.cmd = AmfWRITE;
	myReq.lpa = lpa;

	device->makeReq(myReq);
}

void readPage(uint32_t lpa, uint32_t tag, bool checkRead=false) {
	pthread_mutex_lock(&flashReqMutex);
	curReadsInFlight ++;
	// readTagTable[tag].card = card;
	// readTagTable[tag].bus = bus;
	// readTagTable[tag].chip = chip;
	// readTagTable[tag].block = block;
	// readTagTable[tag].page = page;
	// readTagTable[tag].checkRead = checkRead;

	readTagTable[tag].checkRead = checkRead;
	readTagTable[tag].lpa = lpa;

	pthread_mutex_unlock(&flashReqMutex);

	// if ( verbose_req ) fprintf(stderr, "LOG: sending read page request with tag=%d %d@%d %d %d %d\n", tag, card, bus, chip, block, page );
	// device->readPage(card, bus,chip,block,page,tag,tag*FPAGE_SIZE);

	if ( verbose_req ) fprintf(stderr, "LOG: sending Read Page request with lpa=%u tag=%u\n", lpa, tag);

	AmfRequestT myReq;
	myReq.tag = (uint8_t)tag;
	myReq.cmd = AmfREAD;
	myReq.lpa = lpa;

	device->makeReq(myReq);
}

// Use all BUSES & CHIPS in the device
// Can designate the range of blocks -> incl [blkStart, blkStart + blkCnt) non-incl
//	 Valid block # range: 0 ~ BLOCKS_PER_CHIP-1 (=4097)
void testErase(AmfRequestProxy* device, int blkStart, int blkCnt) {
	// blkEnd can be at most BLOCKS_PER_CHIP-1
	int blkEnd = blkStart + blkCnt;
	if (blkEnd > BLOCKS_PER_CHIP) blkEnd = BLOCKS_PER_CHIP;

	for (int blk = blkStart; blk < blkEnd; blk++){
		for (int chip = 0; chip < CHIPS_PER_BUS; chip++){
			for (int bus = 0; bus < NUM_BUSES; bus++){
				for (int card = 0; card < 2; card++) {
					// eraseBlock(card, bus, chip, blk, waitIdleEraseTag());
					eraseBlock( card + 2*bus + 16*chip + 32768*blk, waitIdleEraseTag());
				}
			}
		}
	}

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
			device->debugDumpReq(1);
			device->debugDumpReq(0);
			device->debugDumpReq(1);
		}
		else {
			elapsed--;
		}
		if ( getNumErasesInFlight() == 0 ) break;
	}
}

// Use all BUSES & CHIPS in the device
// Can designate the range of blocks -> incl [blkStart, blkStart + blkCnt) non-incl
// Can designate the range of pages  -> incl [pageStart, pageStart + pageCnt) non-incl
//	 Valid block # range: 0 ~ BLOCKS_PER_CHIP-1
//	 Valid page  # range: 0 ~ PAGES_PER_BLOCK-1
void testRead(AmfRequestProxy* device, int blkStart, int blkCnt, int pageStart, int pageCnt, bool checkRead=false, int repeat=1) {
	// blkEnd can be at most BLOCKS_PER_CHIP-1
	int blkEnd = blkStart + blkCnt;
	if (blkEnd > BLOCKS_PER_CHIP) blkEnd = BLOCKS_PER_CHIP;

	// pageEnd can be at most BLOCKS_PER_CHIP-1
	int pageEnd = pageStart + pageCnt;
	if (pageEnd > PAGES_PER_BLOCK) pageEnd = PAGES_PER_BLOCK;
	
	for (int rep = 0; rep < repeat; rep++){
		for (int blk = blkStart; blk < blkEnd; blk++){ //4096
			for (int page = pageStart; page < pageEnd; page++) {//256
				for (int chip = 0; chip < CHIPS_PER_BUS; chip++){ //8
					for (int bus = 0; bus < NUM_BUSES; bus++){ //8
						for (int card = 0; card < 2; card++) {
							// readPage(card, bus, chip, blk, page, waitIdleReadBuffer(), checkRead);
							readPage( card + 2*bus + 16*chip + 128*page + 32768*blk, waitIdleReadBuffer(), checkRead);
						}
					}
				}
			}
		}
	}

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
			device->debugDumpReq(1);
		}
		else {
			elapsed--;
		}
		if ( getNumReadsInFlight() == 0 ) break;
	}
}

void testRead2(AmfRequestProxy* device, int lpaStart, int lpaCnt, bool checkRead=false, int repeat=1) {
	for (int lpa = lpaStart; lpa < lpaStart+lpaCnt; lpa++){
		readPage( lpa, waitIdleReadBuffer(), checkRead);
	}

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
			device->debugDumpReq(1);
		}
		else {
			elapsed--;
		}
		if ( getNumReadsInFlight() == 0 ) break;
	}
}

// Use all BUSES & CHIPS in the device
// Can designate the range of blocks -> incl [blkStart, blkStart + blkCnt) non-incl
// Can designate the range of pages  -> incl [pageStart, pageStart + pageCnt) non-incl
//	 Valid block # range: 0 ~ BLOCKS_PER_CHIP-1
//	 Valid page  # range: 0 ~ PAGES_PER_BLOCK-1
void testWrite(AmfRequestProxy* device, int blkStart, int blkCnt, int pageStart, int pageCnt, bool genData=false) {
	// blkEnd can be at most BLOCKS_PER_CHIP-1
	int blkEnd = blkStart + blkCnt;
	if (blkEnd > BLOCKS_PER_CHIP) blkEnd = BLOCKS_PER_CHIP;

	// pageEnd can be at most BLOCKS_PER_CHIP-1
	int pageEnd = pageStart + pageCnt;
	if (pageEnd > PAGES_PER_BLOCK) pageEnd = PAGES_PER_BLOCK;
	
	for (int blk = blkStart; blk < blkEnd; blk++){
		for (int page = pageStart; page < pageEnd; page++) {
			for (int chip = 0; chip < CHIPS_PER_BUS; chip++){
				for (int bus = 0; bus < NUM_BUSES; bus++){
					for (int card = 0; card < 2; card++) {
						int freeTag = waitIdleWriteBuffer();
						if (genData) {
							for (unsigned int w=0; w<FPAGE_SIZE/sizeof(unsigned int); w++) {
								writeBuffers[freeTag][w] = hashAddrToData(bus, chip, blk, w);
							}
						}
						//writePage(card, bus, chip, blk, page, freeTag);
						writePage( card + 2*bus + 16*chip + 128*page + 32768*blk, freeTag);
					}
				}
			}
		}
	} 

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
			device->debugDumpReq(1);
		}
		else {
			elapsed--;
		}
		if ( getNumWritesInFlight() == 0 ) break;
	}
}

void testWrite2(AmfRequestProxy* device, int lpaStart, int lpaCnt, bool genData=false) {
	for (int lpa = lpaStart; lpa < lpaStart+lpaCnt; lpa++){
		int freeTag = waitIdleWriteBuffer();

		if (genData) {
			for (unsigned int w=0; w<FPAGE_SIZE/sizeof(unsigned int); w++) {
				writeBuffers[freeTag][w] = lpa;
			}
		}
		writePage( lpa, freeTag);
	}

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
			device->debugDumpReq(1);
		}
		else {
			elapsed--;
		}
		if ( getNumReadsInFlight() == 0 ) break;
	}
}


int main(int argc, const char **argv)
{
	testPassed=true;
	pthread_mutex_init(&flashReqMutex, NULL);
	sem_init(&aftlLoadedSem, 0, 0);

	fprintf(stderr, "Initializing Connectal & DMA...\n");

	// Device initialization
	device = new AmfRequestProxy(IfcNames_AmfRequestS2H);
	AmfIndication deviceIndication(IfcNames_AmfIndicationH2S);
	DmaManager *dma = platformInit();
	
	// Memory-allocation for DMA
	srcAlloc = portalAlloc(srcAlloc_sz, 0);
	dstAlloc = portalAlloc(dstAlloc_sz, 0);
	srcBuffer = (unsigned int *)portalMmap(srcAlloc, srcAlloc_sz);
	dstBuffer = (unsigned int *)portalMmap(dstAlloc, dstAlloc_sz);

	fprintf(stderr, "dstAlloc = %x\n", dstAlloc); 
	fprintf(stderr, "srcAlloc = %x\n", srcAlloc); 

	// Memory-mapping for DMA
	portalCacheFlush(dstAlloc, dstBuffer, dstAlloc_sz, 1);
	portalCacheFlush(srcAlloc, srcBuffer, srcAlloc_sz, 1);
	ref_dstAlloc = dma->reference(dstAlloc);
	ref_srcAlloc = dma->reference(srcAlloc);

	fprintf(stderr, "ref_dstAlloc = %x\n", ref_dstAlloc); 
	fprintf(stderr, "ref_srcAlloc = %x\n", ref_srcAlloc); 

	device->setDmaWriteRef(ref_dstAlloc);
	device->setDmaReadRef(ref_srcAlloc);

	fprintf(stderr, "Done initializing Hardware & DMA!\n" ); 

	for (int t = 0; t < NUM_TAGS; t++) {
		readTagTable[t].busy = false;
		writeTagTable[t].busy = false;
		eraseTagTable[t].busy = false;

		int byteOffset = t * FPAGE_SIZE;
		readBuffers[t] = dstBuffer + byteOffset/sizeof(unsigned int);
		writeBuffers[t] = srcBuffer + byteOffset/sizeof(unsigned int);
	}

	for (int card=0; card < NUM_CARDS ; card++) {
		for (int bus=0; bus< NUM_BUSES; bus++) {
			for (int c=0; c < CHIPS_PER_BUS; c++) {
				for (int blk=0; blk < BLOCKS_PER_CHIP; blk++) {
					flashStatus[card][bus][c][blk] = UNKNOWN;
				}
			}
		}
	}

	for (int t = 0; t < NUM_TAGS; t++) {
		for ( unsigned int i = 0; i < FPAGE_SIZE/sizeof(unsigned int); i++ ) {
			readBuffers[t][i] = 0xDEADBEEF;
			writeBuffers[t][i] = 0xBEEFDEAD;
		}
	}

	// read done checker
	pthread_t check_thread;
	if(pthread_create(&check_thread, NULL, check_read_buffer_done, NULL)) {
		fprintf(stderr, "Error creating thread\n");
		return -1;
	}

	long actualFrequency=0;
	long requestedFrequency=1e9/MainClockPeriod;
	int status = setClockFrequency(0, requestedFrequency, &actualFrequency);
	fprintf(stderr, "HW Operating Freq - Requested: %5.2f, Actual: %5.2f, status=%d\n"
			,(double)requestedFrequency*1.0e-6
			,(double)actualFrequency*1.0e-6,status);

	fprintf(stderr, "Done initializing Hardware & DMA!\n" ); 
	fflush(stderr);

	fprintf(stderr, "Test Start!\n" );
	fflush(stderr);


	device->debugDumpReq(0);   // echo-back debug message
	device->debugDumpReq(1);   // echo-back debug message
	sleep(1);


	fprintf(stderr, "Map Status Req Sent\n");
	device->askAftlLoaded();
	sem_wait(&aftlLoadedSem);

	fprintf(stderr, "Map Status Set Sent\n");
	device->setAftlLoaded();

	fprintf(stderr, "Map Status Req Sent\n");
	device->askAftlLoaded();
	sem_wait(&aftlLoadedSem);


	// TODO: My Test
	
	{
		verbose_req = true;
		verbose_resp = true;

		fprintf(stderr, "ERASE ALL\n");
		for (uint16_t blk = 0; blk <  BLOCKS_PER_CHIP; blk++) {
			for (uint8_t chip = 0; chip < CHIPS_PER_BUS; chip++) {
				for (uint8_t bus = 0; bus < NUM_BUSES; bus++) {
					for (uint8_t card=0; card < NUM_CARDS; card++) {
						uint8_t tag = (uint8_t)waitIdleEraseTag();

						eraseTagTable[tag].card = card;
						eraseTagTable[tag].bus = bus;
						eraseTagTable[tag].chip = chip;
						eraseTagTable[tag].block = blk;

						pthread_mutex_lock(&flashReqMutex);
						curErasesInFlight ++;
						pthread_mutex_unlock(&flashReqMutex);

						device->eraseRawBlock(card, bus, chip, blk, tag);
					}
				}
			}
		}

		int elapsed = 10000;
		while (true) {
			usleep(100);
			if (elapsed == 0) {
				elapsed=10000;
				device->debugDumpReq(0);
				device->debugDumpReq(1);
			}
			else {
				elapsed--;
			}
			if ( getNumErasesInFlight() == 0 ) break;
		}

		fprintf(stderr, "ERASE ALL 111111\n");
		for (uint8_t card=0; card < NUM_CARDS; card++) {
			for (uint8_t bus = 0; bus < NUM_BUSES; bus++) {
				for (uint8_t chip = 0; chip < CHIPS_PER_BUS; chip++) {
					for (uint16_t blk = 0; blk <  BLOCKS_PER_CHIP; blk++) {
						FlashStatusT status = flashStatus[card][bus][chip][blk];

						fprintf(stderr, "%u %u %u %u: ", card, bus, chip, blk);
						if(status == FREE)
							fprintf(stderr, "FREE\n");
						else if(status == USED)
							fprintf(stderr, "USED\n");
						else if(status == BAD) 
							fprintf(stderr, "BAD\n");
						else fprintf(stderr, "UNKNOWN\n");
					}
				}
			}
		}

		fprintf(stderr, "ERASE ALL 222222\n");


		uint32_t blk_cnt = 0;
		for (uint8_t card=0; card < NUM_CARDS; card++)  {
			for (uint8_t bus = 0; bus < NUM_BUSES; bus++) {
				for (uint8_t chip = 0; chip < CHIPS_PER_BUS; chip++) {
					uint16_t entry_vec[8];

					for (uint16_t blk = 0; blk < BLOCKS_PER_CHIP; blk++) {

						int idx = blk % 8;
						entry_vec[idx] = (uint16_t)(flashStatus[card][bus][chip][blk] << 14);

						if (idx == 7) {
							device->updateBlkInfo((uint16_t)(blk_cnt>>3), entry_vec);
						}

						blk_cnt++;
					}
				}
			}
		}

		fprintf(stderr, "ERASE ALL 333333\n");
		blk_cnt = 0;
		int maxReads = NUM_CARDS*NUM_BUSES*CHIPS_PER_BUS*BLOCKS_PER_CHIP/8;

		for (uint8_t card=0; card < NUM_CARDS; card++) {
			for (uint8_t bus = 0; bus < NUM_BUSES; bus++) {
				for (uint8_t chip = 0; chip < CHIPS_PER_BUS; chip++) {
					for (uint16_t blk = 0; blk < BLOCKS_PER_CHIP; blk++) {

						int idx = blk % 8;
						if (idx == 7) {
							device->readBlkInfo((uint16_t)(blk_cnt>>3));
						}

						blk_cnt++;
					}
				}
			}
		}

		fprintf(stderr, "ERASE ALL 444444\n");

		elapsed = 10000;
		while (true) {
			usleep(100);
			if (elapsed == 0) {
				elapsed=10000;
			}
			else {
				elapsed--;
			}
			if ( blkInfoReads == maxReads ) break;
		}

	}


#if defined(TEST_ERASE_ALL)
	{
		verbose_req = true;
		verbose_resp = true;

		fprintf(stderr, "[TEST] ERASE ALL BLOCKS STARTED!\n");
		testErase(device, 0, BLOCKS_PER_CHIP);
		fprintf(stderr, "[TEST] ERASE ALL BLOCKS DONE!\n"); 
	}
#endif

#if defined(TEST_AMF)
	{
		verbose_req = false;
		verbose_resp = false;

		timespec start, now;

		clock_gettime(CLOCK_REALTIME, &start);
		testWrite2(device, 1024*1024/8, 1024*1024/8 * 9 , false);
		clock_gettime(CLOCK_REALTIME, & now);

		fprintf(stderr, "WRITE SPEED: %f MB/s\n", ((1024*1024*9)/1000)/timespec_diff_sec(start,now));

		clock_gettime(CLOCK_REALTIME, &start);
		testRead2(device, 0, 1024*1024/8 * 10 , false);
		clock_gettime(CLOCK_REALTIME, & now);

		fprintf(stderr, "READ SPEED: %f MB/s\n", ((1024*1024*10)/1000)/timespec_diff_sec(start,now));
	}
#endif

#if defined(MINI_TEST_SUITE)
	{
		// Functionality test
		verbose_req = true;
		verbose_resp = true;

#if defined(SIMULATION)
		int blkStart = 0;
		int blkCnt = 4;
		int pageStart = 0;
		int pageCnt = 2;
#else
		int blkStart = 0;
		int blkCnt = 32; //BLOCKS_PER_CHIP;
		int pageStart = 0;
		int pageCnt = 4; //PAGES_PER_BLOCK;
#endif

		fprintf(stderr, "[TEST] ERASE RANGED BLOCKS (Start: %d, Cnt: %d) STARTED!\n", blkStart, blkCnt); 
		testErase(device, blkStart, blkCnt);
		fprintf(stderr, "[TEST] ERASE RANGED BLOCKS DONE!\n" ); 

		fprintf(stderr, "[TEST] READ CHECK ON ERASED BLOCKS (Start: %d, Cnt: %d) STARTED!\n", blkStart, blkCnt); 
		testRead(device, blkStart, blkCnt, pageStart, pageCnt, true /* checkRead */);
		fprintf(stderr, "[TEST] READ CHECK ON ERASED BLOCKS DONE!\n" ); 

		fprintf(stderr, "[TEST] WRITE ON ERASED BLOCKS (BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", blkStart, blkCnt, pageStart, pageCnt); 
		testWrite(device, blkStart, blkCnt, pageStart, pageCnt, true /* genData */);
		fprintf(stderr, "[TEST] WRITE ON ERASED BLOCKS DONE!\n" ); 

		fprintf(stderr, "[TEST] READ ON WRITTEN BLOCKS (BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", blkStart, blkCnt, pageStart, pageCnt); 
		testRead(device, blkStart, blkCnt, pageStart, pageCnt, true /* checkRead */);
		fprintf(stderr, "[TEST] READ ON WRITTEN BLOCKS DONE!\n" ); 
	}
#endif

#if defined(TEST_READ_SPEED)
	{
		// Read speed test: No printf / No data generation / No data integrity check
		verbose_req = false;
		verbose_resp = false;

		int repeat = 4;
		int blkStart = 0;
		int blkCnt = 128;
		int pageStart = 0;
		int pageCnt = 16;

		timespec start, now;
		clock_gettime(CLOCK_REALTIME, &start);

		fprintf(stderr, "[TEST] READ SPEED (Repeat: %d, BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", repeat, blkStart, blkCnt, pageStart, pageCnt); 
		testRead(device, blkStart, blkCnt, pageStart, pageCnt, false /* checkRead */, repeat);

		fprintf(stderr, "[TEST] READ SPEED DONE!\n" ); 

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "SPEED: %f MB/s\n", (2*repeat*8192.0*NUM_BUSES*CHIPS_PER_BUS*blkCnt*pageCnt/1000000)/timespec_diff_sec(start,now));
	}
#endif

#if defined(TEST_WRITE_SPEED)
	{
		// Read speed test: No printf / No data generation / No data integrity check
		verbose_req = false;
		verbose_resp = false;

		int blkStart = 3001;
		int blkCnt = 30;
		int pageStart = 0;
		int pageCnt = 64;

		timespec start, now;
		clock_gettime(CLOCK_REALTIME, &start);

		fprintf(stderr, "[TEST] Erase before write test (BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", blkStart, blkCnt, pageStart, pageCnt); 
		testErase(device, blkStart, blkCnt);

		fprintf(stderr, "[TEST] WRITE SPEED (BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", blkStart, blkCnt, pageStart, pageCnt); 
		testWrite(device, blkStart, blkCnt, pageStart, pageCnt, false);

		fprintf(stderr, "[TEST] WRITE SPEED DONE!\n" ); 

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "SPEED: %f MB/s\n", (2*8192.0*NUM_BUSES*CHIPS_PER_BUS*blkCnt*pageCnt/1000000)/timespec_diff_sec(start,now));
	}
#endif

	sleep(1);
	if (testPassed==true) {
		fprintf(stderr, "LOG: TEST PASSED!\n");
	}
	else {
		fprintf(stderr, "LOG: **ERROR: TEST FAILED!\n");
	}

	sleep(1);

	checker_done = true;
	pthread_join(check_thread, NULL);
	fprintf(stderr, "Checker released\n");

	dma->dereference(ref_srcAlloc);
	dma->dereference(ref_dstAlloc);
	portalMunmap(srcBuffer, srcAlloc_sz);
	portalMunmap(dstBuffer, dstAlloc_sz);
	fprintf(stderr, "Done releasing DMA!\n");

}
