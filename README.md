# KANEK EXTENTS STORAGE (KES)
This is a library designed for 

### 3       ECL Design
For the design of the Extents Caching Library, a Block size for the 
graph file system will be 8 Kbytes in size. So this is the standard 
block size. 


####        3.1 Extents cache
Extents are groups of contiguous disk blocks. So, in order to make
faster IO operations, an extents cache will be used. An extent needs
a device block address and a number of contiguous block 
so this is the extent representation. This is mapped to memory. 

It works in a similar way to a page cache, but we are caching extents
in memory and not only pages. So ideally a thread would be in charge 
of this cache, flushing out extents marked as "dirty" and keep in 
memory any "pinned" extent. Also operations like "sync" should flush
out all the cached extents in memory.

This extents cache makes lot of use or EIO, the framework cache and 
the bitmap library.

So, it provides most of the functionalities of the EIO library, plus the
caching functionalities of the cache framework and updating the corresponding
bitmap as needed. 

Some of the interfaces:
- Make an extents block 
- Open/Read an specific extent (read an extent into memory)
- Flush extent ( write into storage, mark the bitmap as occupied)
- Reserve extent ( mark the bitmap as occupied, 
