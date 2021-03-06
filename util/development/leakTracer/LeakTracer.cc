/*
 * Modification History
 *
 * 2002-March-31    Jason Rohrer
 * Added support for strdup.
 * Fixed bug in strdup.
 *
 * 2003-October-3    Jason Rohrer
 * Added printout of leak contents in report.
 *
 * 2004-January-16    Jason Rohrer
 * Switched to use minorGems platform-independed mutexes.
 * Changed to use simpler fopen call to open report.
 */


/* 
 * Homepage: <http://www.andreasen.org/LeakTracer/>
 *
 * Authors:
 *  Erwin S. Andreasen <erwin@andreasen.org>
 *  Henner Zeller <foobar@to.com>
 *
 * This program is Public Domain
 */

#ifdef THREAD_SAVE
#define _THREAD_SAVE
//#include <pthread.h>
#include "minorGems/system/MutexLock.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>



// no execinfo stacktrace support on MinGW
#ifdef __MINGW_H
  #define NO_STACK_TRACE
#else
  #include <execinfo.h>
#endif


/*
 * underlying allocation, de-allocation used within 
 * this tool
 */
#define LT_MALLOC  malloc
#define LT_FREE    free
#define LT_REALLOC realloc

/*
 * prime number for the address lookup hash table.
 * if you have _really_ many memory allocations, use a
 * higher value, like 343051 for instance.
 */
#define SOME_PRIME 35323
#define ADDR_HASH(addr) ((unsigned long) addr % SOME_PRIME)

/**
 * Filedescriptor to write to. This should not be a low number,
 * because these often have special meanings (stdin, out, err)
 * and may be closed by the program (daemons)
 * So choose an arbitrary higher FileDescriptor .. e.g. 42
 */
#define FILEDESC    42

/**
 * allocate a bit more memory in order to check if there is a memory
 * overwrite. Either 0 or more than sizeof(unsigned int). Note, you can
 * only detect memory over_write_, not _reading_ beyond the boundaries. Better
 * use electric fence for these kind of bugs 
 *   <ftp://ftp.perens.com/pub/ElectricFence/>
 */
typedef unsigned long magic_t;
#define MAGIC       ((magic_t) 0xAABBCCDDLu)

/**
 * this may be more than sizeof(magic_t); if you want more, then
 * sepecify it like #define SAVESIZE (sizeof(magic_t) + 12)
 */
#define SAVESIZE  (sizeof(magic_t) + 0)

/**
 * on 'new', initialize the memory with this value.
 * if not defined - uninitialized. This is very helpful because
 * it detects if you initialize your classes correctly .. if not,
 * this helps you faster to get the segmentation fault you're 
 * implicitly asking for :-). 
 *
 * Set this to some value which is likely to produce a
 * segmentation fault on your platform.
 */
#define SAVEVALUE   0xAA

/**
 * on 'delete', clean memory with this value.
 * if not defined - no memory clean.
 *
 * Set this to some value which is likely to produce a
 * segmentation fault on your platform.
 */
#define MEMCLEAN    0xEE



// number of stack return addresses to store for each allocation
// used to print a trace when memory leak is detected
#ifndef NO_STACK_TRACE
  #define RECORDED_STACK_SIZE 20
#else
  #define RECORDED_STACK_SIZE 0
#endif



/**
 * Initial Number of memory allocations in our list.
 * Doubles for each re-allocation.
 */
#define INITIALSIZE 32768

static class LeakTracer {
	struct Leak {
		const void *addr;
		size_t      size;
		const void *allocAddr;
		bool        type;
        int stackTraceSize;
        void *stackTraceAddresses[ RECORDED_STACK_SIZE ];
        
		int         nextBucket;
	};
	
	int  newCount;      // how many memory blocks do we have
	int  leaksCount;    // amount of entries in the leaks array
	int  firstFreeSpot; // Where is the first free spot in the leaks array?
	int  currentAllocated; // currentAllocatedMemory
	int  maxAllocated;     // maximum Allocated
	unsigned long totalAllocations; // total number of allocations. stats.
	unsigned int  abortOn;  // resons to abort program (see abortReason_t)

	/**
	 * Have we been initialized yet?  We depend on this being
	 * false before constructor has been called!  
	 */
	bool initialized;	
	bool destroyed;		// Has our destructor been called?


	FILE *report;       // filedescriptor to write to

	/**
	 * pre-allocated array of leak info structs.
	 */
	Leak *leaks;

	/**
	 * fast hash to lookup the spot where an allocation is 
	 * stored in case of an delete. map<void*,index-in-leaks-array>
	 */
	int  *leakHash;     // fast lookup

#ifdef THREAD_SAVE
	MutexLock mutex;
#endif

	enum abortReason_t {
		OVERWRITE_MEMORY    = 0x01,
		DELETE_NONEXISTENT  = 0x02,
		NEW_DELETE_MISMATCH = 0x04
	};

public:
	LeakTracer() {
		initialize();
	}
	
	void initialize() {
		// Unfortunately we might be called before our constructor has actualy fired
		if (initialized)
			return;

		//		fprintf(stderr, "LeakTracer::initialize()\n");
		initialized = true;
		newCount = 0;
		leaksCount = 0;
		firstFreeSpot = 1; // index '0' is special
		currentAllocated = 0;
		maxAllocated = 0;
		totalAllocations = 0;
		abortOn =  OVERWRITE_MEMORY; // only _severe_ reason
		report = 0;
		leaks = 0;
		leakHash = 0;

		char uniqFilename[256];
		const char *filename = getenv("LEAKTRACE_FILE") ? : "leak.out";
		struct stat dummy;
		if (stat(filename, &dummy) == 0) {
			sprintf(uniqFilename, "%s.%d", filename, getpid());
			fprintf(stderr, 
				"LeakTracer: file exists; using %s instead\n",
				uniqFilename);
		}
		else {
			sprintf(uniqFilename, "%s", filename);
		}

        // not sure why this "open" code is here
        // (it doesn't open the file properly in MinGW on win32)
        /*
		int reportfd = open(uniqFilename, 
				    O_WRONLY|O_CREAT|O_TRUNC,S_IREAD|S_IWRITE);
		if (reportfd < 0) {
			fprintf(stderr, "LeakTracer: cannot open %s: %m\n", 
				filename);
			report = stderr;
		}
		else {
			int dupfd = dup2(reportfd, FILEDESC);
			close(reportfd);
			report = fdopen(dupfd, "w");
			if (report == NULL) {
				report = stderr;
			}
		}
		*/

        // simpler version using only fopen
        report = fopen( uniqFilename, "w" );
        if( report == NULL ) {
            fprintf( stderr, "LeakTracer: cannot open %s\n", 
                    uniqFilename );
            report = stderr;
        }

        
		time_t t = time(NULL);
		fprintf (report, "# starting %s", ctime(&t));

		leakHash = (int*) LT_MALLOC(SOME_PRIME * sizeof(int));
		memset ((void*) leakHash, 0x00, SOME_PRIME * sizeof(int));

#ifdef MAGIC
		fprintf (report, "# memory overrun protection of %d Bytes "
			 "with magic 0x%4lX\n", 
			 SAVESIZE, MAGIC);
#endif
		
#ifdef SAVEVALUE
		fprintf (report, "# initializing new memory with 0x%2X\n",
			 SAVEVALUE);
#endif

#ifdef MEMCLEAN
		fprintf (report, "# sweeping deleted memory with 0x%2X\n",
			 MEMCLEAN);
#endif
		if (getenv("LT_ABORTREASON")) {
			abortOn = atoi(getenv("LT_ABORTREASON"));
		}

#define PRINTREASON(x) if (abortOn & x) fprintf(report, "%s ", #x);
		fprintf (report, "# aborts on ");
		PRINTREASON( OVERWRITE_MEMORY );
		PRINTREASON( DELETE_NONEXISTENT );
		PRINTREASON( NEW_DELETE_MISMATCH );
		fprintf (report, "\n");
#undef PRINTREASON

#ifdef THREAD_SAVE
		fprintf (report, "# thread save\n");
		/*
		 * create default, non-recursive ('fast') mutex
		 * to lock our datastructure where we keep track of
		 * the user's new/deletes
		 */
		/*if (pthread_mutex_init(&mutex, NULL) < 0) {
			fprintf(report, "# couldn't init mutex ..\n");
			fclose(report);
			_exit(1);
            }*/
#else
		fprintf(report, "# not thread save; if you use threads, recompile with -DTHREAD_SAVE\n");
#endif
		fflush(report);
	}
	
	/*
	 * the workhorses:
	 */
	void *registerAlloc(size_t size, bool type);
	void  registerFree (void *p, bool type);

	/**
	 * write a hexdump of the given area.
	 */
	void  hexdump(const unsigned char* area, int size);

    void addStackTraceToReport( Leak inLeak );
        
	
	/**
	 * Terminate current running progam.
	 */
	void progAbort(abortReason_t reason) {
		if (abortOn & reason) {
			fprintf(report, "# abort; DUMP of current state\n");
			writeLeakReport();
			fclose(report);
			abort();
		}
		else
			fflush(report);
	}

	/**
	 * write a Report over leaks, e.g. still pending deletes
	 */
	void writeLeakReport();

	~LeakTracer() {
	    //		fprintf(stderr, "LeakTracer::destroy()\n");
		time_t t = time(NULL);
		fprintf (report, "# finished %s", ctime(&t));
		writeLeakReport();
		fclose(report);
		free(leaks);
#ifdef THREAD_SAVE
		//pthread_mutex_destroy(&mutex);
#endif
		destroyed = true;
	}
} leakTracer;

void* LeakTracer::registerAlloc (size_t size, bool type) {
	initialize();

	//	fprintf(stderr, "LeakTracer::registerAlloc()\n");

	if (destroyed) {
		fprintf(stderr, "Oops, registerAlloc called after destruction of LeakTracer (size=%d)\n", size);
		return LT_MALLOC(size);
	}


	void *p = LT_MALLOC(size + SAVESIZE);
	// Need to call the new-handler
	if (!p) {
		fprintf(report, "LeakTracer malloc %m\n");
		_exit (1);
	}

#ifdef SAVEVALUE
	/* initialize with some defined pattern */
	memset(p, SAVEVALUE, size + SAVESIZE);
#endif
	
#ifdef MAGIC
	/*
	 * the magic value is a special pattern which does not need
	 * to be uniform.
	 */
	if (SAVESIZE >= sizeof(magic_t)) {
		magic_t *mag;
		mag = (magic_t*)((char*)p + size);
		*mag = MAGIC;
	}
#endif

#ifdef THREAD_SAVE
	//pthread_mutex_lock(&mutex);
    mutex.lock();
#endif

	++newCount;
	++totalAllocations;
	currentAllocated += size;
	if (currentAllocated > maxAllocated)
		maxAllocated = currentAllocated;
	
	for (;;) {
		for (int i = firstFreeSpot; i < leaksCount; i++)
			if (leaks[i].addr == NULL) {
				leaks[i].addr = p;
				leaks[i].size = size;
				leaks[i].type = type;
				leaks[i].allocAddr=__builtin_return_address(1);
#ifndef NO_STACK_TRACE
                leaks[i].stackTraceSize =
                        backtrace( leaks[i].stackTraceAddresses,
                                   RECORDED_STACK_SIZE );                
#endif
                firstFreeSpot = i+1;
				// allow to lookup our index fast.
				int *hashPos = &leakHash[ ADDR_HASH(p) ];
				leaks[i].nextBucket = *hashPos;
				*hashPos = i;
#ifdef THREAD_SAVE
				//pthread_mutex_unlock(&mutex);
                mutex.unlock();
#endif
				return p;
			}
		
		// Allocate a bigger array
		// Note that leaksCount starts out at 0.
		int new_leaksCount = (leaksCount == 0) ? INITIALSIZE 
			                               : leaksCount * 2;
		leaks = (Leak*)LT_REALLOC(leaks, 
					  sizeof(Leak) * new_leaksCount);
		if (!leaks) {
			fprintf(report, "# LeakTracer realloc failed: %m\n");
			_exit(1);
		}
		else {
			fprintf(report, "# internal buffer now %d\n", 
				new_leaksCount);
			fflush(report);
		}
		memset(leaks+leaksCount, 0x00,
		       sizeof(Leak) * (new_leaksCount-leaksCount));
		leaksCount = new_leaksCount;
	}
}

void LeakTracer::hexdump(const unsigned char* area, int size) {
	fprintf(report, "# ");
	for (int j=0; j < size ; ++j) {
		fprintf (report, "%02x ", *(area+j));
		if (j % 16 == 15) {
			fprintf(report, "  ");
			for (int k=-15; k < 0 ; k++) {
				char c = (char) *(area + j + k);
				fprintf (report, "%c", isprint(c) ? c : '.');
			}
			fprintf(report, "\n# ");
		}
	}
	fprintf(report, "\n");
}


void LeakTracer::addStackTraceToReport( Leak inLeak ) {
#ifndef NO_STACK_TRACE
        fprintf( report, "    S|" );
        for( int s=0; s<inLeak.stackTraceSize; s++ ) {
                fprintf( report, "%p", inLeak.stackTraceAddresses[s] );
                if( s < inLeak.stackTraceSize - 1 ) {
                        fprintf( report, "|" );
                    }
                else {
                        fprintf( report, " " );
                    }
            }
#endif
    }



void LeakTracer::registerFree (void *p, bool type) {
	initialize();

	if (p == NULL)
		return;

	if (destroyed) {
		fprintf(stderr, "Oops, allocation destruction of LeakTracer (p=%p)\n", p);
		return;
	}

#ifdef THREAD_SAVE
	//pthread_mutex_lock(&mutex);
    mutex.lock();
#endif
	int *lastPointer = &leakHash[ ADDR_HASH(p) ];
	int i = *lastPointer;

	while (i != 0 && leaks[i].addr != p) {
		lastPointer = &leaks[i].nextBucket;
		i = *lastPointer;
	}

	if (leaks[i].addr == p) {
		*lastPointer = leaks[i].nextBucket; // detach.
		newCount--;
		leaks[i].addr = NULL;
		currentAllocated -= leaks[i].size;
		if (i < firstFreeSpot)
			firstFreeSpot = i;

		if (leaks[i].type != type) {
			fprintf(report, 
				"S %10p %10p  ",
				leaks[i].allocAddr,
				__builtin_return_address(1) );
			
            addStackTraceToReport( leaks[i] );
            
            fprintf(report, 
				"# new%s but delete%s "
				"; size %d\n",
				((!type) ? "[]" : " normal"),
				((type) ? "[]" : " normal"),
				leaks[i].size );

			progAbort( NEW_DELETE_MISMATCH );
		}
#ifdef MAGIC
		if ((SAVESIZE >= sizeof(magic_t)) && 
		    *((magic_t*)((char*)p + leaks[i].size)) != MAGIC) {
			fprintf(report, "O %10p %10p ",
				leaks[i].allocAddr,
				__builtin_return_address(1) );

            addStackTraceToReport( leaks[i] );
            
            fprintf(report, "# memory overwritten beyond allocated"
				" %d bytes\n",
				leaks[i].size);

			fprintf(report, "# %d byte beyond area:\n",
				SAVESIZE);
			hexdump((unsigned char*)p+leaks[i].size,
				SAVESIZE);
			progAbort( OVERWRITE_MEMORY );
		}
#endif

#ifdef THREAD_SAVE
#  ifdef MEMCLEAN
		int allocationSize = leaks[i].size;
#  endif
		//pthread_mutex_unlock(&mutex);
        mutex.unlock();
#else
#define             allocationSize leaks[i].size
#endif

#ifdef MEMCLEAN
		// set it to some garbage value.
		memset((unsigned char*)p, MEMCLEAN, allocationSize + SAVESIZE);
#endif
		LT_FREE(p);
		return;
	}

#ifdef THREAD_SAVE
	//pthread_mutex_unlock(&mutex);
    mutex.unlock();
#endif
	fprintf(report, "D %10p             # delete non alloc or twice pointer %10p\n", 
		__builtin_return_address(1), p);
	progAbort( DELETE_NONEXISTENT );
}


void LeakTracer::writeLeakReport() {
	initialize();

	if (newCount > 0) {
		fprintf(report, "# LeakReport\n");
		fprintf(report, "# %10s | %9s ",
			"From new @", "Size");
#ifndef NO_STACK_TRACE
        fprintf( report, " | Stack " );
#endif
        fprintf( report, " #   Address,   Contents\n" );
        
	}
	for (int i = 0; i <  leaksCount; i++)
		if (leaks[i].addr != NULL) {
			fprintf(report, "L %10p   %9ld",
				leaks[i].allocAddr,
				(long) leaks[i].size );

            addStackTraceToReport( leaks[i] );

            // write memory contents of leaked memory in a safe way
            // it may no longer be accessible (deallocated some other way)
            // so we shouldn't access it directly.
            // But the system call "write" can do it.
            // See:
            // https://stackoverflow.com/questions/14507524/
            //         testing-whether-memory-is-accessible-in-linux
            fprintf( report, "  # %p,   \"", leaks[i].addr );
            fflush( report );
            
            // this uses file descriptor directly so is not buffered
            ssize_t writeResult = 
                    write( fileno( report ), leaks[i].addr, leaks[i].size );
            
            if( writeResult == -1 ) {
            	fprintf( report, " [reading memory contents failed] " );
            }
            
            fprintf( report, "\"\n" );
		}
	fprintf(report, "# total allocation requests: %6ld ; max. mem used"
		" %d kBytes\n", totalAllocations, maxAllocated / 1024);
	fprintf(report, "# leak %6d Bytes\t:-%c\n", currentAllocated,
		(currentAllocated == 0) ? ')' : '(');
	if (currentAllocated > 50 * 1024) {
		fprintf(report, "# .. that is %d kByte!! A lot ..\n", 
			currentAllocated / 1024);
	}
}

/** -- The actual new/delete operators -- **/

void* operator new(size_t size) {
	return leakTracer.registerAlloc(size,false);
}


void* operator new[] (size_t size) {
	return leakTracer.registerAlloc(size,true);
}


void operator delete (void *p) {
	leakTracer.registerFree(p,false);
}


void operator delete[] (void *p) {
	leakTracer.registerFree(p,true);
}


// added by Jason Rohrer
char *strdup( const char *inString ) {
    char *outString =
        (char*)leakTracer.registerAlloc( strlen( inString ) + 1, true );
    strcpy( outString, inString );
    return outString;
    }


/* Emacs: 
 * Local variables:
 * c-basic-offset: 8
 * End:
 * vi:set tabstop=8 shiftwidth=8 nowrap: 
 */
