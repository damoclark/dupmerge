Dupmerge 
Phil Karn, KA9Q
===================

**dupmerge** reclaims the space used by redundant copies.

* * * * *

[dupmerge man page](http://www.ka9q.net/code/dupmerge/dupmerge.1)<br/>
 [dupmerge.c source code](http://www.ka9q.net/code/dupmerge/dupmerge.c)<br/>
 [dupmerge binary for OSX Snow Leopard (64 bit)](http://www.ka9q.net/code/dupmerge/dupmerge-osx)<br/>
 [dupmerge binary for i386 Linux](http://www.ka9q.net/code/dupmerge/dupmerge-linux-i386)

* * * * *

### Unix Hard Links

To use either command properly you *must* be familiar with the concept of the [hard link](http://en.wikipedia.org/wiki/Hard_link), so I'll provide a quick refresher. It originated in [UNIX](http://en.wikipedia.org/wiki/UNIX), so it has long been part of UNIX clones like [Linux](http://en.wikipedia.org/wiki/Linux), [BSD](http://en.wikipedia.org/wiki/BSD), and [Mac OSX](http://en.wikipedia.org/wiki/Mac%20OSX). It may have also been picked up by other operating systems with which I am unfamiliar.

It might not seem like it, but UNIX files do not actually have names; internally they are uniquely identified by device and [inode](http://en.wikipedia.org/wiki/inode) numbers. The user refers to them with [path names](http://en.wikipedia.org/wiki/Path%20%28computing%29), printable strings that define a series of directories delimited by slashes (/) -- a path -- that ultimately point to the inode and the file's metadata and contents. Every file (inode) has at least one path name pointing to it. When more than one path name points to the same inode, they are all equally valid; there is no preferred or primary path name. These path names are sometimes referred to as "hard links", as distinguished from the ["soft" or "symbolic" link](http://en.wikipedia.org/wiki/Symbolic%20link), a special kind of file that contains a path name (possibly one of several hard links!) pointing to another file.

The **dupmerge** command works by reading a list of path names, identifying those that point to distinct inodes with the same content, deleting all but one, and recreating the path names for the deleted copies as hard links to the remaining copy. When it finishes, all the path names given to the program still exist, and each provides the same content when read. However, the redundant copies have been deleted and the space reclaimed by the filesystem free list.

Here is a simple shell command that invokes dupmerge on all the files in the current directory and under any and all subdirectories:

```bash
find . -print0 | dupmerge -0
```

**dupmerge** automatically ignores all directories, special device files, FIFOs -- everything that isn't an ordinary file -- so you don't have to bother with any fancy arguments to the **find** command. When it's done, every path name will still be present and yield the same contents when accessed. But any path names that had referred to separate copies of the same data are now hard links to a single copy.

Running dupmerge can still make some visible differences and it is important to understand them before you use the program on an important file system because there is, as yet, no way to reverse the effects of **dupmerge**. In UNIX, a file's metadata belongs to the inode, not the path name. The metadata includes the ownership and group membership of the file, its access permissions, and the times of its creation and (if enabled) last access and inode modification. Since only one inode in each group of inodes with the same contents will survive an execution of **dupmerge**, file ownerships, permissions and modification timestamps may change for certain pathnames even though the *contents* remain unchanged. The metadata on the deleted copies of the file is currently lost, though a future version of dupmerge may save this data in an "undo" file so that its effects could be undone. Until then, it is best to limit dupmerge to directory hierarchies whose files have a single owner and group, and to anticipate any problems that might be caused by the loss of metadata on the redundant copies that will be deleted. **Dupmerge** always keeps the oldest copy among a group of identical files on the principle that the oldest timestamp is most likely the actual time that the data was created, with the newer timestamps merely reflecting the time that each copy was made from the original.

Because empty files are often used as system lock files, with significance in the inode metadata, **dupmerge** ignores them by default. (There's little to reclaim by deleting an empty file anyway.)

### How to Compile
Make sure you have openssl library and headers installed.  On Redhat-based systems, this means:

```bash
$ sudo yum install openssl openssl-devel
```

On Debian-based systems:

```bash
$ sudo apt install libssl-dev
```

Then to compile, type:

```bash
$ gcc -O3 -fexpensive-optimizations -o dupmerge dupmerge.c -lssl -lcrypto
```

Move the binary to an appropriate location ```/usr/local/bin``` is sensible.
Move the man page ```dupmerge.1``` similarly to an appropriate location ```/usr/local/man/man1```.

### Dupmerge Options

#### -0

By default, each file name is terminated with either a newline or a null. This option specifies that each file name can only be terminated with a null. (Use find's -print0 option to generate such a list.)

#### -z

By default, empty files are ignored. They are often used as locks, and disturbing them could cause problems. Besides, empty files have no assigned blocks so there's not much to be gained by deleting them (other than the inode entry, of course). This option overrides this behavior; when set, all empty files will be linked together.

#### -s

By default, the list of files is sorted by decreasing size and the largest files are examined first.

I often run dupmerge when a file system is out of (or is about to run out of) free space, so scanning the largest files first usually recovers space more quickly. I can't think of a reason to examine the smallest files first, but if you can this option will do it.

#### -n

This flag tells dupmerge to conduct a dry run. It will appear to operate normally, but without actually unlinking or relinking any files.

#### -q

Suppress the progress messages that dupmerge normally generates, including the unlink/link commands generated for each duplicate file. Error messages are still displayed. This flag is forced off by the -n flag.

#### -f

Enable a shortcut comparison heuristic that can substantially speed up the program under certain special cases. When set, a pair of files with the same basename (the part of the path name after the last '/'), the same size, and the same modification date will be considered identical without actually comparing their contents. This can be an important special case as many duplicates are created by copying a directory hierarchy with a utility like rsync, tar or cpio that preserves modification times. The widely used rsync program uses this heuristic by default so it doesn't seem terribly unsafe, but keep in mind the (small) risk that two different files might be erroneously considered identical. For this reason I recommend avoiding this flag when possible; the default file comparison strategy is actually quite fast.

#### -t

The probability that two different files will have the same size decreases with increasing file size. To reduce the risk of false matches with the -f option, files less than a certain size will nonetheless be compared by their hash codes even when -f is selected. The default size threshold is 100,000 bytes; this option allows another threshold to be set.

### Notes on dupmerge

My first version of this program circa 1993 worked by computing MD5 hashes of every file, sorting the hashes and then looking for duplicates. This worked but it was unnecessarily slow. One reason was that it computed a hash for every file, including those with unique sizes that couldn't possibly have any duplicates (duplicate files always have the same size!)

My [second version](http://www.ka9q.net/code/dupmerge/dupmerge2.c) circa 1999 unlinked the duplicates as a side effect of the sort comparison function.

I have since rewritten it again from scratch. It begins by sorting the file list by size. Files with unique sizes are ignored; two or more files having the same size are compared by their SHA-1 hashes. Comparing hashes, as opposed to actual file contents, has significant performance benefits in most real-world workloads. Each file can be read sequentially, without any disk seeks, and its hash cached so that it need only be read once no matter how many other files have the same size.

But there are nonetheless pathological situations where this algorithm performs much more slowly than direct file-to-file comparison. Consider a collection of a thousand unique files, each exactly 10 megabytes in size. The hash comparison strategy involves reading every file in its entirety while a direct file-to-file comparison can abort as soon as the first difference is detected. Most files, if different, differ near their beginnings.

To handle this possibility with reasonable efficiency, comparing a pair of files actually entails a series of steps. First, both files must have the same size and be on the same file system. (Hard links cannot extend across file systems.) Second, I compute and compare the SHA-1 hash of the first page (4 KB) of each file. This will catch most differing files. If, and only if, the first 4KB of each file have the same hash do I proceed to compute and compare the SHA-1 hash for each entire file. This performs well in practice because most files that differ at all will do so in the first 4KB. It is unusual for two files to have the same size and the same first page, yet differ beyond that. However, it is certainly possible, which is why it is still necessary to compare the complete hashes before declaring two files to be identical.

Every hash result (on the leading page or over the full file) is cached so it never has to be computed more than once. This improves performance substantially when there are many files of the same size.

The program displays copious statistics at the end of execution, such as the number of disk blocks reclaimed from duplicates, the number of first-page and complete-file hash function computations, the number of hash "hits" (references to hash values that have already been computed), and the number of same-size file pairs whose first page hashes match but differ in their full-file hashes.


Phil Karn, KA9Q, karn@ka9q.net

*Last updated by Phil: 7 May 2010*<br/>
*Last updated by Damien Clark: 1 May 2016*
