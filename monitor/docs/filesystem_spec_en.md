# FS16 File System — Specification

Revision 3 · Oct 19, 2026 · @Andrey

## 1. Purpose and Conventions

FS16 is a file system for disks of up to 128 MB, using 16-bit cluster numbers. File metadata resides directly in directory entries. File data is located through direct, single-indirect, and double-indirect pointers, while free space is tracked by a bitmap.

| Parameter | Value |
| --- | --- |
| Block (disk sector) | 512 bytes |
| Cluster | 4 blocks = 2048 bytes |
| Cluster number | 16 bits; 0 means “no cluster” |
| Allocation unit | Cluster |
| Directory entry | 64 bytes; 32 entries per cluster |
| Index cluster | 1024 cluster numbers |

Conventions:

- All multibyte fields are little-endian, as on the CPU and disk controller.
- Offsets and indices are zero-based. Word fields have even offsets.
- Names and volume labels use the machine's single-byte character encoding (*CPU16 Machine*, Section 5). Their fields are padded with zeros, but a string that fills its field does not require a terminating zero.
- Unused bytes in on-disk structures are written as zero and ignored when read.
- Cluster N occupies sectors 4N…4N+3. The first sector's LBA is 18 bits wide: LBA\_LO = N << 2, LBA\_HI = N >> 14. In the controller's terminology, a disk larger than 32 MB is not a “small” disk; the driver therefore always writes LBA\_HI.

## 2. Disk Layout

An FS16 volume consists of cluster 0, the allocation bitmap starting at cluster 1, and the remaining data region. The root directory has no reserved data area: its entry is stored in the superblock, while its contents occupy clusters allocated from the shared pool.

| Clusters | Sectors | Contents |
| --- | --- | --- |
| 0 | 0 | Superblock (Section 3) |
| 0 | 1…3 | Boot code, 1536 bytes; zero-filled on a nonbootable volume |
| 1 … B | 4 … 4B+3 | Allocation bitmap (Section 4) |
| B+1 … N−1 | — | Data area: file contents, directories, and index clusters |

Sizes:

- N is the number of clusters on the volume: N = min(⌊sectors / 4⌋, 65535). Sectors beyond cluster N−1 are not used.
- B is the number of bitmap clusters: B = ⌈N / 16384⌉, from 1 to 4. One bitmap cluster (16384 bits) covers 32 MB.
- The smallest volume contains 3 clusters (12 sectors): cluster 0, one bitmap cluster, and one data cluster.
- Cluster number 0 is never a data pointer because cluster 0 is reserved for system use. This makes zero a suitable “no cluster” value.

## 3. Superblock

Sector 0 contains the superblock, which describes the entire volume and holds the root directory entry. The allocation bitmap always begins at cluster 1, so it needs no separate starting-address field.

| Offset | Size | Field | Contents |
| --- | --- | --- | --- |
| 0 | 4 | MAGIC | Bytes $46 $53 $31 $36 (“FS16”) |
| 4 | 2 | VERSION | 1 |
| 6 | 2 | CLUSTERS | N, the number of clusters on the volume, 3…65535 |
| 8 | 2 | MAPLEN | B, the number of bitmap clusters |
| 10 | 2 | FREE | Number of free clusters |
| 12 | 2 | HINT | Cluster at which to begin searching for free space |
| 14 | 2 | FLAGS | Bit 0: DIRTY — volume mounted for writing; all other bits zero |
| 16 | 4 | SERIAL | Volume serial number, assigned at format time |
| 20 | 16 | LABEL | Volume label, up to 16 single-byte characters |
| 36 | 64 | ROOT | Root directory entry (format in Section 5) |
| 100 | 410 | — | Zeros |
| 510 | 2 | CHECKSUM | The sum of all 256 sector words modulo 2^16 must be zero |

Rules:

- The ROOT entry has an all-zero name and the DIR flag set. On a newly formatted empty volume, the root directory has size 0 and all its pointers are zero.
- FREE and HINT are hints, not authoritative allocation information. The bitmap determines which clusters are in use; the volume checker recalculates both fields. If HINT is outside the data region, allocation starts at B+1.
- Changing any superblock field requires recalculating CHECKSUM. Its value is the additive inverse of the sum of the other 255 words, modulo 2^16.
- Before mounting, the system verifies MAGIC, VERSION = 1, and CHECKSUM; checks that 4N does not exceed the disk's sector count and that B = ⌈N / 16384⌉; and requires the DIR flag in ROOT. A volume that fails any of these checks is not mounted.
- The file-system volume label is unrelated to the label in the controller's INFO block. The latter identifies the medium; the former belongs to the file system and travels with the disk image.

## 4. Allocation Bitmap

The bitmap has 16384·B bits, one for each cluster number from 0 through 16384·B − 1. A set bit means allocated; a clear bit means free.

- Cluster c corresponds to bit (c & 7) in bitmap byte (c >> 3), or equivalently to bit (c & 15) in bitmap word (c >> 4). The bitmap can therefore be accessed by byte or by word.
- Bitmap byte k is stored in sector 4 + (k >> 9), at byte offset k & 511. Each bitmap sector covers 4096 clusters.
- Bits for cluster 0, bitmap clusters 1…B, and bitmap padding positions N…16384·B − 1 must always be set. Since the padding bits are set, a bitmap-word scan need not compare each bit position against N.
- Every cluster referenced by an entry, whether as data or as an index, is allocated. This includes clusters referenced by ROOT.
- A free-space scan begins at HINT and searches while the cluster number is less than N; on reaching N, it wraps to B+1. It must not continue to the end of the bitmap: with B = 4, that end is cluster number 65536, which cannot be represented in 16 bits. Setting out-of-volume bits does **not** permit the scan to pass N; it only removes the need for a per-bit comparison with N within a bitmap word.
- The recommended scan skips words equal to $FFFF. After allocating a cluster, set HINT to that cluster's number + 1 to encourage contiguous allocation as files grow.
- On a 128 MB disk, the bitmap is 8 KB. There is no need to cache it all: holding one sector in memory is sufficient.

## 5. Directory Entry

A directory entry is 64 bytes: 32 bytes for the name, 4 for the size and flags, 4 for the modification timestamp, and 24 for addresses (10 direct pointers, one indirect pointer, and one double-indirect pointer). There are no reserved bytes.

| Offset | Size | Field | Contents |
| --- | --- | --- | --- |
| 0 | 32 | NAME | Name, 1…32 bytes (Section 7), zero-padded; NAME\[0\] = 0 marks a free entry |
| 32 | 4 | SIZE | Bits 0–26: size in bytes; bits 27–31: flags |
| 36 | 4 | MTIME | Last modification timestamp: word at +36 is time, word at +38 is date (Section 5.1) |
| 40 | 20 | DIRECT\[0…9\] | Direct pointers to file data clusters 0…9 |
| 60 | 2 | IND | Single-indirect index cluster for data clusters 10…1033 |
| 62 | 2 | DIND | Double-indirect index for data clusters starting at 1034 |

Flags in the high bits of SIZE:

| SIZE bit | Bit in word at +34 | Name | Meaning |
| --- | --- | --- | --- |
| 31 | 15 | DIR | Entry is a directory |
| 30 | 14 | RDONLY | Writing and deletion are forbidden |
| 29 | 13 | HIDDEN | Omit from ordinary directory listings |
| 28 | 12 | SYSTEM | System file |
| 27 | 11 | — | 0 |

Notes:

- On a 16-bit machine, SIZE can conveniently be read as two words. The word at +32 holds the lower 16 size bits. In the word at +34, bits 0–10 are the upper size bits (mask $07FF), and bits 11–15 are the flags (mask $F800).
- The 27-bit size field can represent up to 128 MB − 1 byte, larger than the capacity of any supported volume.
- Every byte of a free 64-byte directory entry must be zero.
- A file's MTIME changes whenever its data or size changes. A directory's MTIME changes when an entry is created, deleted, or renamed inside it. Renaming or moving a file does not change that file's own MTIME.
- Creation time is not stored.

### 5.1. Date and Time Encoding

The timestamp occupies two words. The low word stores time of day to the nearest second in 12-hour format; the high word stores the date and AM/PM indication. The value is local machine time; no time zone is stored.

Low word (time):

| Bits | Field | Range |
| --- | --- | --- |
| 12–15 | Hour | 0…11 |
| 6–11 | Minute | 0…59 |
| 0–5 | Second | 0…59 |

High word (date):

| Bits | Field | Range |
| --- | --- | --- |
| 10–15 | Year − 2025 | 0…63, representing 2025…2088 |
| 6–9 | Month | 1…12 |
| 1–5 | Day | 1…31 |
| 0 | PM | 0 for AM, 1 for PM |

Rules:

- Hour 0 represents 12 o'clock: midnight is AM, hour 0; noon is PM, hour 0; 23:59:59 is PM, 11:59:59.
- The fields are ordered from most to least significant (year, month, day, AM/PM, hour, minute, second). Timestamps can therefore be compared as unsigned 32-bit numbers: compare the high words first and, if equal, the low words.
- A value of zero in both words means “time unknown”: month 0 is invalid and cannot occur in a genuine date.
- The format's last representable date is December 31, 2088.

## 6. Addressing File Data

For a byte at file offset pos, the file-relative data-cluster index is k = pos >> 11. Because pos < 2^27, k fits in 16 bits, so the subsequent cluster calculations also use 16-bit arithmetic.

```text
k = pos >> 11                      ; file-relative cluster index
if k < 10:     c = DIRECT[k]
elif k < 1034: c = IND[k - 10]     ; IND is an index cluster
else:          j = k - 1034
               t = DIND[j >> 10]   ; second-level index cluster
               c = t[j & 1023]
s      = (pos >> 9) & 3            ; sector within the cluster
LBA_LO = (c << 2) | s
LBA_HI = c >> 14
offset = pos & 511                 ; byte within the sector
```

An index cluster holds 1024 16-bit cluster numbers. To read element i, access sector (i >> 8) of that cluster and take the word at byte offset (i & 255) · 2. Only that sector, not the entire index cluster, needs to be read.

Allocation rules:

- A file with length size has exactly C = ⌈size / 2048⌉ allocated data clusters. FS16 does not support sparse files or unallocated gaps within a file.
- Every pointer beyond the file's C data clusters (k ≥ C) must be zero, whether stored directly in its directory entry or in an index cluster.
- IND is nonzero if and only if C > 10. DIND is nonzero if and only if C > 1034; when present, its first ⌈(C − 1034) / 1024⌉ elements are in use.
- The part of a file's final cluster beyond its logical length has unspecified contents. Reads never return those bytes; if the file grows into that area, the new data overwrites them.

Indexing overhead at different file sizes:

| File size | Data clusters | Index clusters | Index reads per data sector |
| --- | --- | --- | --- |
| Up to 20 KB | 1…10 | 0 | 0 |
| Up to 2068 KB | 11…1034 | 1 | 1 |
| Up to ≈128 MB (entire volume) | Up to ≈65500 | Up to 2 + 63 = 65 | 2 |

## 7. Directories

A directory is a file with the DIR flag set. Its contents are a sequence of 64-byte directory entries, addressed in the same way as ordinary file data (Section 6).

**Size.** A directory occupies whole 2048-byte clusters, each containing 32 entries, so its size is always a multiple of 2048. An empty directory may have size zero and use no clusters. If all entries are occupied, the directory grows by one zero-filled cluster. Clusters in the middle are retained; an implementation may remove trailing clusters consisting entirely of free entries.

**Indexing overhead.** Directories use the same indexing scheme as other files. More than 10 data clusters (over 320 entries) require an IND cluster; beyond 1034 data clusters, DIND and second-level index clusters are also required (Section 6). Index clusters contain pointers, not directory entries: 21 allocated clusters including IND provide space for at most 640 entries, not 672.

**Root.** The root directory's entry is the ROOT field in the superblock. The root directory contains no entry for itself.

**Names.**

- An FS16 name is 1–32 bytes long. Bytes $20 and above are permitted except “/”, which separates path components, and “:”, which separates a Monitor volume prefix from the path. Monitor rejects names containing “:”; a file stored with such a name cannot be opened or deleted through Monitor.
- The names “.” and “..” are forbidden, as they are reserved for path traversal (see **Paths and parent traversal**).
- Name comparisons are **case-insensitive** (Section 7.1). Names within one directory must be unique under this comparison: `Readme`, `README`, and `readme` all designate the same name.
- Original casing is preserved: NAME contains the spelling specified at creation or at the most recent rename. A lookup using any letter case finds the entry.
- Directory entries have no defined ordering. A newly created entry occupies the first available slot.

**Paths and parent traversal.** FS16 stores neither “.”/“..” entries nor links to parent directories on disk. A parent link alone would not reveal the parent's data addresses: those reside in its entry in the grandparent directory. Instead, the implementation maintains a chain from the root to the current directory; “..” moves back along this chain. Moving a directory therefore does not require rewriting its contents.

Paths are divided into components by “/” and interpreted according to these rules:

- “.” is a no-op: remain in the current directory;
- “..” moves one level toward the root along the directory chain;
- repeated “/” characters are equivalent to a single separator: empty intermediate components are skipped;
- a trailing “/” is permitted and does not change the path's meaning: “/w/” denotes the same object as “/w”.

The “X:” volume prefix is not part of FS16. Its meaning is defined by the Monitor specification (Section 4.1). As far as FS16 is concerned, a path consists only of components within one volume; the colon is forbidden in names precisely to reserve it for the volume prefix.

**Entry location.** The location of a directory entry identifies its file: a 16-bit directory-cluster number and an entry index of 0…31. ROOT is represented as “cluster 0”. The implementation retains the entry location for each open file in order to update its size and cluster pointers.

### 7.1. Name Comparison

To compare names, apply fold to each byte independently, then compare the resulting byte sequences. Only the uppercase letters listed below are converted to lowercase:

| Uppercase | Lowercase | fold |
| --- | --- | --- |
| `A–Z` (`$41–$5A`) | `a–z` (`$61–$7A`) | +$20 |
| `А–Я` (`$C0–$DF`) | `а–я` (`$E0–$FF`) | +$20 |
| `Ё` (`$B3`) | `ё` (`$A3`) | $A3 |
| All other bytes | — | Unchanged |

```text
fold(b) = b + $20,  if $41 ≤ b ≤ $5A or $C0 ≤ b ≤ $DF
        = $A3,      if b = $B3
        = b         otherwise
```

- Folding does not change the number of bytes in a name. It also leaves the zero padding unchanged, so implementations can compare the complete 32-byte NAME fields.
- The Cyrillic letters `Е` and `Ё` are distinct: `ёж` and `еж` are different names. Box-drawing characters, symbols, and punctuation are compared byte-for-byte.
- This table is part of the file-system format. All implementations (Monitor, bootloader, volume checker, and host utilities) must apply exactly the same folding rules. Otherwise, one implementation could treat two names as equal while another treats them as different.

## 8. Integrity Invariants

A volume is consistent only when all the following invariants hold. The volume checker described in Section 10 checks them.

1. Every data-region cluster is either free or referenced exactly once by a directory entry or an index cluster. No two files may share a cluster.
2. A bitmap bit is set if and only if its cluster is reserved for system use, lies beyond the volume boundary, or is referenced. FREE counts only the clear bits representing clusters within the volume.
3. All nonzero cluster pointers refer to the data region, B+1 … N−1.
4. Cluster pointers match the file size under the rules of Section 6: exactly C data clusters are allocated, all later pointers are zero, and IND and DIND exist only when required.
5. A directory's size is a multiple of 2048. Each 64-byte entry must either be all zeros or contain a valid name (Section 7), unique within that directory under the case-insensitive comparison defined in Section 7.1. A name containing “:” is invalid; the volume checker reports it. Case-insensitively duplicate names are also an integrity violation; the checker renames the surplus entries (Section 10, item 6).
6. The directory hierarchy must form a tree. Every directory other than the root is represented by exactly one directory entry, and every entry must be reachable from ROOT. Together with rule 1, this rules out cycles and disconnected subtrees.

## 9. Basic Operations

No operation copies file data: extension and truncation manipulate pointers, while moving a file only changes its directory entry. The operations below specify disk write ordering (see Section 10 for why that order matters).

**Path lookup.** Split the path at “/”, applying the rules for empty components, “.”, and “..” from Section 7. For each name, search the current directory one sector (8 entries) or one cluster (32 entries) at a time for a case-insensitive NAME match (Section 7.1). Every intermediate component must name a directory.

**Reading.** Resolve each cluster pointer as described in Section 6. Consecutively numbered file clusters may be read with a single READ command, up to 255 sectors at a time, subject to buffer size.

**Appending data cluster k = C to a file:**

1. Allocate a data cluster and any new index clusters required: IND when k = 10; DIND and the first second-level index cluster when k = 1034; another second-level index cluster when (k − 1034) & 1023 = 0. If allocation fails, release anything already allocated and return a “disk full” error.
2. Mark the allocated clusters in the bitmap and write the bitmap to disk.
3. Write the data. Write each newly allocated index cluster immediately, initialized with the pointer and zeros.
4. If an existing index cluster needs a new pointer, write its modified sector.
5. Write the directory entry with updated SIZE, MTIME, and pointers.

Step 5 need not run after every cluster. It may be deferred until the end of the write operation—for example, one call that appends several clusters. In that case, the directory entry is written once with the final SIZE, MTIME, and pointers, no later than the operation's return. The order “bitmap → data → index → directory entry” still applies. If a failure occurs before the directory entry is written, it retains the old size: the new clusters are leaked but never shared (Section 10). The volume checker will clear pointers past the end of the recorded size.

**Truncation to size size'.** Let C' = ⌈size' / 2048⌉.

1. Identify every cluster to be freed: data clusters with k ≥ C', IND if C' ≤ 10, DIND if C' ≤ 1034, and second-level index clusters that will contain no remaining pointers. Save their cluster numbers in memory **before** clearing any pointers; once the on-disk pointers are cleared, there is no way to recover those numbers.
2. Write the directory entry with the updated SIZE and MTIME and zeroed pointers no longer needed (including IND and DIND, if applicable).
3. Clear unused pointers in any index clusters that remain allocated, and write those clusters. Before freeing a whole second-level index cluster, first write the DIND cluster with the reference to it cleared.
4. Clear the saved clusters' bitmap bits. Free a data cluster only after writing an index that no longer points to it; free a second-level index cluster only after writing DIND with the corresponding entry cleared; free IND and DIND only after writing the directory entry without pointers to them (step 2).

As elsewhere, the ordering ensures that a cluster is released only after its last reference has been removed.

**Creation.** First check that no existing entry has the same name under case-insensitive comparison. Use the first free entry for the new name, flags, SIZE = 0, and current MTIME; if the directory has no free entry, append a cluster before creating one. A new directory is created by the same procedure with the DIR flag set, initially without any data clusters.

**Deletion.** An RDONLY file cannot be deleted, and a directory may be deleted only if all its entries are free. Save the object's cluster pointers, zero and write its directory entry, then release its data and index clusters in the bitmap.

**Renaming and moving.** Check whether the proposed name matches an existing name in the destination directory under case-insensitive comparison, excluding the object's own entry. A case-only rename (`readme` → `README`) is therefore valid. Within one directory, only NAME changes. For a move between directories, write the object's 64-byte entry at the destination before clearing the source entry. A directory cannot be moved into one of its own descendants; the implementation checks the destination path's ancestor chain.

## 10. Write Ordering and Volume Checking

A cluster is marked allocated before the first reference to it is written, and is released only after its last reference has been removed. If an operation is interrupted, clusters may remain marked allocated without being referenced by any file, but the allocator cannot assign the same cluster to two files. Moving an object is the exception: an interruption between writing the destination entry and clearing the source entry can leave two entries referring to the same data, without losing that data.

This rule makes allocation and deallocation asymmetric:

- **Allocation** must be committed to the bitmap immediately, before the first reference to a cluster. Deferring it could leave a referenced cluster marked free after a failure, allowing that same cluster to be allocated to another file.
- **Deallocation** may be deferred: clearing freed bitmap bits may remain cached until unmount (or until cache eviction, a flush, or the next write of that bitmap sector). Delayed release is safe: a failure can at worst leak a cluster, which the volume checker can reclaim.
- **Directory-entry updates** while growing a file may be deferred to the end of the operation (Section 9, **Appending a data cluster**), but never performed before the bitmap, data, and index writes.

**Mounting for writing:**

1. Read and validate the superblock (Section 3).
2. If DIRTY = 1, the previous unmount was incomplete: check the volume before using it for writing, or mount it read-only.
3. Set DIRTY = 1, write the superblock, and issue FLUSH.

**Unmounting:** First write all dirty cached data and issue FLUSH. Then write the superblock with the current FREE and HINT values and DIRTY cleared to 0, and issue FLUSH again. During normal operation, FREE and HINT may be kept only in memory because they are hints.

**Volume check:**

1. Starting at ROOT, traverse the directory tree and build a replacement bitmap in memory (at most 8 KB), marking each referenced cluster.
2. If a cluster pointer lies outside the data region or refers to a cluster already claimed by another reference, truncate the file immediately before that cluster. If the problem occurs at the first cluster, clear the file's directory entry as a duplicate left by an interrupted move.
3. If SIZE disagrees with the cluster pointers, reduce SIZE to match the data actually allocated and clear any pointers beyond the resulting file size.
4. Replace the on-disk allocation bitmap with the reconstructed one and recalculate FREE. Clusters previously marked allocated but no longer referenced become free.
5. Report directory entries whose names contain “:” (Section 8, item 5), but do not modify them. A host-side utility can repair those names.
6. If names are identical under case-insensitive comparison (Section 8, item 5), leave the first entry in directory order unchanged. Assign each subsequent entry a unique name and report the rename. Append the smallest integer, starting with 2, that makes the new name distinct from all other entries under case-insensitive comparison. If the result would exceed 32 bytes, shorten the original name before appending the number. For example, given `readme` and `ReadMe`, the latter becomes `ReadMe2`; if `readme2` already exists, use `ReadMe3`. Such groups may exist on volumes written before revision 3. Until renamed, these extra entries are inaccessible by name: they can neither be opened nor deleted.

## 11. Limits

| Quantity | Value |
| --- | --- |
| Maximum volume | 65535 clusters = 134,215,680 bytes (128 MB − 2 KB) |
| Minimum volume | 3 clusters = 6 KB |
| Reserved area | 1 + B clusters, from 4 KB to 10 KB |
| Maximum file | Data area less its index clusters; the format can represent up to 2^27 − 1 bytes |
| File without index clusters | Up to 20 KB (10 clusters) |
| File with one index cluster | Up to 2068 KB (1034 clusters) |
| Name length | 1…32 bytes |
| Entries per directory cluster | 32 (8 per sector) |
| Entries per directory | Limited only by available disk space; over 320 entries also requires an index cluster (Section 7) |
| Minimum storage for a nonempty file | 2 KB of data + a 64-byte entry |
| Empty file or directory | Only a 64-byte entry |

Not supported in version 1: creation timestamps, hard links, sparse files, or a backup superblock.

## 12. Revision History

| Revision | Date | Changes |
| --- | --- | --- |
| 1 | Oct 10, 2026 | Initial specification. |
| 2 | Oct 16, 2026 | Changes based on prototype A feedback (listed below). |
| 3 | Oct 19, 2026 | Case-insensitive names (listed below). |

Changes in revision 3:

1. **Letter case in names** (Sections 7, 7.1, 8, 9, 10). Names are compared case-insensitively using the fold table (Latin letters, `А–Я`, `Ё`). Two names that compare equal cannot coexist in one directory. Original letter case is preserved on disk. Case-only renames are permitted. The volume checker renames surplus entries in duplicate groups by appending 2, 3, and so forth (Section 10, item 6).
2. **New Section 7.1:** name-comparison rules and character-folding table.
3. **No change to the on-disk format;** VERSION remains 1. Revision 2 volumes mount unchanged. If they contain names differing only in letter case, the volume checker renames the extra entries.

Changes in revision 2:

1. **Colons in names** (Sections 7, 8, 10). Names containing “:” are invalid. The volume checker reports them but does not modify them.
2. **Paths** (Sections 7, 9). “.” is a no-op; repeated “/” characters are equivalent to a single separator; a trailing “/” is allowed and does not change the path's meaning. The “X:” volume prefix is outside the FS16 specification.
3. **Truncation ordering** (Section 9). Cluster numbers to be freed are saved before their pointers are cleared; a second-level index cluster is freed after writing DIND with its reference cleared, and IND and DIND are freed after writing a directory entry that no longer references them.
4. **Bitmap scan limit** (Section 4). Free-space searches stop at N, not at the end of the bitmap. Omitting the bounds comparison refers only to checking cluster numbers against N within a bitmap word.
5. **Deferred directory-entry write** (Sections 9, 10). A directory entry can be written once at the end of an operation, provided the order “bitmap → data → index → directory entry” is preserved.
6. **Deferred deallocation updates** (Section 10). Clearing bits for freed clusters may be delayed until unmount; setting bits for newly allocated clusters must not be delayed.
7. **IND overhead for directories** (Sections 7, 11). A directory holding more than 320 entries requires an additional index cluster.
