/* Dupmerge - Reclaim disk space by deleting redundant copies of files and
 * creating hardlinks in their place

 * Phil Karn, KA9Q
 * karn@ka9q.net
 * http://www.ka9q.net/

 * New algorithm. I no longer do the duplicate unlinking and relinking inside the qsort comparison routine.
 * It was a cute idea, but probably not as good as just producing a list of files, sorting it by size,
 * and running through that.
 * Latest rewrite: May 2010
 * The May 2010 version simplifies the algorithm (only one sort is performed) and fixes bugs in the handling of more
 * than two identical copies of a single file. This version should reliably identify and link all identical copies.

 * This program reads from standard input a list of files (such
 * as that generated by "find . -print") and discovers which files are
 * identical. Dupmerge unlinks one file of each identical pair and
 * recreates its path name as a link to the other.
 *
 * Non-plain files in the input (directories, pipes, devices, etc)
 * are ignored.  Identical files must be on the same file system to be linked.
 *
 * Dupmerge prefers to keep the older of two identical files, as the older
 * timestamp is more likely to be the correct one given that many
 * copy utilities (e.g., 'cp') do not by default preserve modification
 * times.
 *
 * Command line arguments:
 * -0 Delimit file names with nulls rather than newlines; for use with 'find -print0'
 * -q Operate in quiet mode (otherwise, relinks are displayed on stdout)
 * -f Fast (very fast) mode that bypasses an exhaustive file comparison in favor of modification timestamps.
 *    If the two files are the same size, have the same basename and exactly the same timestamp, they're probably the same.
 *    This method seems to work well enough with rsync.
 * -t threshold_size
 *    Apply the fast mode feature only to files larger than threshold_size bytes (default 100,000)
 * -n Dry run: simply list the actions that would be taken without actually unlinking or linking anything. Turns off -q.
 * -s By default, files are sorted in decreasing order of size so that the actual unlinking of duplicate files starts with
 *    the largest files. This recovers disk space as quickly as possible, but if for some reason you want to start with the
 *    smallest files, use this flag.
 *
 * Trivia: this program was inspired by a cheating scandal in the introductory computer science course CS-100
 * at Cornell University the year after I graduated. The TAs simply sorted the projects by object code size and compared
 * those that were equal. That effectively found copies where only the variable names and comments had been changed.
 *
 * March 2009: the sort comparison function looks at the first page of each file when they're the same size.
 * This greatly reduces the number of comparisons done after the sort.
 *
 * Copyright Phil Karn, karn@ka9q.net. May be used under the terms of the GNU General Public License v 2.0
 *
 * $Id: dupmerge.c,v 0.22 2010/05/09 12:51:02 karn Exp karn $
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libgen.h>
#include <sys/mman.h>
#include <limits.h>
#include <unistd.h>
#include <regex.h>
#include <openssl/sha.h>

/* Darwin (OSX) has this, but Linux apparently doesn't */
#ifndef MAP_NOCACHE
#define MAP_NOCACHE (0)
#endif
/* Linux has this and OSX apparently doesn't */
#ifndef MAP_POPULATE
#define MAP_POPULATE (0)
#endif

#define HASHSIZE 20
#define PAGESIZE (4096)

enum flag { NO=0,YES=1,UNKNOWN=-1 };

enum flag Fast_flag = NO;
enum flag Zero_flag = NO;
enum flag Quiet_flag = NO;
enum flag Fast_threshold = 100000;
enum flag No_do = NO;
enum flag Small_first = NO;

/* Statistics counts */
unsigned Regular_file = 0;
unsigned FIFO = 0;
unsigned Character_special = 0;
unsigned Directory = 0;
unsigned Block_special = 0;
unsigned Symbolic_link = 0;
unsigned Socket = 0;
unsigned Whiteout = 0;
unsigned Null_pathname = 0;
unsigned Total_files = 0;
unsigned Empty = 0;
unsigned Stat_fail = 0;
unsigned Not_accessible = 0;
unsigned Files_deleted = 0;
long long Blocks_reclaimed = 0;
long long Full_hashes_computed = 0;
long long Block_hashes_computed = 0;
long long Full_hash_hits = 0;
long long Block_hash_hits = 0;
long long Partial_hit_full_fail = 0;
long long Unlinks = 0;
long long Unlink_failures = 0;
long long Map_fails = 0;

#define ENTRYCHUNK 5000 /* Allocation unit for file table */

/* File table entry */
struct entry {
  char *pathname; /* Path name, reserved space for null */
  struct stat statbuf; /* file i-node data */
  unsigned char partialhash[HASHSIZE];
  unsigned char filehash[HASHSIZE];
  int partialhash_present:1;
  int filehash_present:1;
};

/* Comparison functions */
int comparison_equal(const void *ap,const void *bp); /* Does most of the work */
int comparison_sort(const void *ap,const void *bp); /* Version called by qsort() */

int main(int argc,char *argv[]){
  int i,j;
  struct entry *entries = NULL; /* Dynamically allocated file table */
  struct entry *ep;
  int entryarraysize = 0; /* Start with empty table, allocate on first pass */
  int nfiles;

  /* Process command line args */
  {
    char c;

    while((c = getopt(argc,argv,"snqf0t:")) != EOF){
      switch(c){
      default:
	fprintf(stderr,"Usage: %s [-s] [-n] [-q] [-f] [-0] [-t threshold_size]\n",argv[0]);
	break;
      case 's':
	Small_first = YES;
	break;
      case 'n':
	No_do = YES;
	break;
      case 'q':
	Quiet_flag = YES;
	break;
      case 'f':
	Fast_flag = YES; /* Just compare modification timestamps, not file contents */
	break;
      case '0':
	Zero_flag = YES; /* Path names are delimited by nulls, e.g., from 'find . -print0' */
	break;
      case 't':
	Fast_threshold = atoi(optarg);
	break;
      }
    }
  }
  if(No_do && Quiet_flag){
    fprintf(stderr,"%s: -q flag forced off with -n set\n",argv[0]);
    Quiet_flag = NO; /* Force off */
  }

  /* Start by reading stdin for list of path names
   * Check each one and ignore non-regular files, zero-length files, special files, errors, etc
   */
  ep = NULL;
  for(nfiles=0;!feof(stdin) && !ferror(stdin);){
    int ch;
    char pathname[PATH_MAX+1];

    /* Expand file table if necessary and possible */
    if(nfiles >= entryarraysize){
      entries = (struct entry *)realloc(entries,(entryarraysize + ENTRYCHUNK) * sizeof(struct entry));
      assert(entries != NULL);
      entryarraysize += ENTRYCHUNK;
    }
    ep = &entries[nfiles];
    for(i=0;i< PATH_MAX;i++){
      
      if(EOF == (ch = getc(stdin)) || '\0' == ch || (!Zero_flag && '\n' == ch))
	break;

      pathname[i] = ch;
    }      
    if(ch == EOF)
      break;

    pathname[i] = '\0';

    Total_files++;

    /* Ignore null file names */
    if(strlen(pathname) == 0){
      Null_pathname++;
      continue; /* Reuse entry for next file */
    }
    /* If we can't stat it, ignore it */
    if(lstat(pathname,&entries[nfiles].statbuf) != 0){
      Stat_fail++;
      continue;
    }

#if DEBUG
#ifdef __darwin__
    {
    fprintf(stderr," nlink %d; uid %d; gid %d; atime %ld.%09ld; mtime %ld.%09ld; ctime %ld.%09ld; size %lld; gen %d %s\n",
	    ep->statbuf.st_nlink,ep->statbuf.st_uid,ep->statbuf.st_gid,
	    ep->statbuf.st_atimespec.tv_sec,ep->statbuf.st_atimespec.tv_nsec,
	    ep->statbuf.st_mtimespec.tv_sec,ep->statbuf.st_mtimespec.tv_nsec,
	    ep->statbuf.st_ctimespec.tv_sec,ep->statbuf.st_ctimespec.tv_nsec,
	    (long long)ep->statbuf.st_size,ep->statbuf.st_gen,pathname);
    }
#else
    {
    fprintf(stderr," nlink %d; uid %d; gid %d; atime %ld; mtime %ld; ctime %ld; size %lld %s\n",
	    ep->statbuf.st_nlink,ep->statbuf.st_uid,ep->statbuf.st_gid,
	    ep->statbuf.st_atime,
	    ep->statbuf.st_mtime,
	    ep->statbuf.st_ctime,
	    (long long)ep->statbuf.st_size,pathname);
    }
#endif
#endif
    /* Ignore all but ordinary files */
    switch(ep->statbuf.st_mode & S_IFMT){
    case S_IFREG:
      Regular_file++;
      break;
    case S_IFIFO:
      FIFO++;
      break;
    case S_IFCHR:
      Character_special++;
      break;
    case S_IFDIR:
      Directory++;
      break;
    case S_IFBLK:
      Block_special++;
      break;
    case S_IFLNK:
      Symbolic_link++;
      break;
    case S_IFSOCK:
      Socket++;
      break;
#ifdef S_IFWHT
    case S_IFWHT:
      Whiteout++; /* What's this? */
      break;
#endif
    }
    if((ep->statbuf.st_mode & S_IFMT) != S_IFREG)
      continue;

    /* Ignore empty files and files with no assigned data blocks (any data being stored in the inode).
     * Zero size files are often used as flags and locks we don't want to upset. And we won't recover
     * any data blocks from a file without any data blocks!
     * I should also exclude HFS files on OSX with resource forks
     */
    if(ep->statbuf.st_blocks == 0 || ep->statbuf.st_size == 0){
      Empty++;
      continue;
    }
    /* Ignore files we can't read */
    if(access(pathname,R_OK) == -1){
      Not_accessible++;
      continue;
    }
    /* Otherwise keep the filename and inode on our list */
    ep->pathname = strdup(pathname);
    nfiles++;
  }
  if(!Quiet_flag){
    fprintf(stderr,"%s: input files: total %u; ordinary %u",argv[0],Total_files,Regular_file);
    if(FIFO)
      fprintf(stderr,"; FIFO %u",FIFO);
    if(Character_special)
      fprintf(stderr,"; char special %u",Character_special);
    if(Directory)
      fprintf(stderr,"; directories %u",Directory);
    if(Block_special)
      fprintf(stderr,"; block specials %u",Block_special);
    if(Symbolic_link)
      fprintf(stderr,"; symbolic links %u",Symbolic_link);
    if(Socket)
      fprintf(stderr,"; sockets %u",Socket);
#ifdef S_IFWHT
    if(Whiteout)
      fprintf(stderr,"; whiteouts %u",Whiteout);
#endif
    if(Empty)
      fprintf(stderr,"; empties %u",Empty);
    putc('\n',stderr);

    if(Null_pathname)
      fprintf(stderr,"%s: null pathnames %u\n",argv[0],Null_pathname);

    if(Stat_fail)
      fprintf(stderr,"%s: stat failures %u\n",argv[0],Stat_fail);

    if(Not_accessible)
      fprintf(stderr,"%s: files not accessible %u\n",argv[0],Not_accessible);

    if(nfiles == 0){
      fprintf(stderr,"%s: no files left to examine\n",argv[0]);
      exit(0);
    }

  }
#if DEBUG
  for(i=0;i<nfiles;i++){
    fprintf(stderr,"%lld %s\n",(long long)entries[i].statbuf.st_size,entries[i].pathname);
  }
#endif

  if(No_do){
    fprintf(stderr,"%s: dry run, no files will actually be unlinked\n",argv[0]);
  }
  
  /* Sort by file size/device/mod time/nlinks */
  qsort(entries,nfiles,sizeof(struct entry),comparison_sort);
  if(!Quiet_flag)
    fprintf(stderr,"%s: sort done, %d entries\n",argv[0],nfiles);
    
#if DEBUG
  for(i=0;i<nfiles;i++){
    fprintf(stderr,"%lld %d %s\n",
	    (long long)entries[i].statbuf.st_size,
	    (int)entries[i].statbuf.st_nlink,
	    entries[i].pathname);
  }
#endif
  /* Walk through first of each group of files that are candidates for being the same
   *  This is the reference file
   */
  for(i=0;i<nfiles-1;i++){
    
    /* Ignore hard links to earlier reference files */
    if(entries[i].pathname == NULL)
      continue;

    /* The qsort grouped together all files with the same size on the same device
     * Scan forward for all files with the same size and device as the reference file
     */
    for(j=i+1;
	j<nfiles
	  && entries[i].statbuf.st_size == entries[j].statbuf.st_size
	  && entries[i].statbuf.st_dev == entries[j].statbuf.st_dev;
	j++){
      
      
      /* Ignore hard links to earlier reference files */
      if(entries[j].pathname == NULL)
	continue;

      if(entries[i].statbuf.st_dev == entries[j].statbuf.st_dev
	 && entries[i].statbuf.st_ino == entries[j].statbuf.st_ino){
	/* Existing hard link to reference file; mark so we'll skip over it later */
	free(entries[j].pathname);
	entries[j].pathname = NULL;
	continue;
      }
      if(comparison_equal(&entries[i],&entries[j]) == 0){
	/* Distinct files with identical contents on same file system, can be linked */
	if(!Quiet_flag){
	  fprintf(stderr,"%s: %lld ln %s -> %s\n",argv[0],(long long)entries[j].statbuf.st_size,entries[j].pathname,entries[i].pathname);
	  //	  fprintf(stderr,"debug: inodes %lld %lld\n",(long long)entries[j].statbuf.st_ino,(long long)entries[i].statbuf.st_ino);
	}
	if(entries[j].statbuf.st_nlink == 1){
	  /* Pathname has single remaining link, so its blocks will be recovered */
	  Blocks_reclaimed += entries[j].statbuf.st_blocks;
	}
	
	{
	  /* Some last minute paranoid checks */
	  struct stat statbuf_a,statbuf_b;
	  
	  if(lstat(entries[i].pathname,&statbuf_a)){
	    fprintf(stderr,"%s: can't lstat(%s): %d %s\n",argv[0],entries[i].pathname,errno,strerror(errno));
	    abort();
	  }
	  if(lstat(entries[j].pathname,&statbuf_b)){
	    fprintf(stderr,"%s: can't lstat(%s): %d %s\n",argv[0],entries[j].pathname,errno,strerror(errno));
	    abort();
	  }
	  assert(statbuf_a.st_size == statbuf_b.st_size);
	  assert(statbuf_a.st_ino != statbuf_b.st_ino);
	  assert(statbuf_a.st_dev == statbuf_b.st_dev);
	  assert(statbuf_a.st_mtime <= statbuf_b.st_mtime);
	}
	if(!No_do){
	  if(unlink(entries[j].pathname)) {
	    Unlink_failures++;
	    fprintf(stderr,"%s: can't unlink(%s): %d %s\n",argv[0],entries[j].pathname,errno,strerror(errno));
	  } else if(link(entries[i].pathname,entries[j].pathname)){
	    /* Should never fail */
	    fprintf(stderr,"%s: can't link(%s,%s): %d %s\n",argv[0],entries[i].pathname,entries[j].pathname,errno,strerror(errno));
	    abort();
	  }
	}
	/* Don't use this entry as a reference file later */
	free(entries[j].pathname);
	entries[j].pathname = NULL;
	Unlinks++;
      } /* if same */
    } /* End of inner for() loop */
  } /* end of for loop looking ahead for match with file i */
  if(!Quiet_flag){
    if(No_do)
      fprintf(stderr,"%s: This was a dry run; no files were actually unlinked.\n",argv[0]);
    if(Unlinks)
      fprintf(stderr,"%s: Unlinks: %llu; Unlink failures: %llu; disk blocks reclaimed: %llu\n",argv[0],Unlinks,Unlink_failures,Blocks_reclaimed);

    fprintf(stderr,"%s: First page hashes: %llu; hits %llu\n",argv[0],Block_hashes_computed,Block_hash_hits);
    fprintf(stderr,"%s: Full file hashes: %llu; hits: %llu; full file hash mismatches: %llu; map fails: %llu\n",argv[0],Full_hashes_computed,Full_hash_hits,Partial_hit_full_fail,Map_fails);
  }
  /* Not really necessary since we're exiting */
  free(entries);
  exit(0);
}


/* Compare files by size (used by second sort)
 * Return 0 means same size *and* on same device
 * Returning <0 causes the first argument to sort toward the top of the list
 * Returning >0 causes the second argument to sort toward the top of the list

 * We want the largest files to go to the top of the list, so "smaller is greater".
 * We also want empty entries to go to the end of the list, so they are always "greater" 
 */
int comparison_sort(const void *ap,const void *bp){
  struct entry *a,*b;

  a = (struct entry *)ap;
  b = (struct entry *)bp;

  assert(a != NULL);
  assert(b != NULL);

  if(a == b)
    return 0; /* Can this happen? */

  /* Push all invalid entries to the bottom of the sort */
  if(!b->pathname && !a->pathname)
    return 0;
  if(!b->pathname)
    return -1;
  if(!a->pathname)
    return 1;

  /* Distinguish first by size.
   * By default, bigger files sort first unless overridden with -s option
   */
  if(b->statbuf.st_size != a->statbuf.st_size){
    if(Small_first)
      return a->statbuf.st_size - b->statbuf.st_size;
    else
      return b->statbuf.st_size - a->statbuf.st_size;
  }
  /* Files are same size; distinguish if on different device; ordering is unimportant */
  if(b->statbuf.st_dev != a->statbuf.st_dev)
    return b->statbuf.st_dev - a->statbuf.st_dev;

  /* Same size, same device; distinguish by modification time, older files first */
  if(a->statbuf.st_mtime != b->statbuf.st_mtime)
    return a->statbuf.st_mtime - b->statbuf.st_mtime;

  /* Order equal, same time files with fewer links later so they'll be preferentially deleted sooner */
  if(a->statbuf.st_nlink != b->statbuf.st_nlink)
    return b->statbuf.st_nlink - a->statbuf.st_nlink;

  /* Same size, same device, same modification time, same number of links; includes case of two links to same inode */
  return 0;
}


void get_small_hash(struct entry *ep);
void get_big_hash(struct entry *ep);

int comparison_equal(const void *ap,const void *bp){
  struct entry *a,*b;
  int i;

  a = (struct entry *)ap;
  b = (struct entry *)bp;

  if(a == b)
    return 0; /* Can this happen? */

  assert(a != NULL);
  assert(b != NULL);

  if(a->statbuf.st_dev == b->statbuf.st_dev && a->statbuf.st_ino == b->statbuf.st_ino)
    return 0; /* Files are already hard-linked, so they're the same */

  /* Make file size the most significant part of the comparison */
  if(b->statbuf.st_size != a->statbuf.st_size)
    return b->statbuf.st_size - a->statbuf.st_size;

  /* Same-size files sort together only when they're on the same device */
  if(b->statbuf.st_dev != a->statbuf.st_dev)
    return b->statbuf.st_dev - a->statbuf.st_dev;


  /* Optionally use the rsync heuristic -- if the files have the same size, mod timestamp and
   * the same base name, declare them the same without actually reading the contents
   * Do this only on files larger than Fast_threshold to further reduce chances of false equality
   */
  if(Fast_flag && a->statbuf.st_size > Fast_threshold){	/* Rsync-style fast comparison */
    char *bn1,*bn2;
    
    /* I could use the built-in basename() function, but what a mess it is */
    if((bn1 = strrchr(a->pathname,'/')) == NULL)
      bn1 = a->pathname;
    if((bn2 = strrchr(b->pathname,'/')) == NULL)
      bn2 = b->pathname;
    
    /* Are the basenames and mod times identical? */
    if(0 == strcmp(bn1,bn2) && a->statbuf.st_mtime == b->statbuf.st_mtime)
      return 0;
  }

  /* Next order of business: compare the partial file hashes */
  if(!a->partialhash_present){
    get_small_hash(a);
  } else
    Block_hash_hits++;

  if(!b->partialhash_present){
    get_small_hash(b);
  } else
    Block_hash_hits++;

  i = memcmp(a->partialhash,b->partialhash,HASHSIZE);
  if(i != 0) /* They differ, no need to continue */
    return i;

  /* Partial hashes are the same, compare full file hashes */
  if(!a->filehash_present){
    get_big_hash(a);
  } else 
    Full_hash_hits++;

  if(!b->filehash_present){
    get_big_hash(b);
  } else
    Full_hash_hits++;

  i = memcmp(a->filehash,b->filehash,HASHSIZE);
  if(i != 0){
    Partial_hit_full_fail++;
    return i;
  }
  return 0; /* We've passed the gauntlet; the files are the same! */
}

void get_small_hash(struct entry *ep){
  if(!ep->filehash_present){
    int fd,i,len;
    void *p;

    Block_hashes_computed++;
    if((fd = open(ep->pathname,O_RDONLY)) == -1){
      fprintf(stderr,"can't open(%s): %d %s\n",ep->pathname,errno,strerror(errno));
      abort();
    }
    assert(fd != -1);
    len = ep->statbuf.st_size > PAGESIZE ? PAGESIZE : ep->statbuf.st_size;
    p = mmap(NULL, len, PROT_READ, MAP_NOCACHE|MAP_FILE|MAP_SHARED, fd, 0);
    assert(p != MAP_FAILED); /* No reason for it to fail */
    SHA1(p,len,ep->partialhash);
    i = munmap(p,len);
    assert(i == 0);
    i = close(fd);
    assert(i == 0);
    ep->partialhash_present = 1;
  }
}

void get_big_hash(struct entry *ep){

  if(!ep->filehash_present){
    int fd,i;
    void *p;

    Full_hashes_computed++;
    if((fd = open(ep->pathname,O_RDONLY)) == -1){
      fprintf(stderr,"can't open(%s): %d %s\n",ep->pathname,errno,strerror(errno));
      abort();
    }
    assert(fd != -1);
    p = mmap(NULL, ep->statbuf.st_size, PROT_READ, MAP_NOCACHE|MAP_FILE|MAP_SHARED|MAP_POPULATE, fd, 0);
    if(p != MAP_FAILED){
      SHA1(p,ep->statbuf.st_size,ep->filehash);
      i = munmap(p,ep->statbuf.st_size);
      assert(i == 0);
    } else { /* Not enough address space to map entire file? */
      unsigned long len;
      SHA_CTX context;
      char buffer[PAGESIZE];

      Map_fails++;

      SHA1_Init(&context);
      while((len = read(fd,buffer,PAGESIZE)) > 0){
	SHA1_Update(&context,buffer,len);
      }
      if(len < 0){
	fprintf(stderr,"Read error on %s: %d %s\n",ep->pathname,errno,strerror(errno));
	abort();
      }
      SHA1_Final(ep->filehash,&context);
    }
    i = close(fd);
    assert(i == 0);
    ep->filehash_present = 1;
  }
}

