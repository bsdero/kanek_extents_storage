# KANEK EXTENTS STORAGE (KES)
This is a library designed for low level block/extents storage management. 


###        KES Design and features.
For the design of the Extents Caching Library, some features and functionalities
will be worked on. 


####       KES Internal design      
An extent is a group of contiguous blocks. In order to get access to an 
extent we need starting block address and the number of blocks. 
The starting block address and the number of blocks, together are called an
"extent descriptor". 

"User Extents" are extents with user data. 
An "extent index" is a contiguous array of extents descriptors. 
A "blocks bitmap" is a bitmap which helps to know which blocks in the device
are in use or free. By default the block size is 8KBytes, although 4, 16, 32 or
64 KBytes length per block is also selectable. 
An "Extents storage descriptor" describes:
- Block size 
- Extent index start block address and number of blocks
- Block bitmap start block address and number of blocks
- User extents start block address and number of blocks


The extents disk format help to store extents in a better organized way in 
block storage. It includes the items:
- Extents storage descriptor (optional, also can be stored elsewhere). 
  If stored, it should be stored in the block 0.
- Extents index (optional, can be stored elsewhere). If stored should 
  start at block 1
- Block bitmap (optional, can be stored elsewhere). If stored, should be
  in the last block of the device/file. 
- User extents block address

All of these, is called "extents storage". 

By default, the bitmap is stored at the end of the device, in the last blocks. 
The next interfaces will be provided.

- Create extent storage on file or device.
- Open an existing extent storage file or device.
- Reserve extents 
- Read extents
- Write extents
- Free extents
- Close extents


Below we have a representation of extents storage.
```
        Extent Storage      
+-----------------------------+
| Extent Storage descriptor   |<--This descripts extents storage
+-----------------------------+
| Extents index               |<--This holds extents descriptors
+-----------------------------+
| User extents                |<--User stores data here
|                             |
|                             |
|                             |
|                             |
+-----------------------------+
| Block bitmap                |<--This helps to know which blocks are 
| 010000110011101010101000000 |   in use or free.
+-----------------------------+
```

####       Block size
The default block size is 8 KBytes, although other sizes can be specified:
-4Kb
-8Kb
-16Kb
-32Kb
-64Kb


####       Bitmap management
Code exist already for mark bits and contiguous bit groups. This
functions are useful for bitmap management in block storage. 

####       Extents cache
Extents are groups of contiguous disk blocks. So, in order to make
faster IO operations, an extents cache will be used. An extent needs
a device block address and a number of contiguous block 
so this is the extent representation. This is mapped to memory. 

It works in a similar way to a page cache, but we are caching extents
in memory and not only pages. So ideally a thread would be in charge 
of this cache, flushing out extents marked as "dirty" and keep in 
memory any "pinned" extent. Also operations like "sync" should flush
out all the cached extents in memory.

This extents cache makes lot of use or KFL, and its the cache framework.
The extents cache requires an on-disk bitmap and a full header. Index is optional. 


Some of the interfaces without cache:
- Build/make an extents file ( need to specify if a bitmap, header and index are required)
- Open an extents file
- Close extents file
- Create extent ( mark the bitmap as occupied, alloc a memory buffer)
- Read an specific extent
- Flush extent ( write into storage, mark the bitmap as reserved)
- Delete extent

Operations with cache
- Open extents file, bitmap required, optional index
- Create extents of N blocks
- Map an specific extent into memory
- Flush extent ( write into storage, mark the bitmap as occupied)
- Delete extent
- sync cache
- Pin extent
- Run cache
- Pause cache
- Stop cache
- alloc cache
- free cache
 

